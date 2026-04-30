#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-ldap-node.h"

G_BEGIN_DECLS

/* Fetch the immediate children of `parent_dn` (LDAP scope = ONELEVEL).
 * Returns a GListStore<SbvLdapNode> of the discovered entries; each node
 * carries its full attribute dump so the detail pane can render it
 * without a follow-up call. */
void        sbv_ldap_browse_children_async  (SbvConnection       *conn,
                                              const char          *parent_dn,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
GListStore *sbv_ldap_browse_children_finish (SbvConnection *conn,
                                              GAsyncResult  *result,
                                              GError       **error);

/* Fetch only the attributes of `dn` (scope = BASE). Useful for the root
 * node, which has no parent listing to inherit attributes from.
 * Returns a GHashTable<char*, GStrv> via finish (caller frees). */
void        sbv_ldap_browse_attrs_async     (SbvConnection       *conn,
                                              const char          *dn,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
GHashTable *sbv_ldap_browse_attrs_finish    (SbvConnection *conn,
                                              GAsyncResult  *result,
                                              GError       **error);

/* Subtree search rooted at `base_dn` with an arbitrary user-supplied LDAP
 * filter (e.g. "(sAMAccountName=alice)"). The base DN must be non-NULL —
 * pass the connection's base DN to search the whole directory. The result
 * is capped at 500 entries to keep the UI responsive; if the DC returns
 * LDAP_SIZELIMIT_EXCEEDED the truncated set is still returned successfully. */
void        sbv_ldap_browse_search_async    (SbvConnection       *conn,
                                              const char          *base_dn,
                                              const char          *filter,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
GListStore *sbv_ldap_browse_search_finish   (SbvConnection *conn,
                                              GAsyncResult  *result,
                                              GError       **error);

/* ── Per-attribute modify ─────────────────────────────────────────────── */

/* A single modification to apply to one entry. `values` is a NULL-terminated
 * array of UTF-8 strings; for SBV_LDAP_MOD_DELETE pass NULL/empty to delete
 * the attribute outright, or a value list to delete just those values. */
typedef enum {
  SBV_LDAP_MOD_ADD,
  SBV_LDAP_MOD_REPLACE,
  SBV_LDAP_MOD_DELETE,
} SbvLdapModOp;

typedef struct {
  SbvLdapModOp   op;
  char          *attr;     /* owned */
  char         **values;   /* NULL-terminated GStrv, owned (may be NULL)  */
} SbvLdapMod;

SbvLdapMod *sbv_ldap_mod_new   (SbvLdapModOp op,
                                 const char  *attr,
                                 const char * const *values);
void        sbv_ldap_mod_free  (SbvLdapMod *m);

/* Apply a batch of mods to a single DN in one LDAP modify request. Mods is
 * a GPtrArray<SbvLdapMod*>; the call takes ownership of neither the array
 * nor its elements (caller frees after the async completes). */
void        sbv_ldap_browse_modify_async    (SbvConnection       *conn,
                                              const char          *dn,
                                              GPtrArray           *mods,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
gboolean    sbv_ldap_browse_modify_finish   (SbvConnection *conn,
                                              GAsyncResult  *result,
                                              GError       **error);

G_END_DECLS
