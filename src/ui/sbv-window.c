#include "sbv-window.h"
#include "sbv-connect-dialog.h"
#include "sbv-password-dialog.h"
#include "sbv-users-panel.h"
#include "sbv-groups-panel.h"
#include "sbv-computers-panel.h"
#include "sbv-ldap-panel.h"

struct _SbvWindow {
  AdwApplicationWindow  parent;

  /* Template children */
  AdwToastOverlay      *toast_overlay;
  GtkStack             *content_stack;
  GtkListBox           *profiles_list;
  GtkListBox           *nav_list;
  GtkListBoxRow        *nav_users_row;
  GtkListBoxRow        *nav_groups_row;
  GtkListBoxRow        *nav_computers_row;
  GtkListBoxRow        *nav_ldap_row;
  GtkWidget            *nav_separator;
  GtkButton            *add_conn_btn;
  GtkSpinner           *spinner;

  /* Runtime state */
  GListStore           *profiles_store;  /* owned */
  SbvConnection        *conn;
  SbvProfile           *active_profile;
  SbvUsersPanel        *users_panel;
  SbvGroupsPanel       *groups_panel;
  SbvComputersPanel    *computers_panel;
  SbvLdapPanel         *ldap_panel;
};

G_DEFINE_TYPE (SbvWindow, sbv_window, ADW_TYPE_APPLICATION_WINDOW)

/* Forward declare */
void sbv_window_on_connected (SbvWindow *self, SbvConnection *conn, SbvProfile *profile);
static void rebuild_profiles_list (SbvWindow *self);
static void on_edit_profile_clicked (GtkButton *btn, gpointer user_data);

/* ── Profile row construction ───────────────────────────────────────────── */

/* Builds one sidebar profile row. The row is clickable to connect; a small
 * pencil button on the trailing edge opens the edit dialog. The button
 * consumes its own gesture so clicking it does not trigger row-activated. */
static GtkWidget *
make_profile_row (SbvWindow *self, SbvProfile *profile, SbvProfile *active)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-profile",
                           g_object_ref (profile), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (box, 7);
  gtk_widget_set_margin_bottom (box, 7);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 8);

  gboolean is_active = active &&
    g_str_equal (sbv_profile_get_name (profile) ?: "",
                 sbv_profile_get_name (active)  ?: "");

  GtkWidget *dot = gtk_image_new_from_icon_name (
    is_active ? "emblem-ok-symbolic" : "media-record-symbolic");
  gtk_widget_add_css_class (dot, is_active ? "success" : "dim-label");
  gtk_image_set_pixel_size (GTK_IMAGE (dot), 10);
  gtk_box_append (GTK_BOX (box), dot);

  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_set_hexpand (text_box, TRUE);

  const char *name = sbv_profile_get_name (profile);
  GtkWidget *name_lbl = gtk_label_new (name ? name : "(unnamed)");
  gtk_label_set_xalign (GTK_LABEL (name_lbl), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name_lbl), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (text_box), name_lbl);

  const char *host = sbv_profile_get_host (profile);
  if (host) {
    char *sub = g_strdup_printf ("%s:%d", host, sbv_profile_get_port (profile));
    GtkWidget *host_lbl = gtk_label_new (sub);
    g_free (sub);
    gtk_label_set_xalign (GTK_LABEL (host_lbl), 0);
    gtk_label_set_ellipsize (GTK_LABEL (host_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (host_lbl, "dim-label");
    gtk_widget_add_css_class (host_lbl, "caption");
    gtk_box_append (GTK_BOX (text_box), host_lbl);
  }

  gtk_box_append (GTK_BOX (box), text_box);

  GtkWidget *auth_lbl = gtk_label_new (
    sbv_profile_get_auth_type (profile) == SBV_AUTH_KERBEROS ? "KRB" : "PWD");
  gtk_widget_add_css_class (auth_lbl, "caption");
  gtk_widget_add_css_class (auth_lbl, "dim-label");
  gtk_box_append (GTK_BOX (box), auth_lbl);

  GtkWidget *edit_btn = gtk_button_new_from_icon_name ("document-edit-symbolic");
  gtk_widget_set_tooltip_text (edit_btn, "Edit connection");
  gtk_widget_add_css_class (edit_btn, "flat");
  gtk_widget_set_valign (edit_btn, GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (edit_btn), "sbv-profile",
                           g_object_ref (profile), g_object_unref);
  g_signal_connect (edit_btn, "clicked",
                    G_CALLBACK (on_edit_profile_clicked), self);
  gtk_box_append (GTK_BOX (box), edit_btn);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

static void
rebuild_profiles_list (SbvWindow *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->profiles_list))) != NULL)
    gtk_list_box_remove (self->profiles_list, child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->profiles_store));
  for (guint i = 0; i < n; i++) {
    SbvProfile *p = g_list_model_get_item (G_LIST_MODEL (self->profiles_store), i);
    gtk_list_box_append (self->profiles_list,
                          make_profile_row (self, p, self->active_profile));
    g_object_unref (p);
  }
}

/* ── Kerberos connect callback ──────────────────────────────────────────── */

typedef struct {
  SbvWindow  *window;   /* unowned */
  SbvProfile *profile;  /* owned   */
} KrbConnCtx;

static void
on_krb_connect_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  KrbConnCtx    *ctx  = user_data;
  GError        *err  = NULL;

  gtk_spinner_stop (ctx->window->spinner);

  if (!sbv_connection_connect_finish (conn, result, &err)) {
    sbv_window_show_toast (ctx->window, err->message);
    g_error_free (err);
    g_object_unref (conn);
  } else {
    sbv_window_on_connected (ctx->window, conn, ctx->profile);
    g_object_unref (conn);
  }

  g_object_unref (ctx->profile);
  g_free (ctx);
}

/* ── Connect via profile ────────────────────────────────────────────────── */

static void
on_password_connected (SbvConnection *conn, gpointer user_data)
{
  SbvWindow *self = SBV_WINDOW (user_data);
  sbv_window_on_connected (self, conn, self->active_profile);
}

static void
connect_to_profile (SbvWindow *self, SbvProfile *profile)
{
  g_set_object (&self->active_profile, profile);

  if (sbv_profile_get_auth_type (profile) == SBV_AUTH_KERBEROS) {
    KrbConnCtx *ctx = g_new0 (KrbConnCtx, 1);
    ctx->window  = self;
    ctx->profile = g_object_ref (profile);

    gtk_spinner_start (self->spinner);
    SbvConnection *conn = sbv_connection_new ();
    sbv_connection_connect_profile_async (conn, profile, NULL,
                                           NULL, on_krb_connect_done, ctx);
  } else {
    sbv_password_dialog_show (GTK_WINDOW (self), profile,
                               on_password_connected, self);
  }
}

/* ── Signal handlers ────────────────────────────────────────────────────── */

/* Edit-dialog completion: refresh the sidebar so the (possibly renamed)
 * row reflects the new state, and if the active connection's profile was
 * the one edited, surface a toast that changes apply on next connect. */
static void
on_edit_saved (SbvConnection *conn, SbvProfile *saved, gpointer user_data)
{
  (void) conn;
  SbvWindow *self = SBV_WINDOW (user_data);

  rebuild_profiles_list (self);

  if (saved && self->active_profile) {
    const char *active = sbv_profile_get_name (self->active_profile);
    const char *edited = sbv_profile_get_name (saved);
    if (active && edited && g_str_equal (active, edited)) {
      sbv_window_show_toast (self,
        "Connection changes will apply on next connect.");
    }
  }
}

static void
on_edit_profile_clicked (GtkButton *btn, gpointer user_data)
{
  SbvWindow  *self    = SBV_WINDOW (user_data);
  SbvProfile *profile = g_object_get_data (G_OBJECT (btn), "sbv-profile");
  if (!profile) return;

  GtkWidget *dialog = sbv_edit_dialog_new (GTK_WINDOW (self),
                                             self->profiles_store,
                                             profile,
                                             on_edit_saved, self);
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
on_profile_row_activated (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvWindow  *self    = SBV_WINDOW (user_data);
  SbvProfile *profile = g_object_get_data (G_OBJECT (row), "sbv-profile");
  if (profile)
    connect_to_profile (self, profile);
}

static void
on_nav_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvWindow *self = SBV_WINDOW (user_data);
  if (!row) return;

  if (row == self->nav_users_row)
    gtk_stack_set_visible_child_name (self->content_stack, "users");
  else if (row == self->nav_groups_row)
    gtk_stack_set_visible_child_name (self->content_stack, "groups");
  else if (row == self->nav_computers_row)
    gtk_stack_set_visible_child_name (self->content_stack, "computers");
  else if (row == self->nav_ldap_row)
    gtk_stack_set_visible_child_name (self->content_stack, "ldap");
}

static void
on_connected_from_dialog (SbvConnection *conn, SbvProfile *profile, gpointer user_data)
{
  SbvWindow *self = SBV_WINDOW (user_data);
  rebuild_profiles_list (self);
  sbv_window_on_connected (self, conn, profile);
}

static void
on_add_conn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvWindow *self   = SBV_WINDOW (user_data);
  GtkWidget *dialog = sbv_connect_dialog_new (GTK_WINDOW (self),
                                               self->profiles_store,
                                               NULL,
                                               on_connected_from_dialog, self);
  gtk_window_present (GTK_WINDOW (dialog));
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void
sbv_window_show_toast (SbvWindow *self, const char *message)
{
  adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (message));
}

void
sbv_window_on_connected (SbvWindow *self, SbvConnection *conn, SbvProfile *profile)
{
  g_set_object (&self->conn,           conn);
  g_set_object (&self->active_profile, profile);

  const char *host = sbv_connection_get_host (conn);
  char *title = g_strdup_printf ("Sambervise — %s", host ? host : "");
  gtk_window_set_title (GTK_WINDOW (self), title);
  g_free (title);

  gtk_widget_set_visible (self->nav_separator,       TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->nav_list), TRUE);

  rebuild_profiles_list (self);

  gtk_stack_set_visible_child_name (self->content_stack, "users");
  gtk_list_box_select_row (self->nav_list, self->nav_users_row);

  sbv_users_panel_load     (self->users_panel,     conn);
  sbv_groups_panel_load    (self->groups_panel,    conn);
  sbv_computers_panel_load (self->computers_panel, conn);
  sbv_ldap_panel_load      (self->ldap_panel,      conn);
}

/* ── GObject / template ─────────────────────────────────────────────────── */

static void
sbv_window_dispose (GObject *object)
{
  SbvWindow *self = SBV_WINDOW (object);
  g_clear_object (&self->profiles_store);
  g_clear_object (&self->conn);
  g_clear_object (&self->active_profile);
  G_OBJECT_CLASS (sbv_window_parent_class)->dispose (object);
}

static void
sbv_window_class_init (SbvWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = sbv_window_dispose;

  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  gtk_widget_class_set_template_from_resource (wc,
    "/org/ekm/sambervise/ui/window.ui");

  gtk_widget_class_bind_template_child (wc, SbvWindow, toast_overlay);
  gtk_widget_class_bind_template_child (wc, SbvWindow, content_stack);
  gtk_widget_class_bind_template_child (wc, SbvWindow, profiles_list);
  gtk_widget_class_bind_template_child (wc, SbvWindow, nav_list);
  gtk_widget_class_bind_template_child (wc, SbvWindow, nav_users_row);
  gtk_widget_class_bind_template_child (wc, SbvWindow, nav_groups_row);
  gtk_widget_class_bind_template_child (wc, SbvWindow, nav_computers_row);
  gtk_widget_class_bind_template_child (wc, SbvWindow, nav_ldap_row);
  gtk_widget_class_bind_template_child (wc, SbvWindow, nav_separator);
  gtk_widget_class_bind_template_child (wc, SbvWindow, add_conn_btn);
  gtk_widget_class_bind_template_child (wc, SbvWindow, spinner);
}

static void
sbv_window_init (SbvWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->users_panel     = SBV_USERS_PANEL     (sbv_users_panel_new ());
  self->groups_panel    = SBV_GROUPS_PANEL    (sbv_groups_panel_new ());
  self->computers_panel = SBV_COMPUTERS_PANEL (sbv_computers_panel_new ());
  self->ldap_panel      = SBV_LDAP_PANEL      (sbv_ldap_panel_new ());

  gtk_stack_add_named (self->content_stack, GTK_WIDGET (self->users_panel),     "users");
  gtk_stack_add_named (self->content_stack, GTK_WIDGET (self->groups_panel),    "groups");
  gtk_stack_add_named (self->content_stack, GTK_WIDGET (self->computers_panel), "computers");
  gtk_stack_add_named (self->content_stack, GTK_WIDGET (self->ldap_panel),      "ldap");

  g_signal_connect (self->add_conn_btn,  "clicked",
                    G_CALLBACK (on_add_conn_clicked), self);
  g_signal_connect (self->profiles_list, "row-activated",
                    G_CALLBACK (on_profile_row_activated), self);
  g_signal_connect (self->nav_list,      "row-selected",
                    G_CALLBACK (on_nav_row_selected), self);
}

GtkWidget *
sbv_window_new (GtkApplication *app, GListStore *profiles_store)
{
  SbvWindow *self = g_object_new (SBV_TYPE_WINDOW, "application", app, NULL);
  self->profiles_store = g_object_ref (profiles_store);
  rebuild_profiles_list (self);
  return GTK_WIDGET (self);
}
