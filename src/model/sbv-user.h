#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SBV_TYPE_USER (sbv_user_get_type ())
G_DECLARE_FINAL_TYPE (SbvUser, sbv_user, SBV, USER, GObject)

SbvUser    *sbv_user_new              (void);

const char *sbv_user_get_dn           (SbvUser *self);
const char *sbv_user_get_sam          (SbvUser *self);
const char *sbv_user_get_display_name (SbvUser *self);
const char *sbv_user_get_email        (SbvUser *self);
const char *sbv_user_get_description  (SbvUser *self);
gboolean    sbv_user_get_enabled      (SbvUser *self);

void        sbv_user_set_dn           (SbvUser *self, const char *dn);
void        sbv_user_set_sam          (SbvUser *self, const char *sam);
void        sbv_user_set_display_name (SbvUser *self, const char *name);
void        sbv_user_set_email        (SbvUser *self, const char *email);
void        sbv_user_set_description  (SbvUser *self, const char *desc);
void        sbv_user_set_enabled      (SbvUser *self, gboolean enabled);

G_END_DECLS
