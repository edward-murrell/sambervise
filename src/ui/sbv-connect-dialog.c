#include "sbv-connect-dialog.h"
#include "../backend/sbv-dns.h"

#include <adwaita.h>
#include <string.h>

typedef struct {
  GtkWidget         *dialog;
  GtkWidget         *name_entry;
  GtkWidget         *domain_entry;
  GtkWidget         *discover_btn;
  GtkWidget         *dc_scroll;
  GtkWidget         *dc_list;
  GtkWidget         *host_entry;
  GtkWidget         *port_spin;
  GtkWidget         *base_dn_entry;
  GtkWidget         *krb_radio;
  GtkWidget         *simple_radio;
  GtkWidget         *creds_box;
  GtkWidget         *bind_dn_entry;
  GtkWidget         *password_entry;
  GtkWidget         *use_tls_check;
  GtkWidget         *use_ldaps_check;
  GtkWidget         *skip_cert_check;
  GtkWidget         *error_label;
  GtkWidget         *connect_btn;
  GtkWidget         *cancel_btn;

  GListStore        *profiles_store; /* unowned */
  SbvConnectCallback callback;
  gpointer           callback_data;

  SbvConnection     *pending_conn;
  GCancellable      *discover_cancel;
} DialogData;

static void
dialog_data_free (DialogData *d)
{
  g_clear_object (&d->pending_conn);
  g_clear_object (&d->discover_cancel);
  g_free (d);
}

/* ── Domain → suggested Base DN ────────────────────────────────────────── */

/* Return the best guess at the local DNS domain.
 * Checks /etc/resolv.conf for a "domain" or "search" directive first,
 * then falls back to stripping the hostname prefix.
 * Caller frees the returned string. */
static char *
detect_local_domain (void)
{
  /* 1. Parse /etc/resolv.conf */
  char *contents = NULL;
  if (g_file_get_contents ("/etc/resolv.conf", &contents, NULL, NULL)) {
    char **lines = g_strsplit (contents, "\n", -1);
    g_free (contents);
    for (int i = 0; lines[i]; i++) {
      const char *line = g_strstrip (lines[i]);
      const char *val  = NULL;
      if (g_str_has_prefix (line, "domain "))
        val = line + 7;
      else if (g_str_has_prefix (line, "search "))
        val = line + 7;
      if (val) {
        /* Take the first whitespace-delimited token */
        char **tokens = g_strsplit_set (val, " \t", 2);
        char *domain  = (tokens && tokens[0] && *tokens[0])
                          ? g_strdup (tokens[0]) : NULL;
        g_strfreev (tokens);
        g_strfreev (lines);
        if (domain) return domain;
      }
    }
    g_strfreev (lines);
  }

  /* 2. Derive from hostname (e.g. "host.example.com" → "example.com") */
  const char *hostname = g_get_host_name ();
  if (hostname) {
    const char *dot = strchr (hostname, '.');
    if (dot && *(dot + 1))
      return g_strdup (dot + 1);
  }

  return NULL;
}

static char *
domain_to_base_dn (const char *domain)
{
  if (!domain || !*domain) return NULL;
  char **parts = g_strsplit (domain, ".", -1);
  GString *dn  = g_string_new (NULL);
  for (int i = 0; parts[i]; i++) {
    if (i > 0) g_string_append (dn, ",");
    g_string_append (dn, "DC=");
    g_string_append (dn, parts[i]);
  }
  g_strfreev (parts);
  return g_string_free (dn, FALSE);
}

/* ── Auth radio toggle ──────────────────────────────────────────────────── */

static void
on_auth_toggled (GtkCheckButton *btn, gpointer user_data)
{
  (void) btn;
  DialogData *d       = user_data;
  gboolean    simple  = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->simple_radio));
  gtk_widget_set_visible (d->creds_box, simple);
}

/* ── LDAPS port convenience ─────────────────────────────────────────────── */

static void
on_ldaps_toggled (GtkCheckButton *btn, gpointer user_data)
{
  DialogData *d = user_data;
  if (gtk_check_button_get_active (btn)) {
    int current = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (d->port_spin));
    if (current == 389)
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->port_spin), 636);
  } else {
    int current = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (d->port_spin));
    if (current == 636)
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->port_spin), 389);
  }
}

/* ── DNS discovery ──────────────────────────────────────────────────────── */

static void
on_discover_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  DialogData *d   = user_data;
  GError     *err = NULL;

  gtk_widget_set_sensitive (d->discover_btn, TRUE);

  GListStore *store = sbv_dns_discover_finish (result, &err);
  if (!store) {
    gtk_label_set_text (GTK_LABEL (d->error_label), err->message);
    gtk_widget_set_visible (d->error_label, TRUE);
    g_error_free (err);
    return;
  }

  gtk_widget_set_visible (d->error_label, FALSE);

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (d->dc_list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (d->dc_list), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    /* Use GObject data on the store item for hostname/port */
    GObject *item    = g_list_model_get_item (G_LIST_MODEL (store), i);
    /* Access via property—but SbvDcTarget type is private to sbv-dns.c,
     * so we stashed the data during construction. Retrieve via child
     * widget label text isn't ideal; instead we store them as GObject data. */
    const char *host = g_object_get_data (item, "hostname");
    guint16     port = GPOINTER_TO_UINT (g_object_get_data (item, "port"));

    if (!host) { g_object_unref (item); continue; }

    GtkWidget *row = gtk_list_box_row_new ();
    char *label_text = g_strdup_printf ("%s  :%u", host, port);
    GtkWidget *lbl   = gtk_label_new (label_text);
    g_free (label_text);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0);
    gtk_widget_set_margin_top    (lbl, 6);
    gtk_widget_set_margin_bottom (lbl, 6);
    gtk_widget_set_margin_start  (lbl, 10);
    gtk_widget_set_margin_end    (lbl, 10);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), lbl);

    /* Store host/port on the row for selection handler */
    g_object_set_data_full (G_OBJECT (row), "dc-host", g_strdup (host), g_free);
    g_object_set_data      (G_OBJECT (row), "dc-port", GUINT_TO_POINTER ((guint) port));

    gtk_list_box_append (GTK_LIST_BOX (d->dc_list), row);
    g_object_unref (item);
  }

  /* Auto-fill base DN from the domain field if not already set */
  const char *domain = gtk_editable_get_text (GTK_EDITABLE (d->domain_entry));
  const char *existing_base = gtk_editable_get_text (GTK_EDITABLE (d->base_dn_entry));
  if (domain && *domain && (!existing_base || !*existing_base)) {
    char *base = domain_to_base_dn (domain);
    gtk_editable_set_text (GTK_EDITABLE (d->base_dn_entry), base);
    g_free (base);
  }

  /* If exactly one DC was found, fill host/port directly */
  if (n == 1) {
    GtkListBoxRow *only = gtk_list_box_get_row_at_index (GTK_LIST_BOX (d->dc_list), 0);
    if (only) {
      const char *h = g_object_get_data (G_OBJECT (only), "dc-host");
      guint       p = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (only), "dc-port"));
      if (h) gtk_editable_set_text (GTK_EDITABLE (d->host_entry), h);
      if (p) gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->port_spin), p);
    }
  }

  g_object_unref (store);
  gtk_widget_set_visible (d->dc_scroll, n > 1);
}

static void
on_dc_row_activated (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  DialogData *d    = user_data;
  const char *host = g_object_get_data (G_OBJECT (row), "dc-host");
  guint       port = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "dc-port"));

  if (host)
    gtk_editable_set_text (GTK_EDITABLE (d->host_entry), host);
  if (port)
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->port_spin), port);
}

static void
on_discover_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  DialogData *d      = user_data;
  const char *domain = gtk_editable_get_text (GTK_EDITABLE (d->domain_entry));

  if (!domain || !*domain) {
    gtk_label_set_text (GTK_LABEL (d->error_label), "Enter a domain name first.");
    gtk_widget_set_visible (d->error_label, TRUE);
    return;
  }

  gtk_widget_set_visible (d->error_label, FALSE);
  gtk_widget_set_sensitive (d->discover_btn, FALSE);

  g_clear_object (&d->discover_cancel);
  d->discover_cancel = g_cancellable_new ();

  sbv_dns_discover_async (domain, d->discover_cancel, on_discover_done, d);
}

/* ── Build profile from dialog state ───────────────────────────────────── */

static SbvProfile *
build_profile (DialogData *d)
{
  SbvProfile *p = sbv_profile_new ();

  const char *name = gtk_editable_get_text (GTK_EDITABLE (d->name_entry));
  sbv_profile_set_name (p, *name ? name : NULL);
  sbv_profile_set_host (p, gtk_editable_get_text (GTK_EDITABLE (d->host_entry)));
  sbv_profile_set_port (p, (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (d->port_spin)));
  sbv_profile_set_base_dn (p, gtk_editable_get_text (GTK_EDITABLE (d->base_dn_entry)));

  gboolean simple = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->simple_radio));
  sbv_profile_set_auth_type (p, simple ? SBV_AUTH_SIMPLE : SBV_AUTH_KERBEROS);
  if (simple)
    sbv_profile_set_bind_dn (p, gtk_editable_get_text (GTK_EDITABLE (d->bind_dn_entry)));

  sbv_profile_set_use_tls   (p, gtk_check_button_get_active (GTK_CHECK_BUTTON (d->use_tls_check)));
  sbv_profile_set_use_ldaps (p, gtk_check_button_get_active (GTK_CHECK_BUTTON (d->use_ldaps_check)));
  sbv_profile_set_skip_cert (p, gtk_check_button_get_active (GTK_CHECK_BUTTON (d->skip_cert_check)));

  return p;
}

/* ── Connect ────────────────────────────────────────────────────────────── */

static void
set_busy (DialogData *d, gboolean busy)
{
  gtk_widget_set_sensitive (d->connect_btn,  !busy);
  gtk_widget_set_sensitive (d->cancel_btn,   !busy);
  gtk_widget_set_sensitive (d->host_entry,   !busy);
  gtk_widget_set_sensitive (d->port_spin,    !busy);
}

static void
on_connect_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  DialogData    *d    = user_data;
  GError        *err  = NULL;

  if (!sbv_connection_connect_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (d->error_label), err->message);
    gtk_widget_set_visible (d->error_label, TRUE);
    g_error_free (err);
    set_busy (d, FALSE);
    return;
  }

  SbvProfile *profile = build_profile (d);

  /* Save if a name was given */
  SbvProfile *saved_profile = NULL;
  if (sbv_profile_get_name (profile)) {
    GError *save_err = NULL;
    sbv_profiles_upsert (d->profiles_store, profile, &save_err);
    if (save_err) {
      g_warning ("Failed to save profile: %s", save_err->message);
      g_error_free (save_err);
    } else {
      saved_profile = g_object_ref (profile);
    }
  }

  SbvConnectCallback cb   = d->callback;
  gpointer           data = d->callback_data;

  gtk_window_destroy (GTK_WINDOW (d->dialog));

  cb (conn, saved_profile, data);
  g_object_unref (conn);
  g_clear_object (&saved_profile);
  g_object_unref (profile);
}

static void
on_connect_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  DialogData *d = user_data;

  gtk_widget_set_visible (d->error_label, FALSE);

  const char *host    = gtk_editable_get_text (GTK_EDITABLE (d->host_entry));
  const char *base_dn = gtk_editable_get_text (GTK_EDITABLE (d->base_dn_entry));

  if (!*host || !*base_dn) {
    gtk_label_set_text (GTK_LABEL (d->error_label), "Host and Base DN are required.");
    gtk_widget_set_visible (d->error_label, TRUE);
    return;
  }

  gboolean simple = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->simple_radio));
  if (simple) {
    const char *bind_dn = gtk_editable_get_text (GTK_EDITABLE (d->bind_dn_entry));
    if (!*bind_dn) {
      gtk_label_set_text (GTK_LABEL (d->error_label), "Bind DN / UPN is required for simple auth.");
      gtk_widget_set_visible (d->error_label, TRUE);
      return;
    }
  }

  set_busy (d, TRUE);

  SbvProfile *profile  = build_profile (d);
  const char *password = simple
    ? gtk_editable_get_text (GTK_EDITABLE (d->password_entry))
    : NULL;

  g_clear_object (&d->pending_conn);
  d->pending_conn = sbv_connection_new ();

  sbv_connection_connect_profile_async (d->pending_conn, profile, password,
                                         NULL, on_connect_done, d);
  g_object_unref (profile);
}

static void
on_dialog_destroy (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  dialog_data_free (user_data);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

GtkWidget *
sbv_connect_dialog_new (GtkWindow         *parent,
                         GListStore        *profiles_store,
                         SbvProfile        *edit_profile,
                         SbvConnectCallback callback,
                         gpointer           user_data)
{
  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/org/ekm/sambervise/ui/connect-dialog.ui");

  DialogData *d = g_new0 (DialogData, 1);
  d->dialog          = GTK_WIDGET (gtk_builder_get_object (builder, "dialog"));
  d->name_entry      = GTK_WIDGET (gtk_builder_get_object (builder, "name_entry"));
  d->domain_entry    = GTK_WIDGET (gtk_builder_get_object (builder, "domain_entry"));
  d->discover_btn    = GTK_WIDGET (gtk_builder_get_object (builder, "discover_btn"));
  d->dc_scroll       = GTK_WIDGET (gtk_builder_get_object (builder, "dc_scroll"));
  d->dc_list         = GTK_WIDGET (gtk_builder_get_object (builder, "dc_list"));
  d->host_entry      = GTK_WIDGET (gtk_builder_get_object (builder, "host_entry"));
  d->port_spin       = GTK_WIDGET (gtk_builder_get_object (builder, "port_spin"));
  d->base_dn_entry   = GTK_WIDGET (gtk_builder_get_object (builder, "base_dn_entry"));
  d->krb_radio       = GTK_WIDGET (gtk_builder_get_object (builder, "krb_radio"));
  d->simple_radio    = GTK_WIDGET (gtk_builder_get_object (builder, "simple_radio"));
  d->creds_box       = GTK_WIDGET (gtk_builder_get_object (builder, "creds_box"));
  d->bind_dn_entry   = GTK_WIDGET (gtk_builder_get_object (builder, "bind_dn_entry"));
  d->password_entry  = GTK_WIDGET (gtk_builder_get_object (builder, "password_entry"));
  d->use_tls_check   = GTK_WIDGET (gtk_builder_get_object (builder, "use_tls_check"));
  d->use_ldaps_check = GTK_WIDGET (gtk_builder_get_object (builder, "use_ldaps_check"));
  d->skip_cert_check = GTK_WIDGET (gtk_builder_get_object (builder, "skip_cert_check"));
  d->error_label     = GTK_WIDGET (gtk_builder_get_object (builder, "error_label"));
  d->connect_btn     = GTK_WIDGET (gtk_builder_get_object (builder, "connect_btn"));
  d->cancel_btn      = GTK_WIDGET (gtk_builder_get_object (builder, "cancel_btn"));
  d->profiles_store  = profiles_store;
  d->callback        = callback;
  d->callback_data   = user_data;

  /* Pre-populate if editing a profile */
  if (edit_profile) {
    const char *n = sbv_profile_get_name (edit_profile);
    if (n) gtk_editable_set_text (GTK_EDITABLE (d->name_entry), n);
    const char *h = sbv_profile_get_host (edit_profile);
    if (h) gtk_editable_set_text (GTK_EDITABLE (d->host_entry), h);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->port_spin),
                                sbv_profile_get_port (edit_profile));
    const char *b = sbv_profile_get_base_dn (edit_profile);
    if (b) gtk_editable_set_text (GTK_EDITABLE (d->base_dn_entry), b);

    if (sbv_profile_get_auth_type (edit_profile) == SBV_AUTH_SIMPLE) {
      gtk_check_button_set_active (GTK_CHECK_BUTTON (d->simple_radio), TRUE);
      gtk_widget_set_visible (d->creds_box, TRUE);
      const char *bd = sbv_profile_get_bind_dn (edit_profile);
      if (bd) gtk_editable_set_text (GTK_EDITABLE (d->bind_dn_entry), bd);
    }

    gtk_check_button_set_active (GTK_CHECK_BUTTON (d->use_tls_check),
                                  sbv_profile_get_use_tls (edit_profile));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (d->use_ldaps_check),
                                  sbv_profile_get_use_ldaps (edit_profile));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (d->skip_cert_check),
                                  sbv_profile_get_skip_cert (edit_profile));
  }

  /* Pre-fill domain from the local system when not editing a saved profile */
  if (!edit_profile) {
    char *domain = detect_local_domain ();
    if (domain) {
      gtk_editable_set_text (GTK_EDITABLE (d->domain_entry), domain);
      g_free (domain);
    }
  }

  gtk_window_set_transient_for (GTK_WINDOW (d->dialog), parent);

  g_signal_connect (d->krb_radio,       "toggled", G_CALLBACK (on_auth_toggled),    d);
  g_signal_connect (d->simple_radio,    "toggled", G_CALLBACK (on_auth_toggled),    d);
  g_signal_connect (d->use_ldaps_check, "toggled", G_CALLBACK (on_ldaps_toggled),   d);
  g_signal_connect (d->discover_btn,    "clicked", G_CALLBACK (on_discover_clicked), d);
  g_signal_connect (d->dc_list,  "row-activated",  G_CALLBACK (on_dc_row_activated), d);
  g_signal_connect (d->connect_btn,     "clicked", G_CALLBACK (on_connect_clicked), d);
  g_signal_connect_swapped (d->cancel_btn, "clicked",
                             G_CALLBACK (gtk_window_destroy), d->dialog);
  g_signal_connect (d->dialog, "destroy", G_CALLBACK (on_dialog_destroy), d);

  g_object_unref (builder);
  return d->dialog;
}
