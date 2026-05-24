#include "sbv-create-contact-dialog.h"
#include "../backend/sbv-contacts-backend.h"

#include <adwaita.h>
#include <string.h>

/* Per-dialog state. Freed on the window's "destroy" signal. */
typedef struct {
  GtkWindow                *window;
  SbvConnection            *conn;
  SbvCreateContactCallback  callback;
  gpointer                  user_data;

  GtkWidget *given_entry;
  GtkWidget *sn_entry;
  GtkWidget *cn_entry;
  GtkWidget *mail_entry;
  GtkWidget *tel_entry;
  GtkWidget *container_entry;

  GtkWidget *create_btn;
  GtkWidget *spinner;
  GtkWidget *error_label;

  /* Tracks whether the user has hand-edited cn so we stop auto-filling
   * once they have. Same convention as the create-user dialog. */
  gboolean syncing;
  gboolean cn_manual;
} CreateCtx;

/* Drops the per-dialog allocation. */
static void
ctx_free (CreateCtx *ctx)
{
  g_free (ctx);
}

/* Toggles busy state while the LDAP add round-trip is in flight. */
static void
set_busy (CreateCtx *ctx, gboolean busy)
{
  gtk_widget_set_sensitive (ctx->create_btn, !busy);
  gtk_widget_set_visible   (ctx->spinner,    busy);
  if (busy) gtk_spinner_start (GTK_SPINNER (ctx->spinner));
  else      gtk_spinner_stop  (GTK_SPINNER (ctx->spinner));
}

/* Async-completion callback: surfaces errors inline; on success closes the
 * dialog and forwards the new DN to the parent so it can refresh. */
static void
on_create_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CreateCtx     *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  char *new_dn = sbv_contacts_create_finish (conn, result, &err);

  set_busy (ctx, FALSE);

  if (!new_dn) {
    gtk_label_set_text (GTK_LABEL (ctx->error_label), err->message);
    gtk_widget_set_visible (ctx->error_label, TRUE);
    g_error_free (err);
    return;
  }

  if (ctx->callback)
    ctx->callback (conn, new_dn, ctx->user_data);
  g_free (new_dn);

  gtk_window_destroy (ctx->window);
}

/* Validates the form, then dispatches the create call. */
static void
on_create_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  CreateCtx *ctx = user_data;

  const char *given     = gtk_editable_get_text (GTK_EDITABLE (ctx->given_entry));
  const char *sn        = gtk_editable_get_text (GTK_EDITABLE (ctx->sn_entry));
  const char *cn        = gtk_editable_get_text (GTK_EDITABLE (ctx->cn_entry));
  const char *mail      = gtk_editable_get_text (GTK_EDITABLE (ctx->mail_entry));
  const char *tel       = gtk_editable_get_text (GTK_EDITABLE (ctx->tel_entry));
  const char *container = gtk_editable_get_text (GTK_EDITABLE (ctx->container_entry));

  gtk_widget_set_visible (ctx->error_label, FALSE);

  if (!*cn || !*container) {
    gtk_label_set_text (GTK_LABEL (ctx->error_label),
                        "Full name and container are required.");
    gtk_widget_set_visible (ctx->error_label, TRUE);
    return;
  }

  set_busy (ctx, TRUE);
  sbv_contacts_create_async (ctx->conn, cn, given, sn, mail, tel, container,
                              NULL, on_create_done, ctx);
}

/* Auto-fill cn from given+sn while the user hasn't manually edited cn. */
static void
on_name_part_changed (GtkEditable *editable, gpointer user_data)
{
  (void) editable;
  CreateCtx *ctx = user_data;
  if (ctx->cn_manual) return;

  const char *given = gtk_editable_get_text (GTK_EDITABLE (ctx->given_entry));
  const char *sn    = gtk_editable_get_text (GTK_EDITABLE (ctx->sn_entry));

  char *combined = g_strdup_printf ("%s%s%s", given,
                                     (*given && *sn) ? " " : "", sn);

  ctx->syncing = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (ctx->cn_entry), combined);
  ctx->syncing = FALSE;

  g_free (combined);
}

/* Detect manual cn edits so on_name_part_changed stops overwriting. */
static void
on_cn_changed (GtkEditable *editable, gpointer user_data)
{
  CreateCtx *ctx = user_data;
  if (ctx->syncing) return;
  const char *text = gtk_editable_get_text (editable);
  ctx->cn_manual = (*text != '\0');
}

/* Builds a labelled form row with the project's standard label column width. */
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

/* Public entry — see header. */
void
sbv_create_contact_dialog_show (GtkWindow               *parent,
                                 SbvConnection           *conn,
                                 SbvCreateContactCallback callback,
                                 gpointer                 user_data)
{
  if (!conn) return;

  CreateCtx *ctx = g_new0 (CreateCtx, 1);
  ctx->conn      = conn;
  ctx->callback  = callback;
  ctx->user_data = user_data;

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Create Contact");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 460, 380);
  if (parent)
    gtk_window_set_transient_for (GTK_WINDOW (win), parent);
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

  ctx->given_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->given_entry), "First name");
  g_signal_connect (ctx->given_entry, "changed",
                    G_CALLBACK (on_name_part_changed), ctx);
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("First Name", ctx->given_entry));

  ctx->sn_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->sn_entry), "Last name");
  g_signal_connect (ctx->sn_entry, "changed",
                    G_CALLBACK (on_name_part_changed), ctx);
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Last Name", ctx->sn_entry));

  ctx->cn_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->cn_entry),
                                   "Used as the CN/RDN");
  g_signal_connect (ctx->cn_entry, "changed",
                    G_CALLBACK (on_cn_changed), ctx);
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Full Name (cn)", ctx->cn_entry));

  ctx->mail_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->mail_entry),
                                   "name@example.com");
  gtk_widget_add_css_class (ctx->mail_entry, "monospace");
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Email", ctx->mail_entry));

  ctx->tel_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->tel_entry), "+61 …");
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Phone", ctx->tel_entry));

  ctx->container_entry = gtk_entry_new ();
  gtk_widget_add_css_class (ctx->container_entry, "monospace");
  const char *base = sbv_connection_get_base_dn (conn);
  if (base) {
    char *def = g_strdup_printf ("CN=Users,%s", base);
    gtk_editable_set_text (GTK_EDITABLE (ctx->container_entry), def);
    g_free (def);
  }
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Container DN", ctx->container_entry));

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
  g_signal_connect_swapped (win, "destroy", G_CALLBACK (ctx_free), ctx);
  gtk_window_present (GTK_WINDOW (win));

  gtk_widget_grab_focus (ctx->given_entry);
}
