#include "sbv-sites-panel.h"
#include "sbv-window.h"
#include "sbv-site-dialog.h"
#include "../backend/sbv-sites-backend.h"
#include "../backend/sbv-dn.h"

#include <adwaita.h>
#include <string.h>

struct _SbvSitesPanel {
  GtkBox         parent;

  SbvConnection *conn;
  SbvSitesInfo  *info;        /* as last listed, or NULL */
  guint          load_gen;    /* drops replies from an older load */

  GtkWidget *site_group;
  GtkWidget *site_status;
  GtkWidget *site_add_btn;
  GPtrArray *site_rows;       /* rows added to site_group */

  GtkWidget *subnet_group;
  GtkWidget *subnet_status;
  GtkWidget *subnet_add_btn;
  GPtrArray *subnet_rows;     /* rows added to subnet_group */
};

G_DEFINE_TYPE (SbvSitesPanel, sbv_sites_panel, GTK_TYPE_BOX)

/* Reply context: the panel plus the load it belongs to. */
typedef struct {
  SbvSitesPanel *self;   /* owned ref */
  guint          gen;
} LoadCtx;

/* ── Rows ───────────────────────────────────────────────────────────────── */

/* Removes the rows in `rows` from `group`. */
static void
clear_rows (GtkWidget *group, GPtrArray *rows)
{
  for (guint i = 0; i < rows->len; i++)
    adw_preferences_group_remove (ADW_PREFERENCES_GROUP (group),
                                  g_ptr_array_index (rows, i));
  g_ptr_array_set_size (rows, 0);
}

/* Shows `text` in a group's status line, or hides it for NULL. */
static void
set_status (GtkWidget *label, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text != NULL);
  if (error) {
    gtk_widget_remove_css_class (label, "dim-label");
    gtk_widget_add_css_class (label, "error");
  } else {
    gtk_widget_remove_css_class (label, "error");
    gtk_widget_add_css_class (label, "dim-label");
  }
}

/* Counts the subnets that map to `site_dn`. */
static guint
count_subnets (SbvSitesInfo *info, const char *site_dn)
{
  guint n = 0;
  for (guint i = 0; i < info->subnets->len; i++) {
    SbvSubnet *s = g_ptr_array_index (info->subnets, i);
    if (sbv_dn_equal (s->site_dn, site_dn)) n++;
  }
  return n;
}

/* Builds "dc1.example.com and dc2.example.com. 3 subnets." for a site
 * row. Caller frees. */
static char *
site_summary (SbvSitesInfo *info, const SbvSite *site)
{
  GString *s = g_string_new (NULL);
  guint    n = g_strv_length (site->servers);
  if (n == 0) {
    g_string_append (s, "No DCs");
  } else {
    guint shown = MIN (n, 3);
    for (guint i = 0; i < shown; i++) {
      if (i > 0) g_string_append (s, (i == shown - 1 && shown == n) ? " and " : ", ");
      g_string_append (s, site->servers[i]);
    }
    if (n > shown) g_string_append_printf (s, " and %u more", n - shown);
  }
  guint subnets = count_subnets (info, site->dn);
  if (subnets == 0)
    g_string_append (s, ". No subnets.");
  else
    g_string_append_printf (s, ". %u %s.", subnets, subnets == 1 ? "subnet" : "subnets");
  return g_string_free (s, FALSE);
}

/* Returns the name of the site at `dn`, or NULL when it isn't listed. */
static const char *
site_name (SbvSitesInfo *info, const char *dn)
{
  for (guint i = 0; dn && i < info->sites->len; i++) {
    SbvSite *s = g_ptr_array_index (info->sites, i);
    if (sbv_dn_equal (s->dn, dn)) return s->name;
  }
  return NULL;
}

/* Makes an activatable row with an escaped title and subtitle. */
static GtkWidget *
make_row (const char *title, const char *subtitle, const char *tooltip)
{
  GtkWidget *row = adw_action_row_new ();
  char      *t   = g_markup_escape_text (title ? title : "", -1);
  char      *sub = g_markup_escape_text (subtitle ? subtitle : "", -1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), t);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
  if (tooltip && *tooltip) gtk_widget_set_tooltip_text (row, tooltip);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row),
                             gtk_image_new_from_icon_name ("go-next-symbolic"));
  g_free (t);
  g_free (sub);
  return row;
}

/* ── Dialogs ────────────────────────────────────────────────────────────── */

static void reload (SbvSitesPanel *self);

/* Runs after a dialog creates, saves or deletes something. */
static void
on_dialog_done (const char *message, gpointer user_data)
{
  SbvSitesPanel *self = SBV_SITES_PANEL (user_data);
  GtkWidget     *win  = gtk_widget_get_ancestor (GTK_WIDGET (self), SBV_TYPE_WINDOW);
  if (win) sbv_window_show_toast (SBV_WINDOW (win), message);
  reload (self);
}

/* Returns the window to parent dialogs on. */
static GtkWindow *
parent_window (SbvSitesPanel *self)
{
  GtkWidget *top = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  return top ? GTK_WINDOW (top) : NULL;
}

/* Opens the site dialog for `site`, or a new one for NULL. */
static void
open_site (SbvSitesPanel *self, const SbvSite *site)
{
  if (!self->conn || !self->info) return;
  sbv_site_dialog_show (parent_window (self), self->conn, self->info->sites_dn, site,
                        site ? count_subnets (self->info, site->dn) : 0,
                        on_dialog_done, g_object_ref (self), g_object_unref);
}

/* Opens the subnet dialog for `subnet`, or a new one for NULL. */
static void
open_subnet (SbvSitesPanel *self, const SbvSubnet *subnet)
{
  if (!self->conn || !self->info) return;
  sbv_subnet_dialog_show (parent_window (self), self->conn, self->info->sites_dn, subnet,
                          self->info->sites, on_dialog_done, g_object_ref (self),
                          g_object_unref);
}

/* A site row was activated. */
static void
on_site_activated (AdwActionRow *row, gpointer user_data)
{
  SbvSitesPanel *self = SBV_SITES_PANEL (user_data);
  guint          i    = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "index"));
  if (self->info && i < self->info->sites->len)
    open_site (self, g_ptr_array_index (self->info->sites, i));
}

/* A subnet row was activated. */
static void
on_subnet_activated (AdwActionRow *row, gpointer user_data)
{
  SbvSitesPanel *self = SBV_SITES_PANEL (user_data);
  guint          i    = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "index"));
  if (self->info && i < self->info->subnets->len)
    open_subnet (self, g_ptr_array_index (self->info->subnets, i));
}

/* "+" on the Sites group. */
static void
on_site_add (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  open_site (SBV_SITES_PANEL (user_data), NULL);
}

/* "+" on the Subnets group. */
static void
on_subnet_add (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  open_subnet (SBV_SITES_PANEL (user_data), NULL);
}

/* ── Loading ────────────────────────────────────────────────────────────── */

/* Fills both groups from `self->info`. */
static void
fill_rows (SbvSitesPanel *self)
{
  SbvSitesInfo *info = self->info;

  set_status (self->site_status, info->sites->len == 0 ? "No sites" : NULL, FALSE);
  for (guint i = 0; i < info->sites->len; i++) {
    SbvSite   *s   = g_ptr_array_index (info->sites, i);
    char      *sum = site_summary (info, s);
    GtkWidget *row = make_row (s->name, sum, s->description);
    g_object_set_data (G_OBJECT (row), "index", GUINT_TO_POINTER (i));
    g_signal_connect (row, "activated", G_CALLBACK (on_site_activated), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->site_group), row);
    g_ptr_array_add (self->site_rows, row);
    g_free (sum);
  }

  set_status (self->subnet_status,
              info->subnets->len == 0
                ? "No subnets. Clients pick any DC until their subnet maps to a site."
                : NULL, FALSE);
  for (guint i = 0; i < info->subnets->len; i++) {
    SbvSubnet  *s    = g_ptr_array_index (info->subnets, i);
    const char *site = site_name (info, s->site_dn);
    char       *sub  = site ? g_strdup_printf ("Site %s", site)
                            : g_strdup ("No site. Clients here pick any DC.");
    GtkWidget  *row  = make_row (s->name, sub, s->description);
    if (!site) {
      GtkWidget *icon = gtk_image_new_from_icon_name ("dialog-warning-symbolic");
      gtk_widget_add_css_class (icon, "warning");
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
    }
    g_object_set_data (G_OBJECT (row), "index", GUINT_TO_POINTER (i));
    g_signal_connect (row, "activated", G_CALLBACK (on_subnet_activated), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->subnet_group), row);
    g_ptr_array_add (self->subnet_rows, row);
    g_free (sub);
  }
}

/* List completion. */
static void
on_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadCtx       *ctx  = user_data;
  SbvSitesPanel *self = ctx->self;
  GError        *err  = NULL;

  SbvSitesInfo *info = sbv_sites_list_finish (SBV_CONNECTION (source), result, &err);
  if (ctx->gen != self->load_gen) {
    sbv_sites_info_free (info);
    g_clear_error (&err);
  } else if (!info) {
    set_status (self->site_status, err->message, TRUE);
    set_status (self->subnet_status, NULL, FALSE);
    g_error_free (err);
  } else {
    clear_rows (self->site_group,   self->site_rows);
    clear_rows (self->subnet_group, self->subnet_rows);
    sbv_sites_info_free (self->info);
    self->info = info;
    gtk_widget_set_sensitive (self->site_add_btn,   TRUE);
    gtk_widget_set_sensitive (self->subnet_add_btn, TRUE);
    fill_rows (self);
  }
  g_object_unref (ctx->self);
  g_free (ctx);
}

/* Starts a fresh read. Rows stay until the reply arrives. */
static void
reload (SbvSitesPanel *self)
{
  if (!self->conn) return;
  LoadCtx *ctx = g_new0 (LoadCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->gen  = ++self->load_gen;
  sbv_sites_list_async (self->conn, NULL, on_loaded, ctx);
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

/* Builds a group with a "+" button and a status line. */
static GtkWidget *
build_group (SbvSitesPanel *self, const char *title, const char *description,
             const char *add_tooltip, GCallback on_add,
             GtkWidget **add_btn, GtkWidget **status)
{
  GtkWidget *g = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g), title);
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (g), description);

  *add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (*add_btn, "flat");
  gtk_widget_set_valign (*add_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (*add_btn, add_tooltip);
  gtk_widget_set_sensitive (*add_btn, FALSE);
  g_signal_connect (*add_btn, "clicked", on_add, self);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (g), *add_btn);

  *status = gtk_label_new ("Loading…");
  gtk_label_set_wrap (GTK_LABEL (*status), TRUE);
  gtk_label_set_xalign (GTK_LABEL (*status), 0);
  gtk_widget_add_css_class (*status, "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g), *status);
  return g;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

GtkWidget *
sbv_sites_panel_new (void)
{
  return g_object_new (SBV_TYPE_SITES_PANEL, NULL);
}

void
sbv_sites_panel_load (SbvSitesPanel *self, SbvConnection *conn)
{
  g_return_if_fail (SBV_IS_SITES_PANEL (self));
  g_set_object (&self->conn, conn);
  if (!conn) return;

  clear_rows (self->site_group,   self->site_rows);
  clear_rows (self->subnet_group, self->subnet_rows);
  g_clear_pointer (&self->info, sbv_sites_info_free);
  gtk_widget_set_sensitive (self->site_add_btn,   FALSE);
  gtk_widget_set_sensitive (self->subnet_add_btn, FALSE);
  set_status (self->site_status,   "Loading…", FALSE);
  set_status (self->subnet_status, "Loading…", FALSE);
  reload (self);
}

/* ── GObject ────────────────────────────────────────────────────────────── */

/* Drops the connection, the listed data and the row lists. */
static void
sbv_sites_panel_finalize (GObject *object)
{
  SbvSitesPanel *self = SBV_SITES_PANEL (object);
  g_clear_object (&self->conn);
  sbv_sites_info_free (self->info);
  g_ptr_array_unref (self->site_rows);
  g_ptr_array_unref (self->subnet_rows);
  G_OBJECT_CLASS (sbv_sites_panel_parent_class)->finalize (object);
}

/* Registers finalize. */
static void
sbv_sites_panel_class_init (SbvSitesPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_sites_panel_finalize;
}

/* Builds the scrolling page: sites, then subnets. */
static void
sbv_sites_panel_init (SbvSitesPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  self->site_rows   = g_ptr_array_new ();
  self->subnet_rows = g_ptr_array_new ();

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 720);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
  gtk_widget_set_margin_top    (vbox, 24);
  gtk_widget_set_margin_bottom (vbox, 24);
  gtk_widget_set_margin_start  (vbox, 12);
  gtk_widget_set_margin_end    (vbox, 12);

  self->site_group = build_group (self, "Sites",
    "Each site groups DCs and clients that share a fast network",
    "New site", G_CALLBACK (on_site_add), &self->site_add_btn, &self->site_status);
  self->subnet_group = build_group (self, "Subnets",
    "Clients find the DCs in their site from the subnet they are in",
    "New subnet", G_CALLBACK (on_subnet_add), &self->subnet_add_btn, &self->subnet_status);
  gtk_box_append (GTK_BOX (vbox), self->site_group);
  gtk_box_append (GTK_BOX (vbox), self->subnet_group);

  adw_clamp_set_child (ADW_CLAMP (clamp), vbox);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  gtk_box_append (GTK_BOX (self), scroll);
}
