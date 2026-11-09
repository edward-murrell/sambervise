#include "sbv-move-dialog.h"
#include "sbv-ou-row.h"
#include "../backend/sbv-ou-backend.h"
#include "../backend/sbv-dn.h"

#include <adwaita.h>
#include <string.h>

/* Per-dialog state. Freed on the window's "destroy" signal. */
typedef struct {
  GtkWindow       *window;
  SbvConnection   *conn;      /* owned ref */
  char            *dn;
  char            *current_parent;
  SbvMoveCallback  callback;
  gpointer         user_data;

  GtkWidget *stack;           /* "loading" | "list" */
  GtkWidget *search_entry;
  GtkWidget *list_box;
  GtkWidget *move_btn;
  GtkWidget *spinner;
  GtkWidget *error_label;
  char      *filter_text;

  /* One ref for the window plus one per async call in flight. `alive` is
   * cleared on destroy so late completions don't touch freed widgets. */
  int        refs;
  gboolean   alive;
} MoveCtx;

/* Takes a reference before dispatching an async call. */
static MoveCtx *
ctx_ref (MoveCtx *ctx)
{
  ctx->refs++;
  return ctx;
}

/* Drops one reference and frees the context on the last one. */
static void
ctx_unref (MoveCtx *ctx)
{
  if (--ctx->refs > 0) return;
  g_object_unref (ctx->conn);
  g_free (ctx->dn);
  g_free (ctx->current_parent);
  g_free (ctx->filter_text);
  g_free (ctx);
}

/* Window "destroy": mark the widgets gone and drop the window's ref. */
static void
on_window_destroy (GtkWidget *win, gpointer user_data)
{
  (void) win;
  MoveCtx *ctx = user_data;
  ctx->alive = FALSE;
  ctx_unref (ctx);
}

/* Move button "destroy": the window is closing. GTK destroys the header
 * bar before the list box, and the list box then emits "row-selected"
 * with no row. Disconnect now so that late signal can't reach move_btn. */
static void
on_move_btn_destroy (GtkWidget *btn, gpointer user_data)
{
  (void) btn;
  MoveCtx *ctx = user_data;
  g_signal_handlers_disconnect_by_data (ctx->list_box, ctx);
}

/* Toggles busy state while the move is in flight. */
static void
set_busy (MoveCtx *ctx, gboolean busy)
{
  gtk_widget_set_sensitive (ctx->move_btn, !busy &&
    gtk_list_box_get_selected_row (GTK_LIST_BOX (ctx->list_box)) != NULL);
  gtk_widget_set_visible (ctx->spinner, busy);
  if (busy) gtk_spinner_start (GTK_SPINNER (ctx->spinner));
  else      gtk_spinner_stop  (GTK_SPINNER (ctx->spinner));
}

/* Shows an inline error under the list. */
static void
show_error (MoveCtx *ctx, const char *message)
{
  gtk_label_set_text (GTK_LABEL (ctx->error_label), message);
  gtk_widget_set_visible (ctx->error_label, TRUE);
}

/* Search filter over the container rows: matches the name or any path
 * entry, case-insensitively. */
static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  MoveCtx *ctx = user_data;
  if (!ctx->filter_text) return TRUE;
  SbvOu *ou = sbv_ou_row_get_ou (row);
  if (!ou) return TRUE;
  char *joined = g_strjoinv (" ", (char **) sbv_ou_get_path (ou));
  char *hay    = g_utf8_strdown (joined, -1);
  char *name   = g_utf8_strdown (sbv_ou_get_name (ou) ?: "", -1);
  gboolean hit = strstr (hay, ctx->filter_text) || strstr (name, ctx->filter_text);
  g_free (joined);
  g_free (hay);
  g_free (name);
  return hit;
}

/* Re-runs the list filter when the search entry changes. */
static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  MoveCtx *ctx = user_data;
  g_free (ctx->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  ctx->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (ctx->list_box));
}

/* Only allow Move once a destination is picked. */
static void
on_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  MoveCtx *ctx = user_data;
  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_widget_set_sensitive (ctx->move_btn, row != NULL);
}

/* Async-completion for the container list: build one row per container,
 * greying out the ones the object can't move to. */
static void
on_list_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  MoveCtx *ctx = user_data;
  GError  *err = NULL;

  GListStore *store = sbv_ou_list_finish (SBV_CONNECTION (source), result, &err);
  if (!ctx->alive) {
    g_clear_object (&store);
    g_clear_error (&err);
    ctx_unref (ctx);
    return;
  }

  gtk_stack_set_visible_child_name (GTK_STACK (ctx->stack), "list");
  if (!store) {
    show_error (ctx, err->message);
    g_error_free (err);
    ctx_unref (ctx);
    return;
  }

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    SbvOu     *ou  = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget *row = sbv_ou_row_new (ou);
    const char *odn = sbv_ou_get_dn (ou);

    gboolean is_current = sbv_dn_equal (odn, ctx->current_parent);
    gboolean is_inside  = sbv_dn_is_within (odn, ctx->dn);
    if (is_current || is_inside) {
      gtk_list_box_row_set_selectable  (GTK_LIST_BOX_ROW (row), FALSE);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
      gtk_widget_set_sensitive (row, FALSE);
      gtk_widget_set_tooltip_text (row, is_current
        ? "Current location" : "Can't move an OU into itself");
    }
    gtk_list_box_append (GTK_LIST_BOX (ctx->list_box), row);
    g_object_unref (ou);
  }
  g_object_unref (store);
  ctx_unref (ctx);
}

/* Async-completion for the move itself: close on success, otherwise show
 * the DC's reason inline. */
static void
on_move_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  MoveCtx       *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  char *new_dn = sbv_ou_move_object_finish (conn, result, &err);

  if (!ctx->alive) {
    /* Window closed mid-move. The move still happened, so tell the caller. */
    if (new_dn && ctx->callback) ctx->callback (conn, new_dn, ctx->user_data);
    g_free (new_dn);
    g_clear_error (&err);
    ctx_unref (ctx);
    return;
  }

  set_busy (ctx, FALSE);
  if (!new_dn) {
    show_error (ctx, err->message);
    g_error_free (err);
    ctx_unref (ctx);
    return;
  }

  if (ctx->callback) ctx->callback (conn, new_dn, ctx->user_data);
  g_free (new_dn);
  gtk_window_destroy (ctx->window);
  ctx_unref (ctx);
}

/* "Move" button: dispatch the rename-with-new-superior. */
static void
on_move_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  MoveCtx       *ctx = user_data;
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (ctx->list_box));
  SbvOu         *ou  = row ? sbv_ou_row_get_ou (row) : NULL;
  if (!ou) return;

  gtk_widget_set_visible (ctx->error_label, FALSE);
  set_busy (ctx, TRUE);
  sbv_ou_move_object_async (ctx->conn, ctx->dn, sbv_ou_get_dn (ou),
                            NULL, on_move_done, ctx_ref (ctx));
}

/* Double-click or Enter on a row moves straight away. */
static void
on_row_activated (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  (void) row;
  MoveCtx *ctx = user_data;
  if (gtk_widget_get_sensitive (ctx->move_btn))
    on_move_clicked (NULL, ctx);
}

void
sbv_move_dialog_show (GtkWindow       *parent,
                      SbvConnection   *conn,
                      const char      *dn,
                      const char      *label,
                      SbvMoveCallback  callback,
                      gpointer         user_data)
{
  if (!conn || !dn) return;

  MoveCtx *ctx = g_new0 (MoveCtx, 1);
  ctx->conn      = g_object_ref (conn);
  ctx->dn        = g_strdup (dn);
  ctx->callback  = callback;
  ctx->user_data = user_data;
  ctx->alive     = TRUE;
  ctx->refs      = 1;
  sbv_dn_split (dn, NULL, &ctx->current_parent);

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Move");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 480, 520);
  if (parent) gtk_window_set_transient_for (GTK_WINDOW (win), parent);
  ctx->window = GTK_WINDOW (win);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);
  g_signal_connect_swapped (cancel_btn, "clicked",
                            G_CALLBACK (gtk_window_destroy), win);

  ctx->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (ctx->spinner, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->spinner);

  ctx->move_btn = gtk_button_new_with_label ("Move");
  gtk_widget_add_css_class (ctx->move_btn, "suggested-action");
  gtk_widget_set_sensitive (ctx->move_btn, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->move_btn);
  g_signal_connect (ctx->move_btn, "destroy",
                    G_CALLBACK (on_move_btn_destroy), ctx);
  g_signal_connect (ctx->move_btn, "clicked",
                    G_CALLBACK (on_move_clicked), ctx);

  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top    (body, 12);
  gtk_widget_set_margin_bottom (body, 16);
  gtk_widget_set_margin_start  (body, 16);
  gtk_widget_set_margin_end    (body, 16);
  gtk_widget_set_vexpand (body, TRUE);

  char *esc    = g_markup_escape_text (label && *label ? label : dn, -1);
  char *prompt = g_strdup_printf ("Move <b>%s</b> to:", esc);
  GtkWidget *prompt_lbl = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (prompt_lbl), prompt);
  gtk_label_set_xalign (GTK_LABEL (prompt_lbl), 0);
  gtk_label_set_ellipsize (GTK_LABEL (prompt_lbl), PANGO_ELLIPSIZE_END);
  g_free (prompt);
  g_free (esc);
  gtk_box_append (GTK_BOX (body), prompt_lbl);

  ctx->search_entry = gtk_search_entry_new ();
  gtk_widget_set_tooltip_text (ctx->search_entry, "Search containers");
  g_signal_connect (ctx->search_entry, "search-changed",
                    G_CALLBACK (on_search_changed), ctx);
  gtk_box_append (GTK_BOX (body), ctx->search_entry);

  ctx->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (ctx->stack, TRUE);
  {
    GtkWidget *sp = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (sp));
    gtk_widget_set_halign (sp, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (sp, GTK_ALIGN_CENTER);
    gtk_stack_add_named (GTK_STACK (ctx->stack), sp, "loading");
  }
  {
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    ctx->list_box = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (ctx->list_box),
                                     GTK_SELECTION_SINGLE);
    gtk_list_box_set_filter_func (GTK_LIST_BOX (ctx->list_box),
                                  filter_func, ctx, NULL);
    gtk_widget_add_css_class (ctx->list_box, "boxed-list");
    g_signal_connect (ctx->list_box, "row-selected",
                      G_CALLBACK (on_row_selected), ctx);
    g_signal_connect (ctx->list_box, "row-activated",
                      G_CALLBACK (on_row_activated), ctx);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), ctx->list_box);
    gtk_stack_add_named (GTK_STACK (ctx->stack), scroll, "list");
  }
  gtk_stack_set_visible_child_name (GTK_STACK (ctx->stack), "loading");
  gtk_box_append (GTK_BOX (body), ctx->stack);

  ctx->error_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (ctx->error_label), 0);
  gtk_label_set_wrap (GTK_LABEL (ctx->error_label), TRUE);
  gtk_widget_add_css_class (ctx->error_label, "error");
  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_box_append (GTK_BOX (body), ctx->error_label);

  gtk_box_append (GTK_BOX (vbox), body);
  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect (win, "destroy", G_CALLBACK (on_window_destroy), ctx);
  gtk_window_present (GTK_WINDOW (win));
  gtk_widget_grab_focus (ctx->search_entry);

  sbv_ou_list_async (conn, NULL, on_list_loaded, ctx_ref (ctx));
}
