#include "sbv-create-ou-dialog.h"
#include "../backend/sbv-ou-backend.h"

#include <adwaita.h>

/* Per-dialog state. One ref for the window plus one while the add is in
 * flight, so a late completion never touches freed memory. */
typedef struct {
  GtkWindow           *window;
  SbvConnection       *conn;   /* owned ref */
  SbvCreateOuCallback  callback;
  gpointer             user_data;

  GtkWidget *name_entry;
  GtkWidget *desc_entry;
  GtkWidget *parent_entry;
  GtkWidget *create_btn;
  GtkWidget *spinner;
  GtkWidget *error_label;

  int        refs;
  gboolean   alive;
} CreateCtx;

/* Drops one reference and frees the context on the last one. */
static void
ctx_unref (CreateCtx *ctx)
{
  if (--ctx->refs > 0) return;
  g_object_unref (ctx->conn);
  g_free (ctx);
}

/* Window "destroy": mark the widgets gone and drop the window's ref. */
static void
on_window_destroy (GtkWidget *win, gpointer user_data)
{
  (void) win;
  CreateCtx *ctx = user_data;
  ctx->alive = FALSE;
  ctx_unref (ctx);
}

/* Toggles busy state while the LDAP add is in flight. */
static void
set_busy (CreateCtx *ctx, gboolean busy)
{
  gtk_widget_set_sensitive (ctx->create_btn, !busy);
  gtk_widget_set_visible   (ctx->spinner,    busy);
  if (busy) gtk_spinner_start (GTK_SPINNER (ctx->spinner));
  else      gtk_spinner_stop  (GTK_SPINNER (ctx->spinner));
}

/* Async-completion: close and report the new DN, or show the error. */
static void
on_create_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CreateCtx     *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  char *new_dn = sbv_ou_create_finish (conn, result, &err);

  if (new_dn && ctx->callback)
    ctx->callback (conn, new_dn, ctx->user_data);

  if (ctx->alive) {
    set_busy (ctx, FALSE);
    if (new_dn) {
      gtk_window_destroy (ctx->window);
    } else {
      gtk_label_set_text (GTK_LABEL (ctx->error_label), err->message);
      gtk_widget_set_visible (ctx->error_label, TRUE);
    }
  }

  g_free (new_dn);
  g_clear_error (&err);
  ctx_unref (ctx);
}

/* Validates the form, then dispatches the add. */
static void
on_create_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  CreateCtx *ctx = user_data;

  const char *name   = gtk_editable_get_text (GTK_EDITABLE (ctx->name_entry));
  const char *desc   = gtk_editable_get_text (GTK_EDITABLE (ctx->desc_entry));
  const char *parent = gtk_editable_get_text (GTK_EDITABLE (ctx->parent_entry));

  gtk_widget_set_visible (ctx->error_label, FALSE);
  if (!*name || !*parent) {
    gtk_label_set_text (GTK_LABEL (ctx->error_label),
                        "Name and parent are required.");
    gtk_widget_set_visible (ctx->error_label, TRUE);
    return;
  }

  set_busy (ctx, TRUE);
  ctx->refs++;
  sbv_ou_create_async (ctx->conn, parent, name, desc, NULL, on_create_done, ctx);
}

/* Labelled form row with the standard 140px label column. */
static GtkWidget *
make_field_row (const char *label_text, GtkWidget *widget)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lbl = gtk_label_new (label_text);
  gtk_label_set_xalign (GTK_LABEL (lbl), 1);
  gtk_widget_set_size_request (lbl, 140, -1);
  gtk_widget_add_css_class (lbl, "dim-label");
  gtk_box_append (GTK_BOX (box), lbl);
  gtk_widget_set_hexpand (widget, TRUE);
  gtk_box_append (GTK_BOX (box), widget);
  return box;
}

void
sbv_create_ou_dialog_show (GtkWindow           *parent,
                           SbvConnection       *conn,
                           const char          *parent_dn,
                           SbvCreateOuCallback  callback,
                           gpointer             user_data)
{
  if (!conn) return;

  CreateCtx *ctx = g_new0 (CreateCtx, 1);
  ctx->conn      = g_object_ref (conn);
  ctx->callback  = callback;
  ctx->user_data = user_data;
  ctx->refs      = 1;
  ctx->alive     = TRUE;

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Create OU");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 520, 240);
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

  ctx->create_btn = gtk_button_new_with_label ("Create");
  gtk_widget_add_css_class (ctx->create_btn, "suggested-action");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->create_btn);
  g_signal_connect (ctx->create_btn, "clicked",
                    G_CALLBACK (on_create_clicked), ctx);

  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top    (form, 16);
  gtk_widget_set_margin_bottom (form, 16);
  gtk_widget_set_margin_start  (form, 20);
  gtk_widget_set_margin_end    (form, 20);

  ctx->name_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->name_entry), "e.g. Sales");
  gtk_entry_set_activates_default (GTK_ENTRY (ctx->name_entry), TRUE);
  gtk_box_append (GTK_BOX (form), make_field_row ("Name", ctx->name_entry));

  ctx->desc_entry = gtk_entry_new ();
  gtk_box_append (GTK_BOX (form), make_field_row ("Description", ctx->desc_entry));

  ctx->parent_entry = gtk_entry_new ();
  gtk_widget_add_css_class (ctx->parent_entry, "monospace");
  const char *def = parent_dn ? parent_dn : sbv_connection_get_base_dn (conn);
  gtk_editable_set_text (GTK_EDITABLE (ctx->parent_entry), def ?: "");
  gtk_box_append (GTK_BOX (form), make_field_row ("Parent DN", ctx->parent_entry));

  ctx->error_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (ctx->error_label), 0);
  gtk_label_set_wrap (GTK_LABEL (ctx->error_label), TRUE);
  gtk_widget_add_css_class (ctx->error_label, "error");
  gtk_widget_set_margin_top (ctx->error_label, 6);
  gtk_widget_set_margin_start (ctx->error_label, 148);
  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_box_append (GTK_BOX (form), ctx->error_label);

  gtk_box_append (GTK_BOX (vbox), form);
  gtk_window_set_child (GTK_WINDOW (win), vbox);
  gtk_window_set_default_widget (GTK_WINDOW (win), ctx->create_btn);
  g_signal_connect (win, "destroy", G_CALLBACK (on_window_destroy), ctx);
  gtk_window_present (GTK_WINDOW (win));
  gtk_widget_grab_focus (ctx->name_entry);
}
