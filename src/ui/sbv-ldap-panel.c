#include "sbv-ldap-panel.h"
#include "../backend/sbv-ldap-browse.h"
#include "../model/sbv-ldap-node.h"

#include <adwaita.h>
#include <string.h>

struct _SbvLdapPanel {
  GtkBox            parent;

  GtkWidget        *outer_stack;       /* "loading" | "empty" | "split"   */
  GtkWidget        *spinner;
  GtkWidget        *paned;
  gboolean          paned_init;

  /* Tree */
  GtkWidget        *tree_view;          /* GtkListView                     */
  GListStore       *root_store;         /* one item: the root node         */
  GtkTreeListModel *tree_model;         /* wraps root_store, lazy children */
  GtkSingleSelection *selection;

  /* Detail pane */
  GtkWidget        *detail_stack;       /* "none" | "detail"               */
  GtkWidget        *detail_dn;
  GtkWidget        *raw_attrs_box;

  SbvConnection    *conn;               /* unowned                         */
};

G_DEFINE_TYPE (SbvLdapPanel, sbv_ldap_panel, GTK_TYPE_BOX)

/* ── Forward declarations ──────────────────────────────────────────────── */
static GListModel *create_child_model (gpointer item, gpointer user_data);

/* ── Raw attrs renderer (mirrors users/groups/computers panels) ─────────── */

/* Builds one "<attr>: <values>" row for the All-LDAP-Attributes display. */
static GtkWidget *
make_raw_attr_row (const char *attr, const char *value, int label_width)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (box, 2);
  gtk_widget_set_margin_bottom (box, 2);
  GtkWidget *lbl = gtk_label_new (attr);
  gtk_label_set_xalign (GTK_LABEL (lbl), 1.0);
  gtk_widget_set_size_request (lbl, label_width, -1);
  gtk_widget_add_css_class (lbl, "dim-label");
  gtk_box_append (GTK_BOX (box), lbl);
  GtkWidget *val = gtk_label_new (value);
  gtk_label_set_xalign (GTK_LABEL (val), 0.0);
  gtk_label_set_selectable (GTK_LABEL (val), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (val), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (val, TRUE);
  gtk_widget_add_css_class (val, "monospace");
  gtk_box_append (GTK_BOX (box), val);
  return box;
}

/* Replaces the contents of `box` with rows for each attribute in `attrs`,
 * sorted alphabetically. */
static void
populate_raw_attrs (GtkWidget *box, GHashTable *attrs, int label_width)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (box)) != NULL)
    gtk_box_remove (GTK_BOX (box), child);
  if (!attrs) return;
  GList *keys = g_list_sort (g_hash_table_get_keys (attrs),
                              (GCompareFunc) g_strcmp0);
  for (GList *l = keys; l; l = l->next) {
    const char  *attr   = l->data;
    char       **vals   = g_hash_table_lookup (attrs, attr);
    char        *joined = (vals && vals[0]) ? g_strjoinv ("; ", vals)
                                            : g_strdup ("\xe2\x80\x94");
    gtk_box_append (GTK_BOX (box), make_raw_attr_row (attr, joined, label_width));
    g_free (joined);
  }
  g_list_free (keys);
}

/* ── Tree row factory ──────────────────────────────────────────────────── */

/* Factory "setup": each row is a GtkTreeExpander wrapping a label. */
static void
setup_row (GtkSignalListItemFactory *f, GtkListItem *item, gpointer ud)
{
  (void) f; (void) ud;
  GtkWidget *expander = gtk_tree_expander_new ();
  GtkWidget *label    = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), label);
  gtk_list_item_set_child (item, expander);
}

/* Factory "bind": pulls the SbvLdapNode out of its GtkTreeListRow wrapper
 * and updates the row's expander + label to reflect it. */
static void
bind_row (GtkSignalListItemFactory *f, GtkListItem *item, gpointer ud)
{
  (void) f; (void) ud;
  GtkTreeListRow *row      = gtk_list_item_get_item (item);
  GtkWidget      *expander = gtk_list_item_get_child (item);
  gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

  SbvLdapNode *node  = gtk_tree_list_row_get_item (row);
  GtkWidget   *label = gtk_tree_expander_get_child (GTK_TREE_EXPANDER (expander));
  gtk_label_set_text (GTK_LABEL (label),
                       sbv_ldap_node_get_label (node) ?: "");
  /* gtk_tree_list_row_get_item returns a transfer-full ref. */
  g_object_unref (node);
}

/* ── Lazy children loading ─────────────────────────────────────────────── */

typedef struct {
  SbvLdapPanel *panel;
  SbvLdapNode  *node;       /* unowned — kept alive by the tree model    */
} ChildrenCtx;

/* Async-completion: append the discovered children into the node's store. */
static void
on_children_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ChildrenCtx  *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  GListStore *children = sbv_ldap_browse_children_finish (conn, result, &err);
  sbv_ldap_node_set_loaded (ctx->node, TRUE);

  if (!children) {
    g_warning ("LDAP browse failed for %s: %s",
                sbv_ldap_node_get_dn (ctx->node),
                err ? err->message : "(no detail)");
    if (err) g_error_free (err);
    g_free (ctx);
    return;
  }

  GListStore *dest = sbv_ldap_node_get_children (ctx->node);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (children));
  for (guint i = 0; i < n; i++) {
    SbvLdapNode *child = g_list_model_get_item (G_LIST_MODEL (children), i);
    g_list_store_append (dest, child);
    g_object_unref (child);
  }
  g_object_unref (children);
  g_free (ctx);
}

/* GtkTreeListModelCreateModelFunc: GTK calls this once per item to obtain
 * its child model. We always return the node's children store (so every
 * row appears expandable); on first request we kick off an async LDAP
 * one-level search to populate it. */
static GListModel *
create_child_model (gpointer item, gpointer user_data)
{
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  SbvLdapNode  *node = SBV_LDAP_NODE (item);

  GListStore *children = sbv_ldap_node_get_children (node);

  if (!sbv_ldap_node_get_loaded (node) && self->conn) {
    /* Mark loaded preemptively to suppress duplicate requests if GTK
     * re-invokes us before the async completes. */
    sbv_ldap_node_set_loaded (node, TRUE);

    ChildrenCtx *ctx = g_new0 (ChildrenCtx, 1);
    ctx->panel = self;
    ctx->node  = node;
    sbv_ldap_browse_children_async (self->conn,
                                     sbv_ldap_node_get_dn (node),
                                     NULL, on_children_loaded, ctx);
  }

  return g_object_ref (G_LIST_MODEL (children));
}

/* ── Selection → detail pane ───────────────────────────────────────────── */

/* Render `node`'s attributes in the right-hand pane. */
static void
show_node_detail (SbvLdapPanel *self, SbvLdapNode *node)
{
  if (!node) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    return;
  }
  gtk_label_set_text (GTK_LABEL (self->detail_dn),
                      sbv_ldap_node_get_dn (node) ?: "");
  populate_raw_attrs (self->raw_attrs_box,
                      sbv_ldap_node_get_attrs (node), 200);
  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "detail");
}

/* Selection-changed handler: unwrap the GtkTreeListRow → SbvLdapNode and
 * push it to the detail renderer. */
static void
on_selection_changed (GtkSelectionModel *sel, guint position, guint n_items,
                       gpointer user_data)
{
  (void) position; (void) n_items;
  SbvLdapPanel    *self  = SBV_LDAP_PANEL (user_data);
  GtkSingleSelection *ss = GTK_SINGLE_SELECTION (sel);
  GObject *selected = gtk_single_selection_get_selected_item (ss);
  if (!selected) {
    show_node_detail (self, NULL);
    return;
  }
  GtkTreeListRow *row = GTK_TREE_LIST_ROW (selected);
  SbvLdapNode    *node = gtk_tree_list_row_get_item (row);
  show_node_detail (self, node);
  g_object_unref (node);
}

/* ── Async: root attrs ─────────────────────────────────────────────────── */

typedef struct {
  SbvLdapPanel *panel;
  SbvLdapNode  *root;
} RootAttrsCtx;

/* Completion: stash the root's attrs and refresh detail if the root is
 * currently selected (it usually is by default). */
static void
on_root_attrs_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RootAttrsCtx  *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  GHashTable *attrs = sbv_ldap_browse_attrs_finish (conn, result, &err);
  if (attrs) {
    sbv_ldap_node_set_attrs (ctx->root, attrs);
    /* If root is the currently-selected item, refresh the detail pane. */
    GObject *sel = gtk_single_selection_get_selected_item (ctx->panel->selection);
    if (sel && GTK_IS_TREE_LIST_ROW (sel)) {
      SbvLdapNode *node = gtk_tree_list_row_get_item (GTK_TREE_LIST_ROW (sel));
      if (node == ctx->root)
        show_node_detail (ctx->panel, ctx->root);
      g_object_unref (node);
    }
  } else if (err) {
    g_warning ("Root attrs read failed: %s", err->message);
    g_error_free (err);
  }
  g_free (ctx);
}

/* ── Public load ───────────────────────────────────────────────────────── */

/* Resets the tree to a single root node (the connection's base DN) and
 * fetches its attributes asynchronously. */
void
sbv_ldap_panel_load (SbvLdapPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  const char *base = sbv_connection_get_base_dn (conn);
  if (!base || !*base) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "empty");
    return;
  }

  /* Rebuild the root store with a single node. */
  g_list_store_remove_all (self->root_store);
  SbvLdapNode *root = sbv_ldap_node_new (base, base);
  g_list_store_append (self->root_store, root);

  /* Auto-select the root so the detail pane is non-empty straight away. */
  gtk_single_selection_set_selected (self->selection, 0);

  /* Fetch root attrs in the background. */
  RootAttrsCtx *ctx = g_new0 (RootAttrsCtx, 1);
  ctx->panel = self;
  ctx->root  = root;
  sbv_ldap_browse_attrs_async (conn, base, NULL,
                                on_root_attrs_loaded, ctx);

  g_object_unref (root);
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "split");
}

/* ── Paned position ────────────────────────────────────────────────────── */

/* Sets a sensible initial split when the paned is first realised. */
static void
on_paned_map (GtkWidget *widget, gpointer user_data)
{
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  if (!self->paned_init) {
    int w = gtk_widget_get_width (widget);
    if (w > 0) {
      self->paned_init = TRUE;
      gtk_paned_set_position (GTK_PANED (widget), 320);
    }
  }
}

/* ── GObject lifecycle ─────────────────────────────────────────────────── */

static void
sbv_ldap_panel_finalize (GObject *object)
{
  SbvLdapPanel *self = SBV_LDAP_PANEL (object);
  g_clear_object (&self->tree_model);
  g_clear_object (&self->root_store);
  G_OBJECT_CLASS (sbv_ldap_panel_parent_class)->finalize (object);
}

static void sbv_ldap_panel_class_init (SbvLdapPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_ldap_panel_finalize;
}

static void
sbv_ldap_panel_init (SbvLdapPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->outer_stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->outer_stack, TRUE);

  /* Loading */
  {
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    self->spinner = gtk_spinner_new ();
    gtk_box_append (GTK_BOX (box), self->spinner);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), box, "loading");
  }

  /* Empty */
  {
    GtkWidget *empty = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty),
                                    "view-list-bullet-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Base DN");
    adw_status_page_set_description (ADW_STATUS_PAGE (empty),
      "The active connection has no base DN configured.");
    gtk_stack_add_named (GTK_STACK (self->outer_stack), empty, "empty");
  }

  /* Split: tree on left, detail on right */
  {
    self->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    g_signal_connect (self->paned, "map", G_CALLBACK (on_paned_map), self);

    /* ── Tree ── */
    self->root_store = g_list_store_new (SBV_TYPE_LDAP_NODE);
    self->tree_model = gtk_tree_list_model_new (G_LIST_MODEL (self->root_store),
                                                  FALSE,    /* passthrough  */
                                                  FALSE,    /* autoexpand   */
                                                  create_child_model,
                                                  self,
                                                  NULL);
    self->selection = GTK_SINGLE_SELECTION (
      gtk_single_selection_new (G_LIST_MODEL (self->tree_model)));
    /* gtk_single_selection_new takes the model ref; tree_model still
     * referenced by us for explicit cleanup. Bump for clarity. */
    g_object_ref (self->tree_model);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (setup_row), NULL);
    g_signal_connect (factory, "bind",  G_CALLBACK (bind_row),  NULL);

    self->tree_view = gtk_list_view_new (GTK_SELECTION_MODEL (self->selection),
                                          factory);
    gtk_widget_add_css_class (self->tree_view, "navigation-sidebar");
    g_signal_connect (self->selection, "selection-changed",
                      G_CALLBACK (on_selection_changed), self);

    GtkWidget *tree_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (tree_scroll, TRUE);
    gtk_widget_set_size_request (tree_scroll, 280, -1);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (tree_scroll),
                                    self->tree_view);
    gtk_paned_set_start_child (GTK_PANED (self->paned), tree_scroll);

    /* ── Detail pane ── */
    self->detail_stack = gtk_stack_new ();
    gtk_widget_set_hexpand (self->detail_stack, TRUE);

    GtkWidget *none_page = adw_status_page_new ();
    adw_status_page_set_title (ADW_STATUS_PAGE (none_page), "Select an Entry");
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (none_page),
                                    "view-list-bullet-symbolic");
    gtk_stack_add_named (GTK_STACK (self->detail_stack), none_page, "none");

    GtkWidget *detail_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (detail_scroll, TRUE);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *detail_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top    (detail_box, 16);
    gtk_widget_set_margin_bottom (detail_box, 16);
    gtk_widget_set_margin_start  (detail_box, 16);
    gtk_widget_set_margin_end    (detail_box, 16);

    {
      GtkWidget *hdr = gtk_label_new ("Distinguished Name");
      gtk_label_set_xalign (GTK_LABEL (hdr), 0);
      gtk_widget_add_css_class (hdr, "heading");
      gtk_box_append (GTK_BOX (detail_box), hdr);
    }

    self->detail_dn = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->detail_dn), 0);
    gtk_label_set_selectable (GTK_LABEL (self->detail_dn), TRUE);
    gtk_label_set_ellipsize (GTK_LABEL (self->detail_dn), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_wrap (GTK_LABEL (self->detail_dn), FALSE);
    gtk_widget_add_css_class (self->detail_dn, "monospace");
    gtk_box_append (GTK_BOX (detail_box), self->detail_dn);

    gtk_box_append (GTK_BOX (detail_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    {
      GtkWidget *hdr = gtk_label_new ("All LDAP Attributes");
      gtk_label_set_xalign (GTK_LABEL (hdr), 0);
      gtk_widget_add_css_class (hdr, "heading");
      gtk_widget_set_margin_top (hdr, 8);
      gtk_box_append (GTK_BOX (detail_box), hdr);
    }

    self->raw_attrs_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (detail_box), self->raw_attrs_box);

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (detail_scroll),
                                    detail_box);
    gtk_stack_add_named (GTK_STACK (self->detail_stack), detail_scroll, "detail");

    gtk_paned_set_end_child (GTK_PANED (self->paned), self->detail_stack);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), self->paned, "split");
  }

  gtk_box_append (GTK_BOX (self), self->outer_stack);
}

/* Public constructor. */
GtkWidget *
sbv_ldap_panel_new (void)
{
  return g_object_new (SBV_TYPE_LDAP_PANEL, NULL);
}
