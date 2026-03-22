#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SBV_TYPE_USER (sbv_user_get_type ())
G_DECLARE_FINAL_TYPE (SbvUser, sbv_user, SBV, USER, GObject)

SbvUser    *sbv_user_new                 (void);

const char *sbv_user_get_dn              (SbvUser *self);
const char *sbv_user_get_sam             (SbvUser *self);
const char *sbv_user_get_cn              (SbvUser *self); /* cn = full name */
const char *sbv_user_get_display_name    (SbvUser *self); /* displayName */
const char *sbv_user_get_given_name      (SbvUser *self); /* givenName */
const char *sbv_user_get_sn              (SbvUser *self); /* sn = surname */
const char *sbv_user_get_email           (SbvUser *self);
const char *sbv_user_get_description     (SbvUser *self);
gboolean    sbv_user_get_enabled         (SbvUser *self);
gint64      sbv_user_get_uac             (SbvUser *self); /* raw userAccountControl */
gint64      sbv_user_get_pwd_last_set    (SbvUser *self); /* 0 = must change at next logon */
gint64      sbv_user_get_account_expires (SbvUser *self); /* Windows FILETIME; G_MAXINT64 = never */

void        sbv_user_set_dn              (SbvUser *self, const char *dn);
void        sbv_user_set_sam             (SbvUser *self, const char *sam);
void        sbv_user_set_cn              (SbvUser *self, const char *cn);
void        sbv_user_set_display_name    (SbvUser *self, const char *name);
void        sbv_user_set_given_name      (SbvUser *self, const char *given);
void        sbv_user_set_sn              (SbvUser *self, const char *sn);
void        sbv_user_set_email           (SbvUser *self, const char *email);
void        sbv_user_set_description     (SbvUser *self, const char *desc);
void        sbv_user_set_enabled         (SbvUser *self, gboolean enabled);
void        sbv_user_set_uac             (SbvUser *self, gint64 uac);
void        sbv_user_set_pwd_last_set    (SbvUser *self, gint64 val);
void        sbv_user_set_account_expires (SbvUser *self, gint64 val);

G_END_DECLS
