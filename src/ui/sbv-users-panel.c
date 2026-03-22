#include "sbv-users-panel.h"
#include "../backend/sbv-users-backend.h"
#include "../model/sbv-user.h"

#include <adwaita.h>

struct _SbvUsersPanel {
  GtkBox         parent;

  GtkWidget     *stack;          /* "loading" | "empty" | "list" */
  GtkWidget     *search_entry;
  GtkWidget     *list_box;
  GtkWidget     *spinner;

  SbvConnection *conn;           /* unowned ref */
  char          *filter_text;    /* current search string, lowercase */
};

G_DEFINE_TYPE (SbvUsersPanel, sbv_users_panel, GTK_TYPE_BOX)

/* ── Row helpers ────────────────────────────────────────────────────────── */

static GtkWidget *
make_user_row (SbvUser *user)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-user",
                           g_object_ref (user), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top    (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *icon = gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text_box, TRUE);

  const char *display = sbv_user_get_display_name (user);
  const char *sam     = sbv_user_get_sam (user);

  GtkWidget *name_label = gtk_label_new (display ? display : sam);
  gtk_label_set_xalign (GTK_LABEL (name_label), 0);
  gtk_widget_add_css_class (name_label, "heading");
  gtk_box_append (GTK_BOX (text_box), name_label);

  if (sam) {
    GtkWidget *sam_label = gtk_label_new (sam);
    gtk_label_set_xalign (GTK_LABEL (sam_label), 0);
    gtk_widget_add_css_class (sam_label, "dim-label");
    gtk_box_append (GTK_BOX (text_box), sam_label);
  }

  gtk_box_append (GTK_BOX (box), text_box);

  gboolean enabled = sbv_user_get_enabled (user);
  GtkWidget *status = gtk_label_new (enabled ? "Enabled" : "Disabled");
  gtk_widget_add_css_class (status, enabled ? "success" : "error");
  gtk_box_append (GTK_BOX (box), status);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* ── Filter function ────────────────────────────────────────────────────── */

static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->filter_text || !*self->filter_text)
    return TRUE;

  SbvUser *user = g_object_get_data (G_OBJECT (row), "sbv-user");
  if (!user) return TRUE;

  const char *dn   = sbv_user_get_display_name (user);
  const char *sam  = sbv_user_get_sam (user);
  const char *mail = sbv_user_get_email (user);

  gboolean match = FALSE;

  if (dn) {
    char *lower = g_utf8_strdown (dn, -1);
    match = strstr (lower, self->filter_text) != NULL;
    g_free (lower);
  }
  if (!match && sam) {
    char *lower = g_utf8_strdown (sam, -1);
    match = strstr (lower, self->filter_text) != NULL;
    g_free (lower);
  }
  if (!match && mail) {
    char *lower = g_utf8_strdown (mail, -1);
    match = strstr (lower, self->filter_text) != NULL;
    g_free (lower);
  }
  return match;
}

/* ── Search changed ─────────────────────────────────────────────────────── */

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  g_free (self->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  self->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* ── Context menu: enable / disable ────────────────────────────────────── */

typedef struct {
  SbvUsersPanel *panel;
  SbvUser       *user;
  gboolean       enable;
} ActionData;

static void
on_set_enabled_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ActionData    *ad   = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  if (!sbv_users_set_enabled_finish (conn, result, &err)) {
    g_warning ("set_enabled failed: %s", err->message);
    g_error_free (err);
  } else {
    /* Refresh the panel */
    sbv_users_panel_load (ad->panel, conn);
  }

  g_object_unref (ad->user);
  g_free (ad);
}

static void
on_popover_action (GtkButton *btn, gpointer user_data)
{
  ActionData *ad = user_data;
  gtk_widget_activate_action (GTK_WIDGET (btn), "win.close-popover", NULL);
  sbv_users_set_enabled_async (ad->panel->conn, ad->user, ad->enable,
                                NULL, on_set_enabled_done, ad);
}

static void
show_row_popover (SbvUsersPanel *self, GtkListBoxRow *row, double x, double y)
{
  SbvUser *user = g_object_get_data (G_OBJECT (row), "sbv-user");
  if (!user || !self->conn) return;

  GtkWidget *popover = gtk_popover_new ();
  gtk_widget_set_parent (popover, GTK_WIDGET (row));

  /* Position near the click */
  GdkRectangle rect = { (int)x, (int)y, 1, 1 };
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gboolean enabled = sbv_user_get_enabled (user);

  ActionData *ad = g_new0 (ActionData, 1);
  ad->panel  = self;
  ad->user   = g_object_ref (user);
  ad->enable = !enabled;

  GtkWidget *toggle_btn = gtk_button_new_with_label (
    enabled ? "Disable Account" : "Enable Account");
  gtk_widget_add_css_class (toggle_btn, "flat");
  g_signal_connect_data (toggle_btn, "clicked",
                          G_CALLBACK (on_popover_action),
                          ad, (GClosureNotify) NULL, 0);
  g_signal_connect_swapped (toggle_btn, "clicked",
                             G_CALLBACK (gtk_popover_popdown), popover);
  gtk_box_append (GTK_BOX (vbox), toggle_btn);

  gtk_popover_set_child (GTK_POPOVER (popover), vbox);
  gtk_popover_popup (GTK_POPOVER (popover));
}

static void
on_row_activated (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  show_row_popover (self, row, 0, 0);
}

/* ── Load callback ──────────────────────────────────────────────────────── */

static void
on_users_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  GError        *err  = NULL;

  GListStore *store = sbv_users_list_finish (SBV_CONNECTION (source), result, &err);
  if (!store) {
    g_warning ("Failed to load users: %s", err->message);
    g_error_free (err);
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
    return;
  }

  /* Remove existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    SbvUser *user = g_list_model_get_item (G_LIST_MODEL (store), i);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), make_user_row (user));
    g_object_unref (user);
  }

  g_object_unref (store);

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack),
                                     n == 0 ? "empty" : "list");
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void
sbv_users_panel_load (SbvUsersPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "loading");
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  sbv_users_list_async (conn, NULL, on_users_loaded, self);
}

/* ── GObject init ───────────────────────────────────────────────────────── */

static void
sbv_users_panel_finalize (GObject *object)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (object);
  g_free (self->filter_text);
  G_OBJECT_CLASS (sbv_users_panel_parent_class)->finalize (object);
}

static void
sbv_users_panel_class_init (SbvUsersPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_users_panel_finalize;
}

static void
sbv_users_panel_init (SbvUsersPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);

  /* Toolbar row: search + spinner */
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top    (toolbar, 6);
  gtk_widget_set_margin_bottom (toolbar, 6);
  gtk_widget_set_margin_start  (toolbar, 8);
  gtk_widget_set_margin_end    (toolbar, 8);

  self->search_entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (self->search_entry, TRUE);
  gtk_widget_set_tooltip_text (self->search_entry, "Search users");
  g_signal_connect (self->search_entry, "search-changed",
                    G_CALLBACK (on_search_changed), self);
  gtk_box_append (GTK_BOX (toolbar), self->search_entry);

  self->spinner = gtk_spinner_new ();
  gtk_box_append (GTK_BOX (toolbar), self->spinner);

  gtk_box_append (GTK_BOX (self), toolbar);
  gtk_box_append (GTK_BOX (self), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  /* Stack: loading / empty / list */
  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);

  /* Loading page */
  GtkWidget *loading_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (loading_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (loading_box, GTK_ALIGN_CENTER);
  GtkWidget *big_spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (big_spinner));
  gtk_box_append (GTK_BOX (loading_box), big_spinner);
  gtk_stack_add_named (GTK_STACK (self->stack), loading_box, "loading");

  /* Empty page */
  GtkWidget *empty_page = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty_page),
                                  "system-users-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), "No Users Found");
  gtk_stack_add_named (GTK_STACK (self->stack), empty_page, "empty");

  /* List page */
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scrolled, TRUE);
  self->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list_box),
                                    GTK_SELECTION_SINGLE);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (self->list_box),
                                 filter_func, self, NULL);
  gtk_widget_add_css_class (self->list_box, "boxed-list");
  g_signal_connect (self->list_box, "row-activated",
                    G_CALLBACK (on_row_activated), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), self->list_box);
  gtk_stack_add_named (GTK_STACK (self->stack), scrolled, "list");

  gtk_box_append (GTK_BOX (self), self->stack);
}

GtkWidget *
sbv_users_panel_new (void)
{
  return g_object_new (SBV_TYPE_USERS_PANEL, NULL);
}
