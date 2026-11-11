#include "sbv-bulk-ui.h"
#include "sbv-window.h"
#include "../backend/sbv-dn.h"

#include <adwaita.h>
#include <string.h>

/* How many names a confirmation lists before "and N more". */
#define PREVIEW_MAX 8
/* How many failures a report lists before "And N more". */
#define REPORT_MAX 15

/* Lists up to `max` names, then counts the rest. */
char *
sbv_bulk_ui_preview (const char * const *names, guint max)
{
  guint    n   = names ? g_strv_length ((char **) names) : 0;
  GString *out = g_string_new (NULL);
  guint    shown = MIN (n, max);
  for (guint i = 0; i < shown; i++) {
    if (i > 0) g_string_append (out, i + 1 == shown && shown == n ? " and " : ", ");
    g_string_append (out, names[i]);
  }
  if (n > shown)
    g_string_append_printf (out, " and %u more", n - shown);
  return g_string_free (out, FALSE);
}

/* The top-level window that dialogs should sit on. */
static GtkWindow *
parent_window (GtkWidget *from)
{
  GtkRoot *root = from ? gtk_widget_get_root (from) : NULL;
  return GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL;
}

/* ── Confirm ────────────────────────────────────────────────────────────── */

typedef struct {
  SbvBulkAcceptFn fn;
  gpointer        data;
} ConfirmCtx;

/* Runs the accept callback when the user picks "accept". */
static void
on_confirm_response (AdwMessageDialog *dlg, const char *response, gpointer user_data)
{
  (void) dlg;
  ConfirmCtx *ctx = user_data;
  if (g_strcmp0 (response, "accept") == 0 && ctx->fn)
    ctx->fn (ctx->data);
}

void
sbv_bulk_ui_confirm (GtkWidget          *from,
                     const char         *heading,
                     const char         *accept_label,
                     gboolean            destructive,
                     const char * const *names,
                     SbvBulkAcceptFn     on_accept,
                     gpointer            user_data)
{
  char      *body = sbv_bulk_ui_preview (names, PREVIEW_MAX);
  GtkWidget *dlg  = adw_message_dialog_new (parent_window (from), heading, body);
  g_free (body);

  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "cancel", "_Cancel");
  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "accept", accept_label);
  adw_message_dialog_set_response_appearance (ADW_MESSAGE_DIALOG (dlg), "accept",
      destructive ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
  adw_message_dialog_set_default_response (ADW_MESSAGE_DIALOG (dlg), "cancel");
  adw_message_dialog_set_close_response   (ADW_MESSAGE_DIALOG (dlg), "cancel");

  ConfirmCtx *ctx = g_new0 (ConfirmCtx, 1);
  ctx->fn   = on_accept;
  ctx->data = user_data;
  g_signal_connect_data (dlg, "response", G_CALLBACK (on_confirm_response),
                         ctx, (GClosureNotify) (void (*) (void)) g_free, 0);
  gtk_window_present (GTK_WINDOW (dlg));
}

/* ── Password ───────────────────────────────────────────────────────────── */

typedef struct {
  SbvBulkPasswordFn fn;
  gpointer          data;
  GtkWidget        *dialog;
  GtkWidget        *pw1;
  GtkWidget        *pw2;
  GtkWidget        *must_change;
  GtkWidget        *hint;
} PasswordCtx;

/* Enables Reset only when both entries match and aren't empty, and says
 * why otherwise. */
static void
on_password_changed (GtkEditable *editable, gpointer user_data)
{
  (void) editable;
  PasswordCtx *ctx = user_data;
  const char  *a   = gtk_editable_get_text (GTK_EDITABLE (ctx->pw1));
  const char  *b   = gtk_editable_get_text (GTK_EDITABLE (ctx->pw2));
  gboolean     ok  = *a && g_strcmp0 (a, b) == 0;

  gtk_label_set_text (GTK_LABEL (ctx->hint),
                      !*a ? "" : (*b && !ok) ? "The passwords don't match." : "");
  adw_message_dialog_set_response_enabled (ADW_MESSAGE_DIALOG (ctx->dialog),
                                           "accept", ok);
}

/* Hands the password to the caller on accept. */
static void
on_password_response (AdwMessageDialog *dlg, const char *response, gpointer user_data)
{
  (void) dlg;
  PasswordCtx *ctx = user_data;
  if (g_strcmp0 (response, "accept") != 0 || !ctx->fn) return;
  ctx->fn (gtk_editable_get_text (GTK_EDITABLE (ctx->pw1)),
           gtk_check_button_get_active (GTK_CHECK_BUTTON (ctx->must_change)),
           ctx->data);
}

void
sbv_bulk_ui_ask_password (GtkWidget          *from,
                          const char * const *names,
                          SbvBulkPasswordFn   on_accept,
                          gpointer            user_data)
{
  guint n       = names ? g_strv_length ((char **) names) : 0;
  char *heading = g_strdup_printf ("Reset the password for %u users?", n);
  char *preview = sbv_bulk_ui_preview (names, PREVIEW_MAX);
  char *body    = g_strdup_printf ("%s\n\nEvery account gets the same new password.",
                                   preview);
  GtkWidget *dlg = adw_message_dialog_new (parent_window (from), heading, body);
  g_free (heading);
  g_free (preview);
  g_free (body);

  PasswordCtx *ctx = g_new0 (PasswordCtx, 1);
  ctx->fn     = on_accept;
  ctx->data   = user_data;
  ctx->dialog = dlg;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  ctx->pw1 = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (ctx->pw1), TRUE);
  g_object_set (ctx->pw1, "placeholder-text", "New password", NULL);
  ctx->pw2 = gtk_password_entry_new ();
  g_object_set (ctx->pw2, "placeholder-text", "Confirm password", NULL);
  g_signal_connect (ctx->pw1, "changed", G_CALLBACK (on_password_changed), ctx);
  g_signal_connect (ctx->pw2, "changed", G_CALLBACK (on_password_changed), ctx);
  gtk_box_append (GTK_BOX (box), ctx->pw1);
  gtk_box_append (GTK_BOX (box), ctx->pw2);

  ctx->must_change = gtk_check_button_new_with_label ("User must change password at next logon");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (ctx->must_change), TRUE);
  gtk_box_append (GTK_BOX (box), ctx->must_change);

  ctx->hint = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (ctx->hint), 0);
  gtk_widget_add_css_class (ctx->hint, "error");
  gtk_box_append (GTK_BOX (box), ctx->hint);

  adw_message_dialog_set_extra_child (ADW_MESSAGE_DIALOG (dlg), box);
  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "cancel", "_Cancel");
  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "accept", "_Reset Passwords");
  adw_message_dialog_set_response_appearance (ADW_MESSAGE_DIALOG (dlg), "accept",
                                              ADW_RESPONSE_DESTRUCTIVE);
  adw_message_dialog_set_response_enabled (ADW_MESSAGE_DIALOG (dlg), "accept", FALSE);
  adw_message_dialog_set_default_response (ADW_MESSAGE_DIALOG (dlg), "cancel");
  adw_message_dialog_set_close_response   (ADW_MESSAGE_DIALOG (dlg), "cancel");

  g_signal_connect_data (dlg, "response", G_CALLBACK (on_password_response),
                         ctx, (GClosureNotify) (void (*) (void)) g_free, 0);
  gtk_window_present (GTK_WINDOW (dlg));
  gtk_widget_grab_focus (ctx->pw1);
}

/* ── Report ─────────────────────────────────────────────────────────────── */

void
sbv_bulk_ui_report (GtkWidget *from, const char *done_text, SbvBulkResult *result)
{
  GtkWindow *win = parent_window (from);

  if (result->failures->len == 0) {
    if (SBV_IS_WINDOW (win)) sbv_window_show_toast (SBV_WINDOW (win), done_text);
    return;
  }

  char *heading = g_strdup_printf ("%u of %u failed",
                                   result->failures->len, result->total);
  GString *body = g_string_new (NULL);
  if (result->succeeded > 0)
    g_string_append_printf (body, "The other %u worked.\n\n", result->succeeded);
  guint shown = MIN (result->failures->len, REPORT_MAX);
  for (guint i = 0; i < shown; i++) {
    SbvBulkFailure *f    = g_ptr_array_index (result->failures, i);
    char           *name = sbv_dn_rdn_value (f->dn);
    g_string_append_printf (body, "%s: %s\n", name ? name : f->dn, f->message);
    g_free (name);
  }
  if (result->failures->len > shown)
    g_string_append_printf (body, "And %u more.\n", result->failures->len - shown);

  GtkWidget *dlg = adw_message_dialog_new (win, heading, NULL);
  adw_message_dialog_set_body (ADW_MESSAGE_DIALOG (dlg), g_strchomp (body->str));
  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "close", "_Close");
  gtk_window_present (GTK_WINDOW (dlg));
  g_free (heading);
  g_string_free (body, TRUE);
}

/* ── Selection list ─────────────────────────────────────────────────────── */

GPtrArray *
sbv_bulk_ui_visible_selection (GtkListBox *source)
{
  GPtrArray *out  = g_ptr_array_new_with_free_func (g_object_unref);
  GList     *rows = gtk_list_box_get_selected_rows (source);
  for (GList *l = rows; l; l = l->next)
    if (gtk_widget_get_child_visible (GTK_WIDGET (l->data)))
      g_ptr_array_add (out, g_object_ref (l->data));
  g_list_free (rows);
  return out;
}

/* Remove button on one entry: deselect that row in the source list. */
static void
on_deselect_clicked (GtkButton *btn, gpointer user_data)
{
  GtkListBox    *source = GTK_LIST_BOX (user_data);
  GtkListBoxRow *row    = g_object_get_data (G_OBJECT (btn), "sbv-row");
  if (row && gtk_widget_get_parent (GTK_WIDGET (row)) == GTK_WIDGET (source))
    gtk_list_box_unselect_row (source, row);
}

/* Clear Selection button. */
static void
on_clear_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  gtk_list_box_unselect_all (GTK_LIST_BOX (user_data));
}

/* Ctrl+Click on a selected row: unselect it and stop the list's own
 * handling, which would otherwise keep the row selected. */
static void
on_list_pressed (GtkGestureClick *gesture, int n_press, double x, double y,
                 gpointer user_data)
{
  (void) n_press; (void) x;
  GtkListBox     *source = GTK_LIST_BOX (user_data);
  GdkModifierType state  = gtk_event_controller_get_current_event_state (
                             GTK_EVENT_CONTROLLER (gesture));
  if ((state & GDK_CONTROL_MASK) == 0 || (state & GDK_SHIFT_MASK) != 0)
    return;

  GtkListBoxRow *row = gtk_list_box_get_row_at_y (source, (int) y);
  if (!row || !gtk_list_box_row_is_selected (row))
    return;

  gtk_list_box_unselect_row (source, row);
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

void
sbv_bulk_ui_enable_ctrl_deselect (GtkListBox *source)
{
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_list_pressed), source);
  gtk_widget_add_controller (GTK_WIDGET (source), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
sbv_bulk_ui_selection_new (GtkListBox *source)
{
  sbv_bulk_ui_enable_ctrl_deselect (source);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  g_object_set_data (G_OBJECT (box), "sbv-source", source);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 260);
  gtk_widget_set_size_request (scroll, 320, -1);

  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
  g_object_set_data (G_OBJECT (box), "sbv-list", list);
  gtk_box_append (GTK_BOX (box), scroll);

  GtkWidget *clear = gtk_button_new_with_label ("Clear Selection");
  gtk_widget_add_css_class (clear, "flat");
  gtk_widget_set_halign (clear, GTK_ALIGN_CENTER);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_clear_clicked), source);
  gtk_box_append (GTK_BOX (box), clear);
  return box;
}

void
sbv_bulk_ui_selection_update (GtkWidget *box, SbvBulkRowNameFn name_fn)
{
  GtkListBox *source = g_object_get_data (G_OBJECT (box), "sbv-source");
  GtkListBox *list   = g_object_get_data (G_OBJECT (box), "sbv-list");

  GtkListBoxRow *old;
  while ((old = gtk_list_box_get_row_at_index (list, 0)))
    gtk_list_box_remove (list, GTK_WIDGET (old));

  GPtrArray *rows = sbv_bulk_ui_visible_selection (source);
  for (guint i = 0; i < rows->len; i++) {
    GtkListBoxRow *src  = g_ptr_array_index (rows, i);
    char          *name = name_fn (src);

    GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top    (line, 4);
    gtk_widget_set_margin_bottom (line, 4);
    gtk_widget_set_margin_start  (line, 12);
    gtk_widget_set_margin_end    (line, 6);

    GtkWidget *lbl = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (lbl, TRUE);
    gtk_box_append (GTK_BOX (line), lbl);

    GtkWidget *rm = gtk_button_new_from_icon_name ("list-remove-symbolic");
    gtk_widget_add_css_class (rm, "flat");
    gtk_widget_set_tooltip_text (rm, "Remove from the selection");
    g_object_set_data_full (G_OBJECT (rm), "sbv-row", g_object_ref (src), g_object_unref);
    g_signal_connect (rm, "clicked", G_CALLBACK (on_deselect_clicked), source);
    gtk_box_append (GTK_BOX (line), rm);

    gtk_list_box_append (list, line);
    g_free (name);
  }
  g_ptr_array_unref (rows);
}
