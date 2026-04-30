#include "sbv-ldap-panel.h"
#include "../backend/sbv-ldap-browse.h"
#include "../model/sbv-ldap-node.h"

#include <adwaita.h>
#include <string.h>

/* Per-attribute editor row. The detail pane keeps a flat list of these so
 * Save Changes can diff them against `loaded_attrs` and emit one batched
 * LDAP modify request. */
typedef struct {
  char      *attr;            /* attribute name; NULL for a freshly-added,
                                 not-yet-named row                          */
  GtkWidget *row;              /* outer GtkBox holding the widgets below     */
  GtkWidget *attr_label;       /* GtkLabel (existing attr) or NULL           */
  GtkWidget *attr_entry;       /* GtkEntry (newly-added attr) or NULL        */
  GtkWidget *value_entry;      /* multi-value joined by "; "                 */
  GtkWidget *remove_btn;
  gboolean   is_new;           /* TRUE → was added via the "+" button        */
  gboolean   marked_delete;    /* TRUE → row is rendered struck-through and
                                  contributes a MOD_DELETE on save           */
} AttrRow;

struct _SbvLdapPanel {
  GtkBox            parent;

  GtkWidget        *outer_stack;       /* "loading" | "empty" | "split"    */
  GtkWidget        *spinner;
  GtkWidget        *paned;
  gboolean          paned_init;

  /* Filter search */
  GtkWidget        *search_entry;      /* GtkSearchEntry — LDAP filter      */
  GtkWidget        *search_status;     /* "47 matches", "Truncated", errors */
  GtkWidget        *left_stack;        /* "tree" | "results"                */
  char             *active_filter;     /* current applied filter, or NULL   */

  /* Tree view */
  GtkWidget        *tree_view;
  GListStore       *root_store;
  GtkTreeListModel *tree_model;
  GtkSingleSelection *selection;

  /* Flat results view */
  GtkWidget        *results_view;
  GListStore       *results_store;     /* GListStore<SbvLdapNode>           */
  GtkSingleSelection *results_selection;

  /* Detail pane */
  GtkWidget        *detail_stack;       /* "none" | "detail"                */
  GtkWidget        *detail_dn;
  GtkWidget        *attrs_box;          /* AttrRow widgets live here        */
  GtkWidget        *new_attr_name;
  GtkWidget        *new_attr_value;
  GtkWidget        *new_attr_btn;
  GtkWidget        *save_btn;
  GtkWidget        *save_error;

  /* Selection-tracked editor state */
  GHashTable       *loaded_attrs;       /* GHashTable<char*, GStrv> — copy
                                          of the most recently selected
                                          node's attrs at load time         */
  char             *loaded_dn;          /* DN whose attrs are loaded        */
  GPtrArray        *attr_rows;          /* GPtrArray<AttrRow*>              */

  SbvConnection    *conn;               /* unowned                         */
};

G_DEFINE_TYPE (SbvLdapPanel, sbv_ldap_panel, GTK_TYPE_BOX)

/* ── Forward declarations ──────────────────────────────────────────────── */
static GListModel *create_child_model    (gpointer item, gpointer user_data);
static void        rebuild_attr_rows     (SbvLdapPanel *self);
static void        load_node_into_editor (SbvLdapPanel *self, SbvLdapNode *node);
static void        update_save_sensitive (SbvLdapPanel *self);
static void        on_attr_remove_clicked (GtkButton *btn, gpointer user_data);
static void        on_attr_value_changed (GtkEditable *e, gpointer user_data);

/* ── attr-rows helpers ─────────────────────────────────────────────────── */

/* Free one AttrRow descriptor. Doesn't touch the GtkWidgets — those are
 * owned by the parent box and removed via gtk_box_remove. */
static void
attr_row_free (AttrRow *r)
{
  if (!r) return;
  g_free (r->attr);
  g_free (r);
}

/* Builds the formatted "value1; value2; ..." string the editor displays for
 * a multi-value attribute. */
static char *
join_values (char **values)
{
  if (!values || !values[0]) return g_strdup ("");
  return g_strjoinv ("; ", values);
}

/* Splits the editor's "; "-joined display string back into a NULL-terminated
 * value vector. Empty tokens are dropped so a trailing semicolon doesn't
 * round-trip into an empty value. */
static char **
split_values (const char *joined)
{
  if (!joined || !*joined) return NULL;
  char **raw = g_strsplit (joined, ";", -1);
  GPtrArray *out = g_ptr_array_new ();
  for (int i = 0; raw[i]; i++) {
    char *trimmed = g_strstrip (raw[i]);
    if (*trimmed)
      g_ptr_array_add (out, g_strdup (trimmed));
  }
  g_strfreev (raw);
  if (out->len == 0) {
    g_ptr_array_free (out, TRUE);
    return NULL;
  }
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}

/* Build one editor row. For existing attributes the name is a label;
 * freshly-added rows show an entry instead so the user can pick a name. */
static AttrRow *
make_attr_row (SbvLdapPanel *self,
                const char   *attr,
                const char   *joined_value,
                gboolean      is_new)
{
  AttrRow *r = g_new0 (AttrRow, 1);
  r->attr   = attr ? g_strdup (attr) : NULL;
  r->is_new = is_new;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (box, 2);
  gtk_widget_set_margin_bottom (box, 2);
  r->row = box;

  if (is_new) {
    r->attr_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (r->attr_entry), "attribute");
    gtk_widget_set_size_request (r->attr_entry, 200, -1);
    gtk_widget_add_css_class (r->attr_entry, "monospace");
    g_signal_connect (r->attr_entry, "changed",
                      G_CALLBACK (on_attr_value_changed), self);
    gtk_box_append (GTK_BOX (box), r->attr_entry);
  } else {
    r->attr_label = gtk_label_new (attr ?: "");
    gtk_label_set_xalign (GTK_LABEL (r->attr_label), 1.0);
    gtk_widget_set_size_request (r->attr_label, 200, -1);
    gtk_widget_add_css_class (r->attr_label, "dim-label");
    gtk_box_append (GTK_BOX (box), r->attr_label);
  }

  r->value_entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (r->value_entry), joined_value ?: "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (r->value_entry),
                                   "value (multi-values: \"a; b\")");
  gtk_widget_set_hexpand (r->value_entry, TRUE);
  gtk_widget_add_css_class (r->value_entry, "monospace");
  g_signal_connect (r->value_entry, "changed",
                    G_CALLBACK (on_attr_value_changed), self);
  gtk_box_append (GTK_BOX (box), r->value_entry);

  r->remove_btn = gtk_button_new_from_icon_name ("user-trash-symbolic");
  gtk_widget_add_css_class (r->remove_btn, "flat");
  gtk_widget_set_tooltip_text (r->remove_btn, "Mark attribute for deletion");
  g_object_set_data (G_OBJECT (r->remove_btn), "attr-row", r);
  g_signal_connect (r->remove_btn, "clicked",
                    G_CALLBACK (on_attr_remove_clicked), self);
  gtk_box_append (GTK_BOX (box), r->remove_btn);

  return r;
}

/* "Trash" button on a row: existing rows are marked-for-delete (struck
 * through, value disabled, save will issue a MOD_DELETE); new rows are
 * removed outright since they never existed on the directory. */
static void
on_attr_remove_clicked (GtkButton *btn, gpointer user_data)
{
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  AttrRow      *r    = g_object_get_data (G_OBJECT (btn), "attr-row");
  if (!r) return;

  if (r->is_new) {
    for (guint i = 0; i < self->attr_rows->len; i++) {
      if (g_ptr_array_index (self->attr_rows, i) == r) {
        gtk_box_remove (GTK_BOX (self->attrs_box), r->row);
        g_ptr_array_remove_index (self->attr_rows, i);
        attr_row_free (r);
        break;
      }
    }
  } else {
    r->marked_delete = !r->marked_delete;
    if (r->marked_delete) {
      gtk_widget_add_css_class    (r->row, "dim-label");
      gtk_widget_set_sensitive    (r->value_entry, FALSE);
      gtk_widget_set_tooltip_text (r->remove_btn, "Undo deletion");
    } else {
      gtk_widget_remove_css_class (r->row, "dim-label");
      gtk_widget_set_sensitive    (r->value_entry, TRUE);
      gtk_widget_set_tooltip_text (r->remove_btn, "Mark attribute for deletion");
    }
  }
  update_save_sensitive (self);
}

/* Any edit on a row's name or value entry might affect the dirty state. */
static void
on_attr_value_changed (GtkEditable *e, gpointer user_data)
{
  (void) e;
  update_save_sensitive (SBV_LDAP_PANEL (user_data));
}

/* "+ Add attribute" button — append a fresh editor row in is_new state. */
static void
on_new_attr_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  AttrRow *r = make_attr_row (self, NULL, NULL, TRUE);
  g_ptr_array_add (self->attr_rows, r);
  gtk_box_append (GTK_BOX (self->attrs_box), r->row);
  if (r->attr_entry)
    gtk_widget_grab_focus (r->attr_entry);
  update_save_sensitive (self);
}

/* Returns TRUE if the value vectors `a` and `b` contain the same set of
 * strings (order- and duplicate-insensitive — AD treats most attrs as
 * sets, and the editor's "; " join order isn't meaningful). */
static gboolean
values_equal_set (char **a, char **b)
{
  guint na = 0, nb = 0;
  if (a) while (a[na]) na++;
  if (b) while (b[nb]) nb++;
  if (na != nb) return FALSE;
  for (guint i = 0; i < na; i++) {
    gboolean found = FALSE;
    for (guint j = 0; j < nb; j++) {
      if (g_strcmp0 (a[i], b[j]) == 0) { found = TRUE; break; }
    }
    if (!found) return FALSE;
  }
  return TRUE;
}

/* Returns TRUE if the editor state differs from `loaded_attrs`. */
static gboolean
editor_is_dirty (SbvLdapPanel *self)
{
  if (!self->attr_rows) return FALSE;

  for (guint i = 0; i < self->attr_rows->len; i++) {
    AttrRow *r = g_ptr_array_index (self->attr_rows, i);

    if (r->is_new) {
      const char *name = r->attr_entry
        ? gtk_editable_get_text (GTK_EDITABLE (r->attr_entry)) : "";
      const char *val  = gtk_editable_get_text (GTK_EDITABLE (r->value_entry));
      if (name && *name && val && *val) return TRUE;
      continue;
    }

    if (r->marked_delete) return TRUE;

    char       **orig = self->loaded_attrs && r->attr
      ? g_hash_table_lookup (self->loaded_attrs, r->attr) : NULL;
    const char  *cur_text = gtk_editable_get_text (GTK_EDITABLE (r->value_entry));
    char       **cur      = split_values (cur_text);
    gboolean     same     = values_equal_set (orig, cur);
    g_strfreev (cur);
    if (!same) return TRUE;
  }
  return FALSE;
}

/* Toggle the Save button based on whether the editor has unsaved changes. */
static void
update_save_sensitive (SbvLdapPanel *self)
{
  gtk_widget_set_sensitive (self->save_btn, editor_is_dirty (self));
}

/* ── Save ──────────────────────────────────────────────────────────────── */

/* Refresh-attrs completion: replace loaded_attrs with the DC's view and
 * rebuild rows so the editor mirrors what's actually persisted. */
static void
on_attrs_refreshed_after_save (GObject *source, GAsyncResult *result,
                                gpointer user_data)
{
  SbvLdapPanel  *self = SBV_LDAP_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  GHashTable *attrs = sbv_ldap_browse_attrs_finish (conn, result, &err);
  if (!attrs) {
    if (err) {
      g_warning ("attrs refresh failed: %s", err->message);
      g_error_free (err);
    }
    return;
  }
  g_clear_pointer (&self->loaded_attrs, g_hash_table_unref);
  self->loaded_attrs = attrs;
  rebuild_attr_rows (self);
  update_save_sensitive (self);
}

/* Async-completion for the modify itself. On success, kick off a re-read of
 * the entry so DC-side normalisation (e.g. attribute case, value collapse)
 * is reflected in the editor. */
static void
on_save_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvLdapPanel  *self = SBV_LDAP_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->save_btn, TRUE);

  if (!sbv_ldap_browse_modify_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->save_error),
                        err ? err->message : "Modify failed.");
    gtk_widget_set_visible (self->save_error, TRUE);
    if (err) g_error_free (err);
    return;
  }

  if (self->loaded_dn)
    sbv_ldap_browse_attrs_async (conn, self->loaded_dn, NULL,
                                  on_attrs_refreshed_after_save, self);
}

/* "Save Changes" button: diff editor vs loaded_attrs into a list of mods
 * and dispatch. */
static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  if (!self->conn || !self->loaded_dn) return;

  gtk_widget_set_visible (self->save_error, FALSE);

  GPtrArray *mods = g_ptr_array_new_with_free_func (
    (GDestroyNotify) sbv_ldap_mod_free);

  for (guint i = 0; i < self->attr_rows->len; i++) {
    AttrRow *r = g_ptr_array_index (self->attr_rows, i);

    if (r->is_new) {
      const char *name = r->attr_entry
        ? gtk_editable_get_text (GTK_EDITABLE (r->attr_entry)) : "";
      const char *val  = gtk_editable_get_text (GTK_EDITABLE (r->value_entry));
      if (!name || !*name || !val || !*val) continue;
      char **vv = split_values (val);
      if (vv) {
        g_ptr_array_add (mods,
          sbv_ldap_mod_new (SBV_LDAP_MOD_ADD, name,
                             (const char * const *) vv));
        g_strfreev (vv);
      }
      continue;
    }

    if (r->marked_delete) {
      g_ptr_array_add (mods,
        sbv_ldap_mod_new (SBV_LDAP_MOD_DELETE, r->attr, NULL));
      continue;
    }

    char       **orig = self->loaded_attrs && r->attr
      ? g_hash_table_lookup (self->loaded_attrs, r->attr) : NULL;
    const char  *cur_text = gtk_editable_get_text (GTK_EDITABLE (r->value_entry));
    char       **cur      = split_values (cur_text);
    if (!values_equal_set (orig, cur)) {
      if (cur)
        g_ptr_array_add (mods,
          sbv_ldap_mod_new (SBV_LDAP_MOD_REPLACE, r->attr,
                             (const char * const *) cur));
      else
        g_ptr_array_add (mods,
          sbv_ldap_mod_new (SBV_LDAP_MOD_DELETE, r->attr, NULL));
    }
    g_strfreev (cur);
  }

  if (mods->len == 0) {
    g_ptr_array_unref (mods);
    return;
  }

  gtk_widget_set_sensitive (self->save_btn, FALSE);
  sbv_ldap_browse_modify_async (self->conn, self->loaded_dn, mods,
                                 NULL, on_save_done, self);
  g_ptr_array_unref (mods);
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
  g_object_unref (node);
}

/* Factory for the flat results list: plain label, no expander. */
static void
setup_flat_row (GtkSignalListItemFactory *f, GtkListItem *item, gpointer ud)
{
  (void) f; (void) ud;
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start (label, 6);
  gtk_widget_set_margin_end   (label, 6);
  gtk_list_item_set_child (item, label);
}

static void
bind_flat_row (GtkSignalListItemFactory *f, GtkListItem *item, gpointer ud)
{
  (void) f; (void) ud;
  SbvLdapNode *node  = gtk_list_item_get_item (item);
  GtkWidget   *label = gtk_list_item_get_child (item);
  gtk_label_set_text (GTK_LABEL (label),
                       sbv_ldap_node_get_label (node) ?: "");
}

/* ── Lazy children loading ─────────────────────────────────────────────── */

typedef struct {
  SbvLdapPanel *panel;
  SbvLdapNode  *node;
} ChildrenCtx;

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

/* GtkTreeListModelCreateModelFunc — see comment in original file. */
static GListModel *
create_child_model (gpointer item, gpointer user_data)
{
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  SbvLdapNode  *node = SBV_LDAP_NODE (item);

  if (sbv_ldap_node_get_has_children (node) == 0)
    return NULL;

  GListStore *children = sbv_ldap_node_get_children (node);

  if (!sbv_ldap_node_get_loaded (node) && self->conn) {
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

/* ── Selection → editor ───────────────────────────────────────────────── */

/* Deep-copies the GHashTable<char*, GStrv> so subsequent edits to the node
 * (or save-induced refreshes) don't disturb the editor's notion of "loaded
 * state". */
static GHashTable *
clone_attrs (GHashTable *src)
{
  GHashTable *dst = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free,
                                            (GDestroyNotify) g_strfreev);
  if (!src) return dst;
  GHashTableIter it;
  gpointer       k, v;
  g_hash_table_iter_init (&it, src);
  while (g_hash_table_iter_next (&it, &k, &v)) {
    char **vv = (char **) v;
    guint n = 0;
    if (vv) while (vv[n]) n++;
    char **copy = g_new0 (char *, n + 1);
    for (guint i = 0; i < n; i++) copy[i] = g_strdup (vv[i]);
    g_hash_table_insert (dst, g_strdup (k), copy);
  }
  return dst;
}

/* Wipe the editor box and rebuild one row per attribute in `loaded_attrs`,
 * sorted alphabetically. */
static void
rebuild_attr_rows (SbvLdapPanel *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->attrs_box)) != NULL)
    gtk_box_remove (GTK_BOX (self->attrs_box), child);

  if (self->attr_rows)
    g_ptr_array_set_size (self->attr_rows, 0);
  else
    self->attr_rows = g_ptr_array_new_with_free_func (
      (GDestroyNotify) attr_row_free);

  if (!self->loaded_attrs) return;

  GList *keys = g_list_sort (g_hash_table_get_keys (self->loaded_attrs),
                              (GCompareFunc) g_strcmp0);
  for (GList *l = keys; l; l = l->next) {
    const char  *attr   = l->data;
    char       **vals   = g_hash_table_lookup (self->loaded_attrs, attr);
    char        *joined = join_values (vals);
    AttrRow *r = make_attr_row (self, attr, joined, FALSE);
    g_free (joined);
    g_ptr_array_add (self->attr_rows, r);
    gtk_box_append (GTK_BOX (self->attrs_box), r->row);
  }
  g_list_free (keys);
}

/* Push a node into the editor: caches a copy of its attrs, rebuilds rows. */
static void
load_node_into_editor (SbvLdapPanel *self, SbvLdapNode *node)
{
  if (!node) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    g_clear_pointer (&self->loaded_dn, g_free);
    g_clear_pointer (&self->loaded_attrs, g_hash_table_unref);
    return;
  }

  g_free (self->loaded_dn);
  self->loaded_dn = g_strdup (sbv_ldap_node_get_dn (node));
  g_clear_pointer (&self->loaded_attrs, g_hash_table_unref);
  self->loaded_attrs = clone_attrs (sbv_ldap_node_get_attrs (node));

  gtk_label_set_text (GTK_LABEL (self->detail_dn), self->loaded_dn ?: "");
  rebuild_attr_rows (self);
  gtk_widget_set_visible (self->save_error, FALSE);
  update_save_sensitive (self);

  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "detail");
}

/* Tree selection-changed handler. */
static void
on_tree_selection_changed (GtkSelectionModel *sel, guint position, guint n_items,
                            gpointer user_data)
{
  (void) position; (void) n_items;
  SbvLdapPanel    *self  = SBV_LDAP_PANEL (user_data);
  GObject *selected = gtk_single_selection_get_selected_item (
    GTK_SINGLE_SELECTION (sel));
  if (!selected) {
    load_node_into_editor (self, NULL);
    return;
  }
  GtkTreeListRow *row  = GTK_TREE_LIST_ROW (selected);
  SbvLdapNode    *node = gtk_tree_list_row_get_item (row);
  load_node_into_editor (self, node);
  g_object_unref (node);
}

/* Flat results selection-changed handler. */
static void
on_results_selection_changed (GtkSelectionModel *sel, guint position, guint n_items,
                               gpointer user_data)
{
  (void) position; (void) n_items;
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  GObject *selected = gtk_single_selection_get_selected_item (
    GTK_SINGLE_SELECTION (sel));
  if (!selected) {
    load_node_into_editor (self, NULL);
    return;
  }
  load_node_into_editor (self, SBV_LDAP_NODE (selected));
}

/* ── Filter search ────────────────────────────────────────────────────── */

/* Async-completion: replace the results store with the search hits. */
static void
on_search_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvLdapPanel  *self = SBV_LDAP_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  GListStore *hits = sbv_ldap_browse_search_finish (conn, result, &err);
  if (!hits) {
    gtk_label_set_text (GTK_LABEL (self->search_status),
                        err ? err->message : "Search failed.");
    if (err) g_error_free (err);
    return;
  }

  g_list_store_remove_all (self->results_store);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (hits));
  for (guint i = 0; i < n; i++) {
    GObject *item = g_list_model_get_item (G_LIST_MODEL (hits), i);
    g_list_store_append (self->results_store, item);
    g_object_unref (item);
  }
  g_object_unref (hits);

  char *msg = g_strdup_printf ("%u match%s%s",
                                n, n == 1 ? "" : "es",
                                n >= 500 ? " — truncated, refine the filter" : "");
  gtk_label_set_text (GTK_LABEL (self->search_status), msg);
  g_free (msg);

  /* Auto-select the first hit so the editor is non-empty. */
  if (n > 0)
    gtk_single_selection_set_selected (self->results_selection, 0);
  else
    load_node_into_editor (self, NULL);
}

/* User pressed Enter or activated the search entry: dispatch the search. */
static void
on_search_activated (GtkSearchEntry *e, gpointer user_data)
{
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  if (!self->conn) return;
  const char *filter = gtk_editable_get_text (GTK_EDITABLE (e));
  const char *base   = sbv_connection_get_base_dn (self->conn);
  if (!base || !*base) return;

  if (!filter || !*filter) {
    /* Empty filter clears the search — back to tree view. */
    g_clear_pointer (&self->active_filter, g_free);
    g_list_store_remove_all (self->results_store);
    gtk_label_set_text (GTK_LABEL (self->search_status), "");
    gtk_stack_set_visible_child_name (GTK_STACK (self->left_stack), "tree");
    return;
  }

  g_free (self->active_filter);
  self->active_filter = g_strdup (filter);
  gtk_label_set_text (GTK_LABEL (self->search_status), "Searching…");
  gtk_stack_set_visible_child_name (GTK_STACK (self->left_stack), "results");

  sbv_ldap_browse_search_async (self->conn, base, filter,
                                 NULL, on_search_done, self);
}

/* GtkSearchEntry's "stop-search" (Esc) clears the filter and snaps back to
 * the tree view. */
static void
on_search_stopped (GtkSearchEntry *e, gpointer user_data)
{
  SbvLdapPanel *self = SBV_LDAP_PANEL (user_data);
  gtk_editable_set_text (GTK_EDITABLE (e), "");
  g_clear_pointer (&self->active_filter, g_free);
  g_list_store_remove_all (self->results_store);
  gtk_label_set_text (GTK_LABEL (self->search_status), "");
  gtk_stack_set_visible_child_name (GTK_STACK (self->left_stack), "tree");
}

/* ── Async: root attrs ─────────────────────────────────────────────────── */

typedef struct {
  SbvLdapPanel *panel;
  SbvLdapNode  *root;
} RootAttrsCtx;

static void
on_root_attrs_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RootAttrsCtx  *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  GHashTable *attrs = sbv_ldap_browse_attrs_finish (conn, result, &err);
  if (attrs) {
    sbv_ldap_node_set_attrs (ctx->root, attrs);
    GObject *sel = gtk_single_selection_get_selected_item (ctx->panel->selection);
    if (sel && GTK_IS_TREE_LIST_ROW (sel)) {
      SbvLdapNode *node = gtk_tree_list_row_get_item (GTK_TREE_LIST_ROW (sel));
      if (node == ctx->root)
        load_node_into_editor (ctx->panel, ctx->root);
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

  /* Reset filter + results state. */
  g_clear_pointer (&self->active_filter, g_free);
  if (self->results_store)
    g_list_store_remove_all (self->results_store);
  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
  gtk_label_set_text (GTK_LABEL (self->search_status), "");
  gtk_stack_set_visible_child_name (GTK_STACK (self->left_stack), "tree");

  g_list_store_remove_all (self->root_store);
  SbvLdapNode *root = sbv_ldap_node_new (base, base);
  g_list_store_append (self->root_store, root);
  gtk_single_selection_set_selected (self->selection, 0);

  RootAttrsCtx *ctx = g_new0 (RootAttrsCtx, 1);
  ctx->panel = self;
  ctx->root  = root;
  sbv_ldap_browse_attrs_async (conn, base, NULL,
                                on_root_attrs_loaded, ctx);

  g_object_unref (root);
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "split");
}

/* ── Paned position ────────────────────────────────────────────────────── */

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
  g_clear_object (&self->results_store);
  g_clear_pointer (&self->loaded_dn, g_free);
  g_clear_pointer (&self->active_filter, g_free);
  g_clear_pointer (&self->loaded_attrs, g_hash_table_unref);
  if (self->attr_rows) g_ptr_array_unref (self->attr_rows);
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

  self->attr_rows = g_ptr_array_new_with_free_func ((GDestroyNotify) attr_row_free);

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

  /* Split: search + tree/results on left, editor on right */
  {
    self->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    g_signal_connect (self->paned, "map", G_CALLBACK (on_paned_map), self);

    /* ── Left side ── */
    GtkWidget *left_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    /* Search entry */
    GtkWidget *search_bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top    (search_bar, 6);
    gtk_widget_set_margin_bottom (search_bar, 6);
    gtk_widget_set_margin_start  (search_bar, 8);
    gtk_widget_set_margin_end    (search_bar, 8);

    self->search_entry = gtk_search_entry_new ();
    gtk_widget_add_css_class (self->search_entry, "monospace");
    gtk_widget_set_tooltip_text (self->search_entry,
      "LDAP filter — e.g. (sAMAccountName=alice). Press Enter to search; Esc to clear.");
    g_signal_connect (self->search_entry, "activate",
                      G_CALLBACK (on_search_activated), self);
    g_signal_connect (self->search_entry, "stop-search",
                      G_CALLBACK (on_search_stopped), self);
    gtk_box_append (GTK_BOX (search_bar), self->search_entry);

    self->search_status = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->search_status), 0);
    gtk_widget_add_css_class (self->search_status, "dim-label");
    gtk_widget_add_css_class (self->search_status, "caption");
    gtk_box_append (GTK_BOX (search_bar), self->search_status);

    gtk_box_append (GTK_BOX (left_box), search_bar);
    gtk_box_append (GTK_BOX (left_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* Tree/results stack */
    self->left_stack = gtk_stack_new ();
    gtk_widget_set_vexpand (self->left_stack, TRUE);

    /* Tree page */
    self->root_store = g_list_store_new (SBV_TYPE_LDAP_NODE);
    self->tree_model = gtk_tree_list_model_new (G_LIST_MODEL (self->root_store),
                                                  FALSE, FALSE,
                                                  create_child_model,
                                                  self, NULL);
    self->selection = GTK_SINGLE_SELECTION (
      gtk_single_selection_new (G_LIST_MODEL (self->tree_model)));
    g_object_ref (self->tree_model);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (setup_row), NULL);
    g_signal_connect (factory, "bind",  G_CALLBACK (bind_row),  NULL);
    self->tree_view = gtk_list_view_new (GTK_SELECTION_MODEL (self->selection),
                                          factory);
    gtk_widget_add_css_class (self->tree_view, "navigation-sidebar");
    g_signal_connect (self->selection, "selection-changed",
                      G_CALLBACK (on_tree_selection_changed), self);

    GtkWidget *tree_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (tree_scroll, TRUE);
    gtk_widget_set_size_request (tree_scroll, 280, -1);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (tree_scroll),
                                    self->tree_view);
    gtk_stack_add_named (GTK_STACK (self->left_stack), tree_scroll, "tree");

    /* Results page (flat list) */
    self->results_store = g_list_store_new (SBV_TYPE_LDAP_NODE);
    self->results_selection = GTK_SINGLE_SELECTION (
      gtk_single_selection_new (G_LIST_MODEL (self->results_store)));
    g_object_ref (self->results_store);
    gtk_single_selection_set_autoselect (self->results_selection, FALSE);
    gtk_single_selection_set_can_unselect (self->results_selection, TRUE);

    GtkListItemFactory *flat_factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (flat_factory, "setup", G_CALLBACK (setup_flat_row), NULL);
    g_signal_connect (flat_factory, "bind",  G_CALLBACK (bind_flat_row),  NULL);
    self->results_view = gtk_list_view_new (
      GTK_SELECTION_MODEL (self->results_selection), flat_factory);
    gtk_widget_add_css_class (self->results_view, "navigation-sidebar");
    g_signal_connect (self->results_selection, "selection-changed",
                      G_CALLBACK (on_results_selection_changed), self);

    GtkWidget *results_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (results_scroll, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (results_scroll),
                                    self->results_view);
    gtk_stack_add_named (GTK_STACK (self->left_stack), results_scroll, "results");

    gtk_box_append (GTK_BOX (left_box), self->left_stack);
    gtk_paned_set_start_child (GTK_PANED (self->paned), left_box);

    /* ── Right side: editor pane ── */
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
    gtk_widget_add_css_class (self->detail_dn, "monospace");
    gtk_box_append (GTK_BOX (detail_box), self->detail_dn);

    gtk_box_append (GTK_BOX (detail_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    {
      GtkWidget *hdr = gtk_label_new ("LDAP Attributes");
      gtk_label_set_xalign (GTK_LABEL (hdr), 0);
      gtk_widget_add_css_class (hdr, "heading");
      gtk_widget_set_margin_top (hdr, 8);
      gtk_box_append (GTK_BOX (detail_box), hdr);
    }

    self->attrs_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (detail_box), self->attrs_box);

    /* Add-attribute row */
    GtkWidget *add_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top (add_box, 8);
    self->new_attr_btn = gtk_button_new_with_label ("Add attribute");
    gtk_widget_add_css_class (self->new_attr_btn, "flat");
    g_signal_connect (self->new_attr_btn, "clicked",
                      G_CALLBACK (on_new_attr_clicked), self);
    gtk_box_append (GTK_BOX (add_box), self->new_attr_btn);
    gtk_box_append (GTK_BOX (detail_box), add_box);
    /* These are kept on the struct to satisfy initializers, but the row is
     * built lazily by on_new_attr_clicked. */
    self->new_attr_name  = NULL;
    self->new_attr_value = NULL;

    /* Save button + error */
    gtk_box_append (GTK_BOX (detail_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *save_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top (save_box, 8);
    self->save_btn = gtk_button_new_with_label ("Save Changes");
    gtk_widget_add_css_class (self->save_btn, "suggested-action");
    gtk_widget_set_halign (self->save_btn, GTK_ALIGN_START);
    gtk_widget_set_sensitive (self->save_btn, FALSE);
    g_signal_connect (self->save_btn, "clicked",
                      G_CALLBACK (on_save_clicked), self);
    gtk_box_append (GTK_BOX (save_box), self->save_btn);

    self->save_error = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->save_error), 0);
    gtk_label_set_wrap (GTK_LABEL (self->save_error), TRUE);
    gtk_widget_add_css_class (self->save_error, "error");
    gtk_widget_set_visible (self->save_error, FALSE);
    gtk_box_append (GTK_BOX (save_box), self->save_error);
    gtk_box_append (GTK_BOX (detail_box), save_box);

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
