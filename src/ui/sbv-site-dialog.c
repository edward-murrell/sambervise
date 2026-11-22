#include "sbv-site-dialog.h"
#include "../backend/sbv-sites-backend.h"

#include <adwaita.h>
#include <string.h>

/* State for one open site or subnet dialog. Freed when the window goes. */
typedef struct {
  gboolean       is_subnet;
  GtkWidget     *win;
  SbvConnection *conn;
  char          *sites_dn;
  gboolean       busy;

  /* What the dialog opened with. `dn` is NULL for a new object. */
  char          *dn;
  char          *name;
  char          *description;
  char          *site_dn;       /* subnets only */
  guint          n_subnets;     /* sites only */
  char         **site_dns;      /* subnets only: DN per combo item, item 0 is none */

  GtkWidget *name_row;          /* AdwEntryRow, or NULL for an existing subnet */
  GtkWidget *desc_row;
  GtkWidget *site_combo;        /* subnets only */
  GtkWidget *error_label;
  GtkWidget *spinner;
  GtkWidget *save_btn;
  GtkWidget *cancel_btn;
  GtkWidget *delete_btn;

  SbvSiteDoneFn  done;
  gpointer       done_data;
  GDestroyNotify done_free;
} SiteDlg;

/* Frees the dialog state. */
static void
site_dlg_free (SiteDlg *d)
{
  if (d->done_free) d->done_free (d->done_data);
  g_clear_object (&d->conn);
  g_free (d->sites_dn);
  g_free (d->dn);
  g_free (d->name);
  g_free (d->description);
  g_free (d->site_dn);
  g_strfreev (d->site_dns);
  g_free (d);
}

/* Shows `msg` under the form, or hides the label when NULL. */
static void
show_error (SiteDlg *d, const char *msg)
{
  gtk_label_set_text (GTK_LABEL (d->error_label), msg ? msg : "");
  gtk_widget_set_visible (d->error_label, msg != NULL);
}

/* Locks the dialog while a request runs. */
static void
set_busy (SiteDlg *d, gboolean busy)
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
  return ((SiteDlg *) user_data)->busy;
}

/* Returns the stripped text of an entry row. Caller frees. */
static char *
row_text (GtkWidget *row)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (row))));
}

/* ── Save and delete ────────────────────────────────────────────────────── */

typedef struct {
  SiteDlg    *d;
  const char *message;   /* static toast text */
} OpCtx;

/* Completion for any change: close and report, or show the error. */
static void
on_op_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OpCtx   *ctx = user_data;
  SiteDlg *d   = ctx->d;
  GError  *err = NULL;
  gboolean ok  = sbv_sites_op_finish (SBV_CONNECTION (source), result, &err);
  set_busy (d, FALSE);
  if (!ok) {
    show_error (d, err->message);
    g_error_free (err);
  } else {
    if (d->done) d->done (ctx->message, d->done_data);
    gtk_window_destroy (GTK_WINDOW (d->win));
  }
  g_free (ctx);
}

/* Makes the completion context for a change. */
static OpCtx *
op_ctx (SiteDlg *d, const char *message)
{
  OpCtx *ctx = g_new0 (OpCtx, 1);
  ctx->d       = d;
  ctx->message = message;
  return ctx;
}

/* Save for a site: check a new or changed name, then create or update. */
static void
save_site (SiteDlg *d)
{
  char *name = row_text (d->name_row);
  char *desc = row_text (d->desc_row);
  char *msg  = NULL;
  if (!d->dn || g_strcmp0 (name, d->name) != 0)
    msg = sbv_site_name_check (name);
  if (msg) {
    show_error (d, msg);
  } else {
    show_error (d, NULL);
    set_busy (d, TRUE);
    if (d->dn)
      sbv_site_update_async (d->conn, d->dn, d->name, name, d->description, desc,
                             NULL, on_op_done, op_ctx (d, "Site saved"));
    else
      sbv_site_create_async (d->conn, d->sites_dn, name, desc, NULL, on_op_done,
                             op_ctx (d, "Site created"));
  }
  g_free (msg);
  g_free (name);
  g_free (desc);
}

/* Save for a subnet: check a new name, then create or update. */
static void
save_subnet (SiteDlg *d)
{
  SbvSubnet s = { 0 };
  guint     i = adw_combo_row_get_selected (ADW_COMBO_ROW (d->site_combo));
  s.dn          = d->dn;
  s.site_dn     = (i != GTK_INVALID_LIST_POSITION && i > 0) ? d->site_dns[i] : NULL;
  s.description = row_text (d->desc_row);

  char *msg = NULL;
  if (!d->dn)
    s.name = sbv_subnet_normalise (gtk_editable_get_text (GTK_EDITABLE (d->name_row)), &msg);

  if (msg) {
    show_error (d, msg);
  } else {
    show_error (d, NULL);
    set_busy (d, TRUE);
    if (d->dn) {
      SbvSubnet o = { .dn = d->dn, .name = d->name, .description = d->description,
                      .site_dn = d->site_dn };
      sbv_subnet_update_async (d->conn, &o, &s, NULL, on_op_done,
                               op_ctx (d, "Subnet saved"));
    } else {
      sbv_subnet_create_async (d->conn, d->sites_dn, &s, NULL, on_op_done,
                               op_ctx (d, "Subnet created"));
    }
  }
  g_free (msg);
  g_free (s.name);
  g_free (s.description);
}

/* Save or Create. */
static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SiteDlg *d = user_data;
  if (d->is_subnet) save_subnet (d);
  else              save_site (d);
}

/* Confirmation reply for Delete. */
static void
on_delete_response (AdwMessageDialog *dlg, const char *response, gpointer user_data)
{
  (void) dlg;
  SiteDlg *d = user_data;
  if (g_strcmp0 (response, "delete") != 0) return;
  show_error (d, NULL);
  set_busy (d, TRUE);
  if (d->is_subnet)
    sbv_subnet_delete_async (d->conn, d->dn, NULL, on_op_done, op_ctx (d, "Subnet deleted"));
  else
    sbv_site_delete_async (d->conn, d->dn, NULL, on_op_done, op_ctx (d, "Site deleted"));
}

/* Delete: ask first. */
static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SiteDlg *d = user_data;
  char    *body;
  if (d->is_subnet)
    body = g_strdup ("Clients in this subnet will no longer find their nearest DC "
                     "through it.");
  else if (d->n_subnets > 0)
    body = g_strdup_printf ("%u %s will no longer map to a site.", d->n_subnets,
                            d->n_subnets == 1 ? "subnet" : "subnets");
  else
    body = g_strdup ("No subnets map to this site.");

  char      *heading = g_strdup_printf ("Delete %s %s?",
                                        d->is_subnet ? "Subnet" : "Site", d->name);
  GtkWidget *dlg     = adw_message_dialog_new (GTK_WINDOW (d->win), heading, body);
  g_free (heading);
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

/* Window "destroy": free the dialog state. */
static void
on_dialog_destroy (GtkWidget *w, gpointer user_data)
{
  (void) w;
  site_dlg_free (user_data);
}

/* Makes an entry row with `title` and `text`. */
static GtkWidget *
entry_row (const char *title, const char *text)
{
  GtkWidget *row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), text ? text : "");
  return row;
}

/* Builds the window, header bar and body box. Returns the body box, which
 * the caller fills before calling present_dialog. */
static GtkWidget *
build_window (SiteDlg *d, GtkWindow *parent, const char *title)
{
  d->win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (d->win), title);
  gtk_window_set_modal (GTK_WINDOW (d->win), TRUE);
  if (parent) gtk_window_set_transient_for (GTK_WINDOW (d->win), parent);
  gtk_window_set_default_size (GTK_WINDOW (d->win), 480, -1);

  GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  d->cancel_btn = gtk_button_new_with_label ("Cancel");
  g_signal_connect_swapped (d->cancel_btn, "clicked", G_CALLBACK (gtk_window_destroy), d->win);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), d->cancel_btn);
  d->save_btn = gtk_button_new_with_label (d->dn ? "Save" : "Create");
  gtk_widget_add_css_class (d->save_btn, "suggested-action");
  g_signal_connect (d->save_btn, "clicked", G_CALLBACK (on_save_clicked), d);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), d->save_btn);
  d->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (d->spinner, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), d->spinner);
  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top    (body, 18);
  gtk_widget_set_margin_bottom (body, 18);
  gtk_widget_set_margin_start  (body, 12);
  gtk_widget_set_margin_end    (body, 12);
  gtk_box_append (GTK_BOX (vbox), body);
  gtk_window_set_child (GTK_WINDOW (d->win), vbox);
  return body;
}

/* Adds the error label and, for an existing object, the Delete button,
 * then shows the window. `can_delete` FALSE disables Delete with `why`
 * as its tooltip. */
static void
present_dialog (SiteDlg *d, GtkWidget *body, const char *delete_label,
                gboolean can_delete, const char *why)
{
  d->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (d->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (d->error_label), 0);
  gtk_widget_add_css_class (d->error_label, "error");
  gtk_widget_set_visible (d->error_label, FALSE);
  gtk_box_append (GTK_BOX (body), d->error_label);

  if (d->dn) {
    d->delete_btn = gtk_button_new_with_label (delete_label);
    gtk_widget_add_css_class (d->delete_btn, "destructive-action");
    gtk_widget_set_halign (d->delete_btn, GTK_ALIGN_START);
    g_signal_connect (d->delete_btn, "clicked", G_CALLBACK (on_delete_clicked), d);
    gtk_box_append (GTK_BOX (body), d->delete_btn);
    if (!can_delete) {
      gtk_widget_set_sensitive (d->delete_btn, FALSE);
      GtkWidget *hint = gtk_label_new (why);
      gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
      gtk_label_set_xalign (GTK_LABEL (hint), 0);
      gtk_widget_add_css_class (hint, "dim-label");
      gtk_box_append (GTK_BOX (body), hint);
    }
  }

  g_signal_connect (d->win, "close-request", G_CALLBACK (on_close_request), d);
  g_signal_connect (d->win, "destroy", G_CALLBACK (on_dialog_destroy), d);
  gtk_window_present (GTK_WINDOW (d->win));
  gtk_widget_grab_focus (d->name_row ? d->name_row : d->desc_row);
}

/* Makes the shared dialog state. */
static SiteDlg *
site_dlg_new (SbvConnection *conn, const char *sites_dn, SbvSiteDoneFn done,
              gpointer user_data, GDestroyNotify user_data_free)
{
  SiteDlg *d   = g_new0 (SiteDlg, 1);
  d->conn      = g_object_ref (conn);
  d->sites_dn  = g_strdup (sites_dn);
  d->done      = done;
  d->done_data = user_data;
  d->done_free = user_data_free;
  return d;
}

void
sbv_site_dialog_show (GtkWindow *parent, SbvConnection *conn, const char *sites_dn,
                      const SbvSite *site, guint n_subnets, SbvSiteDoneFn done,
                      gpointer user_data, GDestroyNotify user_data_free)
{
  g_return_if_fail (conn && sites_dn);
  SiteDlg *d = site_dlg_new (conn, sites_dn, done, user_data, user_data_free);
  if (site) {
    d->dn          = g_strdup (site->dn);
    d->name        = g_strdup (site->name);
    d->description = g_strdup (site->description);
  }
  d->n_subnets = n_subnets;

  GtkWidget *body = build_window (d, parent, site ? "Edit Site" : "New Site");

  GtkWidget *g = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g), "General");
  if (!site)
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (g),
      "Group DCs and clients that share a fast network");
  d->name_row = entry_row ("Name", d->name);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->name_row);
  d->desc_row = entry_row ("Description", d->description);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->desc_row);
  gtk_box_append (GTK_BOX (body), g);

  guint n_dcs = site && site->servers ? g_strv_length (site->servers) : 0;
  if (site) {
    GtkWidget *dcs = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (dcs), "Domain Controllers");
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (dcs),
      "A DC's site is set when it joins the domain");
    if (n_dcs == 0) {
      GtkWidget *none = gtk_label_new ("No DCs in this site");
      gtk_label_set_xalign (GTK_LABEL (none), 0);
      gtk_widget_add_css_class (none, "dim-label");
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (dcs), none);
    }
    for (guint i = 0; i < n_dcs; i++) {
      GtkWidget *row = adw_action_row_new ();
      char      *t   = g_markup_escape_text (site->servers[i], -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), t);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row),
                                 gtk_image_new_from_icon_name ("network-server-symbolic"));
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (dcs), row);
      g_free (t);
    }
    gtk_box_append (GTK_BOX (body), dcs);
  }

  present_dialog (d, body, "Delete Site", n_dcs == 0,
                  "Move this site's DCs to another site before deleting it.");
}

void
sbv_subnet_dialog_show (GtkWindow *parent, SbvConnection *conn, const char *sites_dn,
                        const SbvSubnet *subnet, GPtrArray *sites, SbvSiteDoneFn done,
                        gpointer user_data, GDestroyNotify user_data_free)
{
  g_return_if_fail (conn && sites_dn);
  SiteDlg *d   = site_dlg_new (conn, sites_dn, done, user_data, user_data_free);
  d->is_subnet = TRUE;
  if (subnet) {
    d->dn          = g_strdup (subnet->dn);
    d->name        = g_strdup (subnet->name);
    d->description = g_strdup (subnet->description);
    d->site_dn     = g_strdup (subnet->site_dn);
  }

  GtkWidget *body = build_window (d, parent, subnet ? "Edit Subnet" : "New Subnet");
  GtkWidget *g    = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g), "General");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (g),
    "Clients in this subnet use the DCs in its site first");

  if (subnet) {
    GtkWidget *row = adw_action_row_new ();
    char      *t   = g_markup_escape_text (subnet->name ? subnet->name : "", -1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Subnet");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), t);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), row);
    g_free (t);
  } else {
    d->name_row = entry_row ("Subnet, such as 10.1.0.0/16", NULL);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->name_row);
  }

  /* Item 0 is "None". The rest follow `sites`. */
  guint          n     = sites ? sites->len : 0;
  GtkStringList *names = gtk_string_list_new (NULL);
  guint          pick  = 0;
  d->site_dns = g_new0 (char *, n + 2);
  d->site_dns[0] = g_strdup ("");
  gtk_string_list_append (names, "None");
  for (guint i = 0; i < n; i++) {
    SbvSite *s = g_ptr_array_index (sites, i);
    gtk_string_list_append (names, s->name ? s->name : s->dn);
    d->site_dns[i + 1] = g_strdup (s->dn);
    if (d->site_dn && g_ascii_strcasecmp (d->site_dn, s->dn) == 0) pick = i + 1;
  }
  /* A new subnet defaults to the only site, when there is one. */
  if (!subnet && n == 1) pick = 1;

  d->site_combo = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (d->site_combo), "Site");
  adw_combo_row_set_model (ADW_COMBO_ROW (d->site_combo), G_LIST_MODEL (names));
  adw_combo_row_set_selected (ADW_COMBO_ROW (d->site_combo), pick);
  g_object_unref (names);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->site_combo);

  d->desc_row = entry_row ("Description", d->description);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), d->desc_row);
  gtk_box_append (GTK_BOX (body), g);

  present_dialog (d, body, "Delete Subnet", TRUE, NULL);
}
