#include "sbv-groups-panel.h"
#include "../backend/sbv-groups-backend.h"
#include "../model/sbv-group.h"

#include <adwaita.h>

struct _SbvGroupsPanel {
  GtkBox         parent;

  GtkWidget     *stack;         /* "loading" | "empty" | "split" */
  GtkWidget     *search_entry;
  GtkWidget     *list_box;
  GtkWidget     *detail_stack;  /* "none" | "members" */
  GtkWidget     *members_list;
  GtkWidget     *detail_title;

  SbvConnection *conn;
  char          *filter_text;
};

G_DEFINE_TYPE (SbvGroupsPanel, sbv_groups_panel, GTK_TYPE_BOX)

/* ── Row helpers ────────────────────────────────────────────────────────── */

static GtkWidget *
make_group_row (SbvGroup *group)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-group",
                           g_object_ref (group), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top    (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *icon = gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text_box, TRUE);

  const char *display = sbv_group_get_display_name (group);
  const char *sam     = sbv_group_get_sam (group);

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

  guint count = sbv_group_get_member_count (group);
  char *count_str = g_strdup_printf ("%u member%s", count, count == 1 ? "" : "s");
  GtkWidget *count_label = gtk_label_new (count_str);
  g_free (count_str);
  gtk_widget_add_css_class (count_label, "dim-label");
  gtk_box_append (GTK_BOX (box), count_label);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* ── Filter ─────────────────────────────────────────────────────────────── */

static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  if (!self->filter_text || !*self->filter_text)
    return TRUE;

  SbvGroup *group = g_object_get_data (G_OBJECT (row), "sbv-group");
  if (!group) return TRUE;

  const char *dn  = sbv_group_get_display_name (group);
  const char *sam = sbv_group_get_sam (group);

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
  return match;
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  g_free (self->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  self->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* ── Selection: show members ────────────────────────────────────────────── */

static void
on_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);

  if (!row) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    return;
  }

  SbvGroup *group = g_object_get_data (G_OBJECT (row), "sbv-group");
  if (!group) return;

  const char *display = sbv_group_get_display_name (group);
  const char *sam     = sbv_group_get_sam (group);
  gtk_label_set_text (GTK_LABEL (self->detail_title),
                       display ? display : (sam ? sam : ""));

  /* Clear and repopulate member list */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->members_list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->members_list), child);

  const char * const *members = sbv_group_get_members (group);
  if (members) {
    for (int i = 0; members[i]; i++) {
      GtkWidget *member_row = gtk_list_box_row_new ();
      gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (member_row), FALSE);

      GtkWidget *label = gtk_label_new (members[i]);
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_set_margin_top    (label, 6);
      gtk_widget_set_margin_bottom (label, 6);
      gtk_widget_set_margin_start  (label, 12);
      gtk_widget_set_margin_end    (label, 12);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (member_row), label);
      gtk_list_box_append (GTK_LIST_BOX (self->members_list), member_row);
    }
  }

  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "members");
}

/* ── Load ───────────────────────────────────────────────────────────────── */

static void
on_groups_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  GError         *err  = NULL;

  GListStore *store = sbv_groups_list_finish (SBV_CONNECTION (source), result, &err);
  if (!store) {
    g_warning ("Failed to load groups: %s", err->message);
    g_error_free (err);
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
    return;
  }

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    SbvGroup *group = g_list_model_get_item (G_LIST_MODEL (store), i);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), make_group_row (group));
    g_object_unref (group);
  }

  g_object_unref (store);
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack),
                                     n == 0 ? "empty" : "split");
}

void
sbv_groups_panel_load (SbvGroupsPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "loading");
  sbv_groups_list_async (conn, NULL, on_groups_loaded, self);
}

/* ── GObject init ───────────────────────────────────────────────────────── */

static void
sbv_groups_panel_finalize (GObject *object)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (object);
  g_free (self->filter_text);
  G_OBJECT_CLASS (sbv_groups_panel_parent_class)->finalize (object);
}

static void
sbv_groups_panel_class_init (SbvGroupsPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_groups_panel_finalize;
}

static void
sbv_groups_panel_init (SbvGroupsPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);

  /* Search toolbar */
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top    (toolbar, 6);
  gtk_widget_set_margin_bottom (toolbar, 6);
  gtk_widget_set_margin_start  (toolbar, 8);
  gtk_widget_set_margin_end    (toolbar, 8);

  self->search_entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (self->search_entry, TRUE);
  gtk_widget_set_tooltip_text (self->search_entry, "Search groups");
  g_signal_connect (self->search_entry, "search-changed",
                    G_CALLBACK (on_search_changed), self);
  gtk_box_append (GTK_BOX (toolbar), self->search_entry);
  gtk_box_append (GTK_BOX (self), toolbar);
  gtk_box_append (GTK_BOX (self), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  /* Outer stack */
  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);

  /* Loading */
  GtkWidget *loading_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (loading_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (loading_box, GTK_ALIGN_CENTER);
  GtkWidget *big_spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (big_spinner));
  gtk_box_append (GTK_BOX (loading_box), big_spinner);
  gtk_stack_add_named (GTK_STACK (self->stack), loading_box, "loading");

  /* Empty */
  GtkWidget *empty_page = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty_page),
                                  "avatar-default-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), "No Groups Found");
  gtk_stack_add_named (GTK_STACK (self->stack), empty_page, "empty");

  /* Split: group list + member detail */
  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);

  GtkWidget *left_scroll = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (left_scroll, 260, -1);
  self->list_box = gtk_list_box_new ();
  gtk_list_box_set_filter_func (GTK_LIST_BOX (self->list_box),
                                 filter_func, self, NULL);
  gtk_widget_add_css_class (self->list_box, "boxed-list");
  g_signal_connect (self->list_box, "row-selected",
                    G_CALLBACK (on_row_selected), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (left_scroll), self->list_box);
  gtk_paned_set_start_child (GTK_PANED (paned), left_scroll);

  /* Detail pane */
  GtkWidget *detail_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (detail_box, TRUE);

  self->detail_title = gtk_label_new ("");
  gtk_widget_add_css_class (self->detail_title, "title-3");
  gtk_widget_set_margin_top    (self->detail_title, 12);
  gtk_widget_set_margin_bottom (self->detail_title, 6);
  gtk_widget_set_margin_start  (self->detail_title, 12);
  gtk_box_append (GTK_BOX (detail_box), self->detail_title);

  self->detail_stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->detail_stack, TRUE);

  GtkWidget *no_selection = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (no_selection), "Select a Group");
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (no_selection),
                                  "avatar-default-symbolic");
  gtk_stack_add_named (GTK_STACK (self->detail_stack), no_selection, "none");

  GtkWidget *members_scroll = gtk_scrolled_window_new ();
  self->members_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->members_list),
                                    GTK_SELECTION_NONE);
  gtk_widget_add_css_class (self->members_list, "boxed-list");
  gtk_widget_set_margin_top    (self->members_list, 6);
  gtk_widget_set_margin_start  (self->members_list, 12);
  gtk_widget_set_margin_end    (self->members_list, 12);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (members_scroll),
                                  self->members_list);
  gtk_stack_add_named (GTK_STACK (self->detail_stack), members_scroll, "members");

  gtk_box_append (GTK_BOX (detail_box), self->detail_stack);
  gtk_paned_set_end_child (GTK_PANED (paned), detail_box);

  gtk_stack_add_named (GTK_STACK (self->stack), paned, "split");
  gtk_box_append (GTK_BOX (self), self->stack);
}

GtkWidget *
sbv_groups_panel_new (void)
{
  return g_object_new (SBV_TYPE_GROUPS_PANEL, NULL);
}
