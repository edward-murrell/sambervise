#include "sbv-ldap-node.h"

struct _SbvLdapNode {
  GObject     parent;
  char       *dn;
  char       *label;
  GListStore *children;    /* lazy: NULL until first request           */
  gboolean    loaded;
  GHashTable *attrs;       /* GHashTable<char*, GStrv>, owned          */
  int         has_children; /* -1 unknown, 0 leaf, 1 has children       */
};

G_DEFINE_TYPE (SbvLdapNode, sbv_ldap_node, G_TYPE_OBJECT)

/* GObject finalize — frees everything the node owns. */
static void
sbv_ldap_node_finalize (GObject *object)
{
  SbvLdapNode *self = SBV_LDAP_NODE (object);
  g_free (self->dn);
  g_free (self->label);
  g_clear_object (&self->children);
  g_clear_pointer (&self->attrs, g_hash_table_unref);
  G_OBJECT_CLASS (sbv_ldap_node_parent_class)->finalize (object);
}

static void sbv_ldap_node_class_init (SbvLdapNodeClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_ldap_node_finalize;
}

static void sbv_ldap_node_init (SbvLdapNode *self) { (void) self; }

/* Allocates a new node with the given DN and display label. The label is
 * usually the leftmost RDN value but the caller decides — for the root
 * we typically pass the full DN. */
SbvLdapNode *
sbv_ldap_node_new (const char *dn, const char *label)
{
  SbvLdapNode *n = g_object_new (SBV_TYPE_LDAP_NODE, NULL);
  n->dn           = g_strdup (dn);
  n->label        = g_strdup (label);
  n->has_children = -1;
  return n;
}

const char *sbv_ldap_node_get_dn    (SbvLdapNode *self) { return self->dn; }
const char *sbv_ldap_node_get_label (SbvLdapNode *self) { return self->label; }

/* Lazily allocates the children store on first access. */
GListStore *
sbv_ldap_node_get_children (SbvLdapNode *self)
{
  if (!self->children)
    self->children = g_list_store_new (SBV_TYPE_LDAP_NODE);
  return self->children;
}

gboolean sbv_ldap_node_get_loaded (SbvLdapNode *self)              { return self->loaded; }
void     sbv_ldap_node_set_loaded (SbvLdapNode *self, gboolean v)  { self->loaded = v; }

GHashTable *sbv_ldap_node_get_attrs (SbvLdapNode *self) { return self->attrs; }

/* Replaces the attributes table; takes ownership. */
void
sbv_ldap_node_set_attrs (SbvLdapNode *self, GHashTable *attrs)
{
  g_clear_pointer (&self->attrs, g_hash_table_unref);
  self->attrs = attrs;
}

int  sbv_ldap_node_get_has_children (SbvLdapNode *self)        { return self->has_children; }
void sbv_ldap_node_set_has_children (SbvLdapNode *self, int v) { self->has_children = v; }
