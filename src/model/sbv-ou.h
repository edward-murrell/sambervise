#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SBV_TYPE_OU (sbv_ou_get_type ())
G_DECLARE_FINAL_TYPE (SbvOu, sbv_ou, SBV, OU, GObject)

/* What kind of directory container an SbvOu wraps. Only organisational
 * units can be renamed, moved or deleted from the UI. Containers such as
 * CN=Users are valid move targets but are otherwise left alone. */
typedef enum {
  SBV_OU_KIND_OU,         /* objectClass=organizationalUnit */
  SBV_OU_KIND_CONTAINER,  /* objectClass=container, e.g. CN=Users */
  SBV_OU_KIND_DOMAIN,     /* the domain root (base DN) */
} SbvOuKind;

/* A container that can hold users, groups, computers and contacts.
 * String getters return NULL when unset. `path` is the list of RDN values
 * from the domain root down (empty for the root itself). */

SbvOu       *sbv_ou_new              (void);

const char  *sbv_ou_get_dn           (SbvOu *self);
const char  *sbv_ou_get_name         (SbvOu *self);
const char  *sbv_ou_get_description  (SbvOu *self);
SbvOuKind    sbv_ou_get_kind         (SbvOu *self);
gboolean     sbv_ou_get_critical     (SbvOu *self); /* isCriticalSystemObject */
const char *const *sbv_ou_get_path   (SbvOu *self);
guint        sbv_ou_get_depth        (SbvOu *self); /* number of path entries */
GHashTable  *sbv_ou_get_ldap_attrs   (SbvOu *self); /* char* → GStrv, borrowed */

/* TRUE when the UI may rename, move or delete this entry: an OU that the
 * DC does not flag as a critical system object. */
gboolean     sbv_ou_is_editable      (SbvOu *self);

void         sbv_ou_set_dn           (SbvOu *self, const char *dn);
void         sbv_ou_set_name         (SbvOu *self, const char *name);
void         sbv_ou_set_description  (SbvOu *self, const char *desc);
void         sbv_ou_set_kind         (SbvOu *self, SbvOuKind kind);
void         sbv_ou_set_critical     (SbvOu *self, gboolean critical);
void         sbv_ou_set_path         (SbvOu *self, const char *const *path);
/* Takes ownership of `attrs`. */
void         sbv_ou_set_ldap_attrs   (SbvOu *self, GHashTable *attrs);

/* GCompareDataFunc that orders entries as a tree walk: each parent before
 * its children, siblings by case-insensitive name. Suitable for
 * g_list_store_sort. */
int          sbv_ou_compare_tree     (gconstpointer a,
                                      gconstpointer b,
                                      gpointer      user_data);

G_END_DECLS
