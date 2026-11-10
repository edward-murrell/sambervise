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
/* RFC2307 / POSIX attributes; -1 means attribute not present */
gint        sbv_user_get_uid_number      (SbvUser *self);
gint        sbv_user_get_gid_number      (SbvUser *self);
const char *sbv_user_get_login_shell     (SbvUser *self);
const char *sbv_user_get_unix_home_dir   (SbvUser *self);
const char *sbv_user_get_gecos           (SbvUser *self);

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
void        sbv_user_set_uid_number      (SbvUser *self, gint uid);
void        sbv_user_set_gid_number      (SbvUser *self, gint gid);
void        sbv_user_set_login_shell     (SbvUser *self, const char *shell);
void        sbv_user_set_unix_home_dir   (SbvUser *self, const char *home);
void        sbv_user_set_gecos           (SbvUser *self, const char *gecos);
/* Lockout. lockout_time is the raw lockoutTime FILETIME (0 = not set).
 * lockout_end is when it ends: 0 = unknown, G_MAXINT64 = until an admin
 * unlocks it. */
gint64      sbv_user_get_lockout_time    (SbvUser *self);
gboolean    sbv_user_get_locked_out      (SbvUser *self);
gint64      sbv_user_get_lockout_end     (SbvUser *self);
void        sbv_user_set_lockout_time    (SbvUser *self, gint64 filetime);
void        sbv_user_set_locked_out      (SbvUser *self, gboolean locked);
void        sbv_user_set_lockout_end     (SbvUser *self, gint64 filetime);
/* Works out when a lockout ends, from lockoutTime, the domain's
 * lockoutDuration (a negative interval) and the current FILETIME.
 * Returns 0 when the account isn't locked, G_MAXINT64 when it stays
 * locked until an admin unlocks it, or the FILETIME it ends. */
gint64      sbv_user_lockout_end         (gint64 lockout_time,
                                          gint64 lockout_duration,
                                          gint64 now);
/* Password expiry. TRUE when the password has expired and must be
 * changed before the account can log on. */
gboolean    sbv_user_get_password_expired (SbvUser *self);
void        sbv_user_set_password_expired (SbvUser *self, gboolean expired);
/* Works out whether a password has expired from userAccountControl,
 * pwdLastSet, the domain's maxPwdAge (a negative interval) and the
 * current FILETIME. Used when the DC doesn't return the computed flag. */
gboolean    sbv_user_password_expired    (gint64 uac,
                                          gint64 pwd_last_set,
                                          gint64 max_pwd_age,
                                          gint64 now);
/* Windows profile: homeDirectory, homeDrive, scriptPath, profilePath.
 * NULL means the attribute is not present. */
const char *sbv_user_get_home_directory  (SbvUser *self);
const char *sbv_user_get_home_drive      (SbvUser *self);
const char *sbv_user_get_script_path     (SbvUser *self);
const char *sbv_user_get_profile_path    (SbvUser *self);
void        sbv_user_set_home_directory  (SbvUser *self, const char *path);
void        sbv_user_set_home_drive      (SbvUser *self, const char *drive);
void        sbv_user_set_script_path     (SbvUser *self, const char *path);
void        sbv_user_set_profile_path    (SbvUser *self, const char *path);
/* Turns a drive letter such as "h" or "H:" into "H:". Returns NULL when
 * the text isn't a single letter with an optional colon. Caller frees. */
char       *sbv_user_normalise_home_drive (const char *drive);
/* Raw LDAP attributes — GHashTable<char*,GStrv>; takes/returns ownership */
GHashTable *sbv_user_get_ldap_attrs      (SbvUser *self);
void        sbv_user_set_ldap_attrs      (SbvUser *self, GHashTable *attrs);

G_END_DECLS
