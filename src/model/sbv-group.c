#include "sbv-group.h"

struct _SbvGroup {
  GObject  parent;
  char    *dn;
  char    *sam;
  char    *display_name;
  char    *description;
  gint32   group_type;
  char   **members; /* GStrv, owned */
};

G_DEFINE_TYPE (SbvGroup, sbv_group, G_TYPE_OBJECT)

static void
sbv_group_finalize (GObject *object)
{
  SbvGroup *self = SBV_GROUP (object);
  g_free (self->dn);
  g_free (self->sam);
  g_free (self->display_name);
  g_free (self->description);
  g_strfreev (self->members);
  G_OBJECT_CLASS (sbv_group_parent_class)->finalize (object);
}

static void sbv_group_class_init (SbvGroupClass *klass) { G_OBJECT_CLASS (klass)->finalize = sbv_group_finalize; }
static void sbv_group_init       (SbvGroup *self)        { (void) self; }

SbvGroup *
sbv_group_new (void)
{
  return g_object_new (SBV_TYPE_GROUP, NULL);
}

const char *sbv_group_get_dn           (SbvGroup *self) { return self->dn; }
const char *sbv_group_get_sam          (SbvGroup *self) { return self->sam; }
const char *sbv_group_get_display_name (SbvGroup *self) { return self->display_name; }
const char *sbv_group_get_description  (SbvGroup *self) { return self->description; }
gint32      sbv_group_get_group_type   (SbvGroup *self) { return self->group_type; }

const char * const *
sbv_group_get_members (SbvGroup *self)
{
  return (const char * const *) self->members;
}

guint
sbv_group_get_member_count (SbvGroup *self)
{
  if (!self->members)
    return 0;
  return g_strv_length (self->members);
}

void sbv_group_set_dn           (SbvGroup *self, const char *v) { g_free (self->dn);           self->dn           = g_strdup (v); }
void sbv_group_set_sam          (SbvGroup *self, const char *v) { g_free (self->sam);          self->sam          = g_strdup (v); }
void sbv_group_set_display_name (SbvGroup *self, const char *v) { g_free (self->display_name); self->display_name = g_strdup (v); }
void sbv_group_set_description  (SbvGroup *self, const char *v) { g_free (self->description);  self->description  = g_strdup (v); }
void sbv_group_set_group_type   (SbvGroup *self, gint32      v) { self->group_type = v; }

void
sbv_group_set_members (SbvGroup *self, char **members)
{
  g_strfreev (self->members);
  self->members = members;
}
