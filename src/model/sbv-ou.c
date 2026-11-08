#include "sbv-ou.h"

struct _SbvOu {
  GObject     parent;
  char       *dn;
  char       *name;
  char       *description;
  SbvOuKind   kind;
  gboolean    critical;
  char      **path;        /* GStrv, never NULL after init */
  GHashTable *ldap_attrs;  /* char* → GStrv, owned, may be NULL */
};

G_DEFINE_TYPE (SbvOu, sbv_ou, G_TYPE_OBJECT)

/* Releases every owned string and the raw attribute table. */
static void
sbv_ou_finalize (GObject *object)
{
  SbvOu *self = SBV_OU (object);
  g_free (self->dn);
  g_free (self->name);
  g_free (self->description);
  g_strfreev (self->path);
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  G_OBJECT_CLASS (sbv_ou_parent_class)->finalize (object);
}

/* Hooks up finalize. */
static void
sbv_ou_class_init (SbvOuClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_ou_finalize;
}

/* New entries default to an OU with an empty path. */
static void
sbv_ou_init (SbvOu *self)
{
  self->kind = SBV_OU_KIND_OU;
  self->path = g_new0 (char *, 1);
}

SbvOu *
sbv_ou_new (void)
{
  return g_object_new (SBV_TYPE_OU, NULL);
}

/* ── Accessors ── */

const char  *sbv_ou_get_dn          (SbvOu *self) { return self->dn; }
const char  *sbv_ou_get_name        (SbvOu *self) { return self->name; }
const char  *sbv_ou_get_description (SbvOu *self) { return self->description; }
SbvOuKind    sbv_ou_get_kind        (SbvOu *self) { return self->kind; }
gboolean     sbv_ou_get_critical    (SbvOu *self) { return self->critical; }
const char *const *sbv_ou_get_path  (SbvOu *self) { return (const char *const *) self->path; }
guint        sbv_ou_get_depth       (SbvOu *self) { return g_strv_length (self->path); }
GHashTable  *sbv_ou_get_ldap_attrs  (SbvOu *self) { return self->ldap_attrs; }

void sbv_ou_set_dn          (SbvOu *self, const char *v) { g_free (self->dn);          self->dn          = g_strdup (v); }
void sbv_ou_set_name        (SbvOu *self, const char *v) { g_free (self->name);        self->name        = g_strdup (v); }
void sbv_ou_set_description (SbvOu *self, const char *v) { g_free (self->description); self->description = g_strdup (v); }
void sbv_ou_set_kind        (SbvOu *self, SbvOuKind   v) { self->kind     = v; }
void sbv_ou_set_critical    (SbvOu *self, gboolean    v) { self->critical = v; }

void
sbv_ou_set_path (SbvOu *self, const char *const *path)
{
  g_strfreev (self->path);
  self->path = path ? g_strdupv ((char **) path) : g_new0 (char *, 1);
}

void
sbv_ou_set_ldap_attrs (SbvOu *self, GHashTable *attrs)
{
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  self->ldap_attrs = attrs;
}

gboolean
sbv_ou_is_editable (SbvOu *self)
{
  return self->kind == SBV_OU_KIND_OU && !self->critical;
}

int
sbv_ou_compare_tree (gconstpointer a, gconstpointer b, gpointer user_data)
{
  (void) user_data;
  SbvOu *x = SBV_OU ((gpointer) a);
  SbvOu *y = SBV_OU ((gpointer) b);

  /* Compare path entries pairwise. A shorter path that matches the longer
   * one's prefix is its ancestor, so it sorts first. */
  for (guint i = 0; ; i++) {
    const char *px = x->path[i];
    const char *py = y->path[i];
    if (!px || !py) return (px ? 1 : 0) - (py ? 1 : 0);

    char *fx = g_utf8_casefold (px, -1);
    char *fy = g_utf8_casefold (py, -1);
    int   c  = g_utf8_collate (fx, fy);
    g_free (fx);
    g_free (fy);
    if (c != 0) return c;
  }
}
