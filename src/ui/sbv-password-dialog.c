#include "sbv-password-dialog.h"
#include <adwaita.h>

typedef struct {
  GtkWidget          *dialog;
  GtkWidget          *password_entry;
  GtkWidget          *error_label;
  GtkWidget          *connect_btn;
  GtkWidget          *cancel_btn;

  SbvProfile         *profile;   /* unowned */
  SbvPasswordCallback callback;
  gpointer            callback_data;

  SbvConnection      *pending_conn;
} PwData;

static void
pw_data_free (PwData *d)
{
  g_clear_object (&d->pending_conn);
  g_free (d);
}

static void
set_busy (PwData *d, gboolean busy)
{
  gtk_widget_set_sensitive (d->connect_btn,    !busy);
  gtk_widget_set_sensitive (d->cancel_btn,     !busy);
  gtk_widget_set_sensitive (d->password_entry, !busy);
}

static void
on_connect_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  PwData        *d    = user_data;
  GError        *err  = NULL;

  if (!sbv_connection_connect_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (d->error_label), err->message);
    gtk_widget_set_visible (d->error_label, TRUE);
    g_error_free (err);
    set_busy (d, FALSE);
    return;
  }

  SbvPasswordCallback cb   = d->callback;
  gpointer            data = d->callback_data;

  gtk_window_destroy (GTK_WINDOW (d->dialog));
  cb (conn, data);
  g_object_unref (conn);
}

static void
on_connect_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  PwData     *d      = user_data;
  const char *passwd = gtk_editable_get_text (GTK_EDITABLE (d->password_entry));

  gtk_widget_set_visible (d->error_label, FALSE);
  set_busy (d, TRUE);

  g_clear_object (&d->pending_conn);
  d->pending_conn = sbv_connection_new ();

  sbv_connection_connect_profile_async (d->pending_conn, d->profile, passwd,
                                         NULL, on_connect_done, d);
}

static void
on_dialog_destroy (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  PwData *d = user_data;
  g_clear_object (&d->pending_conn);
  pw_data_free (d);
}

void
sbv_password_dialog_show (GtkWindow          *parent,
                           SbvProfile         *profile,
                           SbvPasswordCallback callback,
                           gpointer            user_data)
{
  PwData *d = g_new0 (PwData, 1);
  d->profile       = profile;
  d->callback      = callback;
  d->callback_data = user_data;

  /* Build a minimal dialog */
  d->dialog = gtk_window_new ();
  const char *name = sbv_profile_get_name (profile);
  char *title = g_strdup_printf ("Connect to %s", name ? name : "");
  gtk_window_set_title (GTK_WINDOW (d->dialog), title);
  g_free (title);
  gtk_window_set_modal (GTK_WINDOW (d->dialog), TRUE);
  gtk_window_set_transient_for (GTK_WINDOW (d->dialog), parent);
  gtk_window_set_resizable (GTK_WINDOW (d->dialog), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (d->dialog), 360, -1);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  d->cancel_btn = gtk_button_new_with_label ("Cancel");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), d->cancel_btn);
  g_signal_connect_swapped (d->cancel_btn, "clicked",
                             G_CALLBACK (gtk_window_destroy), d->dialog);

  d->connect_btn = gtk_button_new_with_label ("Connect");
  gtk_widget_add_css_class (d->connect_btn, "suggested-action");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), d->connect_btn);
  g_signal_connect (d->connect_btn, "clicked", G_CALLBACK (on_connect_clicked), d);

  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top    (form, 16);
  gtk_widget_set_margin_bottom (form, 16);
  gtk_widget_set_margin_start  (form, 16);
  gtk_widget_set_margin_end    (form, 16);

  /* Show which user will be connecting */
  const char *bind_dn = sbv_profile_get_bind_dn (profile);
  if (bind_dn && *bind_dn) {
    char *msg = g_strdup_printf ("Password for %s", bind_dn);
    GtkWidget *lbl = gtk_label_new (msg);
    g_free (msg);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0);
    gtk_widget_add_css_class (lbl, "dim-label");
    gtk_box_append (GTK_BOX (form), lbl);
  }

  d->password_entry = gtk_entry_new ();
  gtk_entry_set_visibility (GTK_ENTRY (d->password_entry), FALSE);
  gtk_entry_set_placeholder_text (GTK_ENTRY (d->password_entry), "Password");
  gtk_entry_set_activates_default (GTK_ENTRY (d->password_entry), TRUE);
  gtk_box_append (GTK_BOX (form), d->password_entry);

  d->error_label = gtk_label_new ("");
  gtk_widget_set_visible (d->error_label, FALSE);
  gtk_label_set_wrap (GTK_LABEL (d->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (d->error_label), 0);
  gtk_widget_add_css_class (d->error_label, "error");
  gtk_box_append (GTK_BOX (form), d->error_label);

  gtk_box_append (GTK_BOX (vbox), form);
  gtk_window_set_child (GTK_WINDOW (d->dialog), vbox);

  g_signal_connect (d->dialog, "destroy", G_CALLBACK (on_dialog_destroy), d);

  gtk_window_present (GTK_WINDOW (d->dialog));
}
