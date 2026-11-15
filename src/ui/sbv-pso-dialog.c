#include "sbv-pso-dialog.h"
#include "sbv-policy-form.h"
#include "../backend/sbv-pso-backend.h"
#include "../backend/sbv-users-backend.h"
#include "../backend/sbv-groups-backend.h"
#include "../backend/sbv-dn.h"
#include "../model/sbv-user.h"
#include "../model/sbv-group.h"
#include "../model/sbv-search.h"

#include <adwaita.h>
#include <string.h>

/* groupType bits: a global group, and a security group. */
#define GROUP_TYPE_GLOBAL   0x00000002
#define GROUP_TYPE_SECURITY 0x80000000u

/* State for one open dialog. Freed when the window goes. */
typedef struct {
  GtkWidget     *win;
  SbvConnection *conn;
  SbvPso        *original;     /* as passed in, with or without a DN */
  GPtrArray     *applies;      /* char* DNs shown in the list */
  gboolean       busy;

  GtkWidget *name_row;
  GtkWidget *desc_row;
  GtkWidget *precedence;
  GtkWidget *reversible;
  GtkWidget *form;
  GtkWidget *applies_list;
  GtkWidget *error_label;
  GtkWidget *spinner;
  GtkWidget *save_btn;
  GtkWidget *cancel_btn;
  GtkWidget *delete_btn;

  SbvPsoDoneFn   done;
  gpointer       done_data;
  GDestroyNotify done_free;
} PsoDlg;

/* Frees the dialog state. */
static void
pso_dlg_free (PsoDlg *d)
{
  if (d->done_free) d->done_free (d->done_data);
  g_clear_object (&d->conn);
  sbv_pso_free (d->original);
  g_ptr_array_unref (d->applies);
  g_free (d);
}

/* Shows `msg` under the form, or hides the label when NULL. */
static void
show_error (PsoDlg *d, const char *msg)
{
  gtk_label_set_text (GTK_LABEL (d->error_label), msg ? msg : "");
  gtk_widget_set_visible (d->error_label, msg != NULL);
}

/* Locks the dialog while a request runs. */
static void
set_busy (PsoDlg *d, gboolean busy)
{
  d->busy = busy;
  gtk_widget_set_visible (d->spinner, busy);
  if (busy) gtk_spinner_start (GTK_SPINNER (d->spinner));
  else      gtk_spinner_stop  (GTK_SPINNER (d->spinner));
  gtk_widget_set_sensitive (d->save_btn,   !busy);
  gtk_widget_set_sensitive (d->cancel_btn, !busy);
  if (d->delete_btn) gtk_widget_set_sensitive (d->delete_btn, !busy);
}

/* Keeps the window open while a request runs. */
static gboolean
on_close_request (GtkWindow *win, gpointer user_data)
{
  (void) win;
  return ((PsoDlg *) user_data)->busy;
}

/* Runs the caller's callback, then closes the dialog. */
static void
finish (PsoDlg *d, const char *message)
{
  set_busy (d, FALSE);
  if (d->done) d->done (message, d->done_data);
  gtk_window_destroy (GTK_WINDOW (d->win));
}

/* ── Applies-to list ────────────────────────────────────────────────────── */

static void rebuild_applies (PsoDlg *d);

/* Remove button on an applies-to row. */
static void
on_applies_remove (GtkButton *btn, gpointer user_data)
{
  PsoDlg     *d  = user_data;
  const char *dn = g_object_get_data (G_OBJECT (btn), "dn");
  for (guint i = 0; i < d->applies->len; i++) {
    if (sbv_dn_equal (g_ptr_array_index (d->applies, i), dn)) {
      g_ptr_array_remove_index (d->applies, i);
      break;
    }
  }
  rebuild_applies (d);
}

/* Refills the applies-to list from `d->applies`. */
static void
rebuild_applies (PsoDlg *d)
{
  GtkListBoxRow *old;
  while ((old = gtk_list_box_get_row_at_index (GTK_LIST_BOX (d->applies_list), 0)))
    gtk_list_box_remove (GTK_LIST_BOX (d->applies_list), GTK_WIDGET (old));

  for (guint i = 0; i < d->applies->len; i++) {
    const char *dn   = g_ptr_array_index (d->applies, i);
    char       *name = sbv_dn_rdn_value (dn);
    char       *esc  = g_markup_escape_text (name ? name : dn, -1);
    char       *sub  = g_markup_escape_text (dn, -1);

    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), esc);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
    adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 1);
    gtk_widget_set_tooltip_text (row, dn);

    GtkWidget *rm = gtk_button_new_from_icon_name ("list-remove-symbolic");
    gtk_widget_add_css_class (rm, "flat");
    gtk_widget_set_valign (rm, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (rm, "Remove");
    g_object_set_data_full (G_OBJECT (rm), "dn", g_strdup (dn), g_free);
    g_signal_connect (rm, "clicked", G_CALLBACK (on_applies_remove), d);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), rm);

    gtk_list_box_append (GTK_LIST_BOX (d->applies_list), row);
    g_free (name);
    g_free (esc);
    g_free (sub);
  }
}

/* Adds `dn` to the list unless it is already there. */
static void
add_applies (PsoDlg *d, const char *dn)
{
  for (guint i = 0; i < d->applies->len; i++)
    if (sbv_dn_equal (g_ptr_array_index (d->applies, i), dn)) return;
  g_ptr_array_add (d->applies, g_strdup (dn));
  rebuild_applies (d);
}

/* ── Picker for users and groups ────────────────────────────────────────── */

/* State for the picker window. Shared by its two list loads. */
typedef struct {
  GtkWidget *dialog;        /* the PSO dialog window; weak */
  GtkWidget *win;           /* the picker window; weak */
  GtkWidget *list_box;
  GtkWidget *add_btn;
  GtkWidget *spinner;
  GtkWidget *error_label;
  char      *filter;
  int        pending;       /* list loads still running */
} Picker;

/* Clears the picker state when its last reference goes. */
static void
picker_clear (gpointer data)
{
  Picker *p = data;
  if (p->dialog) g_object_remove_weak_pointer (G_OBJECT (p->dialog), (gpointer *) &p->dialog);
  if (p->win)    g_object_remove_weak_pointer (G_OBJECT (p->win),    (gpointer *) &p->win);
  g_free (p->filter);
}

/* Drops one reference to the picker state. */
static void
picker_unref (Picker *p)
{
  g_atomic_rc_box_release_full (p, picker_clear);
}

/* Shows a row when it matches the search text. */
static gboolean
picker_filter (GtkListBoxRow *row, gpointer user_data)
{
  Picker     *p = user_data;
  const char *fields[] = {
    g_object_get_data (G_OBJECT (row), "name"),
    g_object_get_data (G_OBJECT (row), "sam"),
  };
  return sbv_search_matches (p->filter, fields, G_N_ELEMENTS (fields));
}

/* Re-runs the filter on each search edit. */
static void
on_picker_search (GtkSearchEntry *entry, gpointer user_data)
{
  Picker *p = user_data;
  g_free (p->filter);
  p->filter = sbv_search_fold (gtk_editable_get_text (GTK_EDITABLE (entry)));
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (p->list_box));
}

/* Enables Add while a row is selected. Bound to the button's lifetime. */
static void
on_picker_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  gtk_widget_set_sensitive (GTK_WIDGET (user_data), row != NULL);
}

/* Sorts rows by kind (users first), then name. */
static int
picker_sort (GtkListBoxRow *a, GtkListBoxRow *b, gpointer user_data)
{
  (void) user_data;
  int ka = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (a), "kind"));
  int kb = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "kind"));
  if (ka != kb) return ka - kb;
  const char *na = g_object_get_data (G_OBJECT (a), "name");
  const char *nb = g_object_get_data (G_OBJECT (b), "name");
  return g_utf8_collate (na ? na : "", nb ? nb : "");
}

/* Appends one user or group row to the picker. */
static void
picker_add_row (Picker *p, int kind, const char *dn, const char *name,
                const char *sam)
{
  if (!dn) return;
  if (!name || !*name) name = sam ? sam : dn;

  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "dn",   g_strdup (dn),   g_free);
  g_object_set_data_full (G_OBJECT (row), "name", g_strdup (name), g_free);
  g_object_set_data_full (G_OBJECT (row), "sam",  g_strdup (sam),  g_free);
  g_object_set_data (G_OBJECT (row), "kind", GINT_TO_POINTER (kind));

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *icon = gtk_image_new_from_icon_name (kind == 0 ? "avatar-default-symbolic"
                                                            : "system-users-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *lbl = gtk_label_new (name);
  gtk_label_set_xalign (GTK_LABEL (lbl), 0);
  gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (lbl, TRUE);
  gtk_box_append (GTK_BOX (box), lbl);

  if (sam) {
    GtkWidget *sl = gtk_label_new (sam);
    gtk_widget_add_css_class (sl, "dim-label");
    gtk_box_append (GTK_BOX (box), sl);
  }
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  gtk_list_box_append (GTK_LIST_BOX (p->list_box), row);
}

/* Hides the spinner once both lists are in. */
static void
picker_load_done (Picker *p)
{
  if (--p->pending == 0) gtk_widget_set_visible (p->spinner, FALSE);
}

/* Shows a load error under the search box. */
static void
picker_show_error (Picker *p, const char *msg)
{
  gtk_label_set_text (GTK_LABEL (p->error_label), msg);
  gtk_widget_set_visible (p->error_label, TRUE);
}

/* Users list completion. */
static void
on_picker_users (GObject *source, GAsyncResult *result, gpointer user_data)
{
  Picker     *p     = user_data;
  GError     *err   = NULL;
  GListStore *store = sbv_users_list_finish (SBV_CONNECTION (source), result, &err);
  if (p->win) {
    picker_load_done (p);
    if (!store) {
      picker_show_error (p, err->message);
    } else {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
      for (guint i = 0; i < n; i++) {
        SbvUser    *u    = g_list_model_get_item (G_LIST_MODEL (store), i);
        const char *name = sbv_user_get_display_name (u);
        if (!name || !*name) name = sbv_user_get_cn (u);
        picker_add_row (p, 0, sbv_user_get_dn (u), name, sbv_user_get_sam (u));
        g_object_unref (u);
      }
    }
  }
  g_clear_error (&err);
  if (store) g_object_unref (store);
  picker_unref (p);
}

/* Groups list completion. Keeps global security groups only, since a
 * policy can't apply to other kinds. */
static void
on_picker_groups (GObject *source, GAsyncResult *result, gpointer user_data)
{
  Picker     *p     = user_data;
  GError     *err   = NULL;
  GListStore *store = sbv_groups_list_finish (SBV_CONNECTION (source), result, &err);
  if (p->win) {
    picker_load_done (p);
    if (!store) {
      picker_show_error (p, err->message);
    } else {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
      for (guint i = 0; i < n; i++) {
        SbvGroup *g  = g_list_model_get_item (G_LIST_MODEL (store), i);
        guint32   gt = (guint32) sbv_group_get_group_type (g);
        if ((gt & GROUP_TYPE_GLOBAL) && (gt & GROUP_TYPE_SECURITY)) {
          char *cn = sbv_dn_rdn_value (sbv_group_get_dn (g));
          picker_add_row (p, 1, sbv_group_get_dn (g), cn, sbv_group_get_sam (g));
          g_free (cn);
        }
        g_object_unref (g);
      }
    }
  }
  g_clear_error (&err);
  if (store) g_object_unref (store);
  picker_unref (p);
}

/* Add: put the chosen account in the dialog's list and close. */
static void
on_picker_add (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  Picker        *p   = user_data;
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (p->list_box));
  if (!row) return;
  if (p->dialog) {
    PsoDlg *d = g_object_get_data (G_OBJECT (p->dialog), "pso-dlg");
    if (d) add_applies (d, g_object_get_data (G_OBJECT (row), "dn"));
  }
  gtk_window_destroy (GTK_WINDOW (p->win));
}

/* Row activation adds it, like pressing Add. */
static void
on_picker_activated (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  (void) row;
  on_picker_add (NULL, user_data);
}

/* Picker window "destroy": drop the window's reference. */
static void
on_picker_destroy (GtkWidget *w, gpointer user_data)
{
  (void) w;
  picker_unref (user_data);
}

/* Opens the user and group picker over the dialog. */
static void
show_picker (PsoDlg *d)
{
  Picker *p = g_atomic_rc_box_new0 (Picker);

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Apply To");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (d->win));
  gtk_window_set_destroy_with_parent (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 440, 520);
  p->win    = win;
  p->dialog = d->win;
  g_object_add_weak_pointer (G_OBJECT (p->win),    (gpointer *) &p->win);
  g_object_add_weak_pointer (G_OBJECT (p->dialog), (gpointer *) &p->dialog);

  GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label ("Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (gtk_window_destroy), win);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  p->add_btn = gtk_button_new_with_label ("Add");
  gtk_widget_add_css_class (p->add_btn, "suggested-action");
  gtk_widget_set_sensitive (p->add_btn, FALSE);
  g_signal_connect (p->add_btn, "clicked", G_CALLBACK (on_picker_add), p);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), p->add_btn);
  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *hint = gtk_label_new ("Users and global security groups");
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_widget_set_margin_top   (hint, 8);
  gtk_widget_set_margin_start (hint, 8);
  gtk_box_append (GTK_BOX (vbox), hint);

  GtkWidget *search = gtk_search_entry_new ();
  gtk_widget_set_margin_top    (search, 8);
  gtk_widget_set_margin_bottom (search, 4);
  gtk_widget_set_margin_start  (search, 8);
  gtk_widget_set_margin_end    (search, 8);
  g_signal_connect (search, "search-changed", G_CALLBACK (on_picker_search), p);
  gtk_box_append (GTK_BOX (vbox), search);

  p->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (p->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (p->error_label), 0);
  gtk_widget_add_css_class (p->error_label, "error");
  gtk_widget_set_margin_start (p->error_label, 8);
  gtk_widget_set_visible (p->error_label, FALSE);
  gtk_box_append (GTK_BOX (vbox), p->error_label);

  p->spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (p->spinner));
  gtk_widget_set_margin_top (p->spinner, 16);
  gtk_widget_set_halign (p->spinner, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (vbox), p->spinner);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  p->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (p->list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (p->list_box), picker_filter, p, NULL);
  gtk_list_box_set_sort_func (GTK_LIST_BOX (p->list_box), picker_sort, NULL, NULL);
  gtk_widget_add_css_class (p->list_box, "boxed-list");
  gtk_widget_set_margin_start  (p->list_box, 8);
  gtk_widget_set_margin_end    (p->list_box, 8);
  gtk_widget_set_margin_bottom (p->list_box, 8);
  g_signal_connect_object (p->list_box, "row-selected",
                           G_CALLBACK (on_picker_selected), p->add_btn, 0);
  g_signal_connect (p->list_box, "row-activated", G_CALLBACK (on_picker_activated), p);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), p->list_box);
  gtk_box_append (GTK_BOX (vbox), scroll);

  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect (win, "destroy", G_CALLBACK (on_picker_destroy), p);
  gtk_window_present (GTK_WINDOW (win));

  p->pending = 2;
  sbv_users_list_async  (d->conn, NULL, on_picker_users,  g_atomic_rc_box_acquire (p));
  sbv_groups_list_async (d->conn, NULL, on_picker_groups, g_atomic_rc_box_acquire (p));
}

/* "Add…" under the applies-to list. */
static void
on_add_applies_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  show_picker (user_data);
}

/* ── Save and delete ────────────────────────────────────────────────────── */

/* Reads the dialog into a new policy. Caller frees. */
static SbvPso *
dialog_pso (PsoDlg *d)
{
  SbvPso *p = sbv_pso_copy (d->original);
  g_free (p->name);
  p->name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (d->name_row))));
  g_free (p->description);
  p->description = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (d->desc_row))));
  p->precedence  = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (d->precedence));
  p->reversible  = gtk_switch_get_active (GTK_SWITCH (d->reversible));
  p->policy      = sbv_policy_form_get (SBV_POLICY_FORM (d->form));

  g_strfreev (p->applies_to);
  p->applies_to = g_new0 (char *, d->applies->len + 1);
  for (guint i = 0; i < d->applies->len; i++)
    p->applies_to[i] = g_strdup (g_ptr_array_index (d->applies, i));
  return p;
}

/* Create completion. */
static void
on_created (GObject *source, GAsyncResult *result, gpointer user_data)
{
  PsoDlg *d   = user_data;
  GError *err = NULL;
  char   *dn  = sbv_pso_create_finish (SBV_CONNECTION (source), result, &err);
  if (!dn) {
    set_busy (d, FALSE);
    show_error (d, err->message);
    g_error_free (err);
    return;
  }
  g_free (dn);
  finish (d, "Password policy created");
}

/* Update completion. */
static void
on_updated (GObject *source, GAsyncResult *result, gpointer user_data)
{
  PsoDlg *d   = user_data;
  GError *err = NULL;
  char   *dn  = sbv_pso_update_finish (SBV_CONNECTION (source), result, &err);
  if (!dn) {
    set_busy (d, FALSE);
    show_error (d, err->message);
    g_error_free (err);
    return;
  }
  g_free (dn);
  finish (d, "Password policy saved");
}

/* Save: check the values, then create or update. */
static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  PsoDlg *d   = user_data;
  SbvPso *p   = dialog_pso (d);
  char   *msg = sbv_pso_check (p);
  if (msg) {
    show_error (d, msg);
    g_free (msg);
    sbv_pso_free (p);
    return;
  }
  show_error (d, NULL);
  set_busy (d, TRUE);
  if (d->original->dn)
    sbv_pso_update_async (d->conn, d->original, p, NULL, on_updated, d);
  else
    sbv_pso_create_async (d->conn, p, NULL, on_created, d);
  sbv_pso_free (p);
}

/* Delete completion. */
static void
on_deleted (GObject *source, GAsyncResult *result, gpointer user_data)
{
  PsoDlg *d   = user_data;
  GError *err = NULL;
  if (!sbv_pso_delete_finish (SBV_CONNECTION (source), result, &err)) {
    set_busy (d, FALSE);
    show_error (d, err->message);
    g_error_free (err);
    return;
  }
  finish (d, "Password policy deleted");
}

/* Confirmation reply for Delete. */
static void
on_delete_response (AdwMessageDialog *dlg, const char *response, gpointer user_data)
{
  (void) dlg;
  PsoDlg *d = user_data;
  if (g_strcmp0 (response, "delete") != 0) return;
  show_error (d, NULL);
  set_busy (d, TRUE);
  sbv_pso_delete_async (d->conn, d->original->dn, NULL, on_deleted, d);
}

/* Delete: ask first. */
static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  PsoDlg    *d    = user_data;
  char      *body = g_strdup_printf (
    "Accounts under \"%s\" will fall back to the next policy, or the domain policy.",
    d->original->name ? d->original->name : "");
  GtkWidget *dlg  = adw_message_dialog_new (GTK_WINDOW (d->win),
                                            "Delete Password Policy?", body);
  g_free (body);
  adw_message_dialog_add_responses (ADW_MESSAGE_DIALOG (dlg),
                                    "cancel", "Cancel", "delete", "Delete", NULL);
  adw_message_dialog_set_response_appearance (ADW_MESSAGE_DIALOG (dlg), "delete",
                                              ADW_RESPONSE_DESTRUCTIVE);
  adw_message_dialog_set_default_response (ADW_MESSAGE_DIALOG (dlg), "cancel");
  adw_message_dialog_set_close_response (ADW_MESSAGE_DIALOG (dlg), "cancel");
  g_signal_connect (dlg, "response", G_CALLBACK (on_delete_response), d);
  gtk_window_present (GTK_WINDOW (dlg));
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

/* Builds the name, description, precedence and reversible rows. */
static GtkWidget *
build_general (PsoDlg *d)
{
  GtkWidget *g = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g), "General");

  d->name_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (d->name_row), "Name");
  gtk_editable_set_text (GTK_EDITABLE (d->name_row),
                         d->original->name ? d->original->name : "");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->name_row);

  d->desc_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (d->desc_row), "Description");
  gtk_editable_set_text (GTK_EDITABLE (d->desc_row),
                         d->original->description ? d->original->description : "");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->desc_row);

  GtkWidget *prow = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (prow), "Precedence");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (prow),
    "When several policies apply, the lowest number wins");
  d->precedence = gtk_spin_button_new_with_range (1, 999999, 1);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (d->precedence), 0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->precedence),
                             MAX (1, d->original->precedence));
  gtk_widget_set_valign (d->precedence, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ADW_ACTION_ROW (prow), d->precedence);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), prow);

  GtkWidget *rrow = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rrow),
                                 "Store passwords with reversible encryption");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (rrow),
    "Only for old protocols that need the plain password. Leave off.");
  d->reversible = gtk_switch_new ();
  gtk_switch_set_active (GTK_SWITCH (d->reversible), d->original->reversible);
  gtk_widget_set_valign (d->reversible, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ADW_ACTION_ROW (rrow), d->reversible);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (rrow), d->reversible);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), rrow);
  return g;
}

/* Builds the applies-to group with its Add button. */
static GtkWidget *
build_applies (PsoDlg *d)
{
  GtkWidget *g = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g), "Applies To");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (g),
    "Users and global security groups this policy covers");

  GtkWidget *add = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (add, "flat");
  gtk_widget_set_valign (add, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (add, "Add a user or group");
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_applies_clicked), d);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (g), add);

  d->applies_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (d->applies_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (d->applies_list, "boxed-list");
  GtkWidget *empty = gtk_label_new ("Applies to nobody yet");
  gtk_widget_add_css_class (empty, "dim-label");
  gtk_widget_set_margin_top    (empty, 12);
  gtk_widget_set_margin_bottom (empty, 12);
  gtk_list_box_set_placeholder (GTK_LIST_BOX (d->applies_list), empty);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->applies_list);
  rebuild_applies (d);
  return g;
}

/* Window "destroy": free the dialog state. */
static void
on_dialog_destroy (GtkWidget *w, gpointer user_data)
{
  (void) w;
  pso_dlg_free (user_data);
}

/* Returns the tallest the dialog's content may be: the height of the
 * parent's monitor, less room for the header bar and panels. */
static int
max_content_height (GtkWindow *parent)
{
  GdkDisplay *display = parent ? gtk_widget_get_display (GTK_WIDGET (parent))
                               : gdk_display_get_default ();
  GdkSurface *surface = parent ? gtk_native_get_surface (GTK_NATIVE (parent)) : NULL;
  GdkMonitor *monitor = NULL;
  if (display && surface)
    monitor = gdk_display_get_monitor_at_surface (display, surface);
  if (!monitor && display) {
    GListModel *monitors = gdk_display_get_monitors (display);
    if (g_list_model_get_n_items (monitors) > 0) {
      monitor = g_list_model_get_item (monitors, 0);
      g_object_unref (monitor);   /* the display keeps it alive */
    }
  }
  if (!monitor) return 700;

  GdkRectangle geo;
  gdk_monitor_get_geometry (monitor, &geo);
  return MAX (300, geo.height - 160);
}

void
sbv_pso_dialog_show (GtkWindow      *parent,
                     SbvConnection  *conn,
                     const SbvPso   *pso,
                     SbvPsoDoneFn    done,
                     gpointer        user_data,
                     GDestroyNotify  user_data_free)
{
  g_return_if_fail (conn && pso);

  PsoDlg *d    = g_new0 (PsoDlg, 1);
  d->conn      = g_object_ref (conn);
  d->original  = sbv_pso_copy (pso);
  d->applies   = g_ptr_array_new_with_free_func (g_free);
  d->done      = done;
  d->done_data = user_data;
  d->done_free = user_data_free;
  for (guint i = 0; pso->applies_to && pso->applies_to[i]; i++)
    g_ptr_array_add (d->applies, g_strdup (pso->applies_to[i]));

  gboolean is_new = pso->dn == NULL;
  d->win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (d->win),
                        is_new ? "New Password Policy" : "Edit Password Policy");
  gtk_window_set_modal (GTK_WINDOW (d->win), TRUE);
  if (parent) gtk_window_set_transient_for (GTK_WINDOW (d->win), parent);
  gtk_window_set_default_size (GTK_WINDOW (d->win), 600, -1);
  g_object_set_data (G_OBJECT (d->win), "pso-dlg", d);

  GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  d->cancel_btn = gtk_button_new_with_label ("Cancel");
  g_signal_connect_swapped (d->cancel_btn, "clicked", G_CALLBACK (gtk_window_destroy), d->win);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), d->cancel_btn);
  d->save_btn = gtk_button_new_with_label (is_new ? "Create" : "Save");
  gtk_widget_add_css_class (d->save_btn, "suggested-action");
  g_signal_connect (d->save_btn, "clicked", G_CALLBACK (on_save_clicked), d);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), d->save_btn);
  d->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (d->spinner, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), d->spinner);
  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);
  /* Grow to fit the form, and only scroll when the screen is too short. */
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll),
                                              max_content_height (parent));
  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 560);
  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top    (body, 18);
  gtk_widget_set_margin_bottom (body, 18);
  gtk_widget_set_margin_start  (body, 12);
  gtk_widget_set_margin_end    (body, 12);

  gtk_box_append (GTK_BOX (body), build_general (d));

  d->form = sbv_policy_form_new ("Used instead of the domain policy for the accounts below",
                                 1024);
  sbv_policy_form_set (SBV_POLICY_FORM (d->form), &pso->policy);
  gtk_box_append (GTK_BOX (body), d->form);
  gtk_box_append (GTK_BOX (body), build_applies (d));

  d->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (d->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (d->error_label), 0);
  gtk_widget_add_css_class (d->error_label, "error");
  gtk_widget_set_visible (d->error_label, FALSE);
  gtk_box_append (GTK_BOX (body), d->error_label);

  if (!is_new) {
    d->delete_btn = gtk_button_new_with_label ("Delete Policy");
    gtk_widget_add_css_class (d->delete_btn, "destructive-action");
    gtk_widget_set_halign (d->delete_btn, GTK_ALIGN_START);
    g_signal_connect (d->delete_btn, "clicked", G_CALLBACK (on_delete_clicked), d);
    gtk_box_append (GTK_BOX (body), d->delete_btn);
  }

  adw_clamp_set_child (ADW_CLAMP (clamp), body);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  gtk_box_append (GTK_BOX (vbox), scroll);
  gtk_window_set_child (GTK_WINDOW (d->win), vbox);

  g_signal_connect (d->win, "close-request", G_CALLBACK (on_close_request), d);
  g_signal_connect (d->win, "destroy", G_CALLBACK (on_dialog_destroy), d);
  gtk_window_present (GTK_WINDOW (d->win));
  gtk_widget_grab_focus (d->name_row);
}
