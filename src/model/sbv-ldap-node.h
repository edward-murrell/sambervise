#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* A single entry in the raw-LDAP tree browser.
 *
 * Each node represents one LDAP entry. The `children` GListStore is created
 * lazily — NULL until the entry is expanded, after which it is populated by
 * the browse backend. `attrs` holds the entry's full attribute dump as a
 * GHashTable<char*, GStrv> (same shape as the model objects' raw attrs). */
#define SBV_TYPE_LDAP_NODE (sbv_ldap_node_get_type ())
G_DECLARE_FINAL_TYPE (SbvLdapNode, sbv_ldap_node, SBV, LDAP_NODE, GObject)

SbvLdapNode *sbv_ldap_node_new           (const char *dn,
                                           const char *label);

const char  *sbv_ldap_node_get_dn        (SbvLdapNode *self);
const char  *sbv_ldap_node_get_label     (SbvLdapNode *self);

/* Returns the children store, creating an empty one on first access.
 * Owned by the node — callers must not unref. */
GListStore  *sbv_ldap_node_get_children  (SbvLdapNode *self);

/* TRUE once a children-load has completed (success or failure). Used by the
 * UI to avoid re-issuing the LDAP search on subsequent expand toggles. */
gboolean     sbv_ldap_node_get_loaded    (SbvLdapNode *self);
void         sbv_ldap_node_set_loaded    (SbvLdapNode *self, gboolean v);

/* Raw LDAP attributes (GHashTable<char*, GStrv>). Takes/returns ownership. */
GHashTable  *sbv_ldap_node_get_attrs     (SbvLdapNode *self);
void         sbv_ldap_node_set_attrs     (SbvLdapNode *self, GHashTable *attrs);

/* Tristate: -1 unknown (default), 0 known leaf, 1 known to have children.
 * Sourced from the LDAP `hasSubordinates` operational attribute when the
 * server publishes it. The browser uses this to suppress the expander on
 * leaf nodes; nodes in the "unknown" state still get an expander so the
 * tree remains explorable when the attribute isn't returned. */
int          sbv_ldap_node_get_has_children (SbvLdapNode *self);
void         sbv_ldap_node_set_has_children (SbvLdapNode *self, int v);

G_END_DECLS
