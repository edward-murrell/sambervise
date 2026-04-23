#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  SBV_AUTH_KERBEROS, /* GSSAPI with existing ticket */
  SBV_AUTH_SIMPLE,   /* LDAP simple bind            */
} SbvAuthType;

#define SBV_TYPE_PROFILE (sbv_profile_get_type ())
G_DECLARE_FINAL_TYPE (SbvProfile, sbv_profile, SBV, PROFILE, GObject)

SbvProfile  *sbv_profile_new           (void);
SbvProfile  *sbv_profile_copy          (SbvProfile *self);

const char  *sbv_profile_get_name      (SbvProfile *self);
const char  *sbv_profile_get_host      (SbvProfile *self);
int          sbv_profile_get_port      (SbvProfile *self);
const char  *sbv_profile_get_base_dn   (SbvProfile *self);
SbvAuthType  sbv_profile_get_auth_type (SbvProfile *self);
const char  *sbv_profile_get_bind_dn   (SbvProfile *self);
gboolean     sbv_profile_get_use_tls   (SbvProfile *self);
gboolean     sbv_profile_get_use_ldaps (SbvProfile *self);
gboolean     sbv_profile_get_skip_cert (SbvProfile *self);
/* Per-profile POSIX UID/GID range fallback (-1 = unset, use defaults) */
gint64       sbv_profile_get_uid_min   (SbvProfile *self);
gint64       sbv_profile_get_uid_max   (SbvProfile *self);
gint64       sbv_profile_get_gid_min   (SbvProfile *self);
gint64       sbv_profile_get_gid_max   (SbvProfile *self);

void sbv_profile_set_name      (SbvProfile *self, const char *name);
void sbv_profile_set_host      (SbvProfile *self, const char *host);
void sbv_profile_set_port      (SbvProfile *self, int port);
void sbv_profile_set_base_dn   (SbvProfile *self, const char *base_dn);
void sbv_profile_set_auth_type (SbvProfile *self, SbvAuthType auth_type);
void sbv_profile_set_bind_dn   (SbvProfile *self, const char *bind_dn);
void sbv_profile_set_use_tls   (SbvProfile *self, gboolean use_tls);
void sbv_profile_set_use_ldaps (SbvProfile *self, gboolean use_ldaps);
void sbv_profile_set_skip_cert (SbvProfile *self, gboolean skip_cert);
void sbv_profile_set_uid_min   (SbvProfile *self, gint64 v);
void sbv_profile_set_uid_max   (SbvProfile *self, gint64 v);
void sbv_profile_set_gid_min   (SbvProfile *self, gint64 v);
void sbv_profile_set_gid_max   (SbvProfile *self, gint64 v);

G_END_DECLS
