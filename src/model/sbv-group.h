#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SBV_TYPE_GROUP (sbv_group_get_type ())
G_DECLARE_FINAL_TYPE (SbvGroup, sbv_group, SBV, GROUP, GObject)

SbvGroup   *sbv_group_new              (void);

const char *sbv_group_get_dn           (SbvGroup *self);
const char *sbv_group_get_sam          (SbvGroup *self);
const char *sbv_group_get_display_name (SbvGroup *self);
const char *sbv_group_get_description  (SbvGroup *self);
/* Returns a GStrv (NULL-terminated array) of member DNs; caller does not own */
const char * const *sbv_group_get_members (SbvGroup *self);
guint        sbv_group_get_member_count   (SbvGroup *self);

void        sbv_group_set_dn           (SbvGroup *self, const char *dn);
void        sbv_group_set_sam          (SbvGroup *self, const char *sam);
void        sbv_group_set_display_name (SbvGroup *self, const char *name);
void        sbv_group_set_description  (SbvGroup *self, const char *desc);
/* Takes ownership of a NULL-terminated array of strings */
void        sbv_group_set_members      (SbvGroup *self, char **members);

G_END_DECLS
