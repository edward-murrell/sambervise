#include "sbv-user.h"

struct _SbvUser {
  GObject  parent;
  char    *dn;
  char    *sam;
  char    *cn;
  char    *display_name;
  char    *given_name;
  char    *sn;
  char    *email;
  char    *description;
  gboolean enabled;
  gint64   uac;
  gint64   pwd_last_set;
  gint64   account_expires;
  /* RFC2307 / POSIX */
  gint        uid_number;   /* -1 = not present */
  gint        gid_number;   /* -1 = not present */
  char       *login_shell;
  char       *unix_home_dir;
  char       *gecos;
  /* Lockout */
  gint64      lockout_time;  /* FILETIME; 0 = never locked */
  gboolean    locked_out;
  gint64      lockout_end;   /* FILETIME; 0 = unknown; G_MAXINT64 = until unlocked */
  gboolean    password_expired;
  /* Windows profile */
  char       *home_directory;
  char       *home_drive;
  char       *script_path;
  char       *profile_path;
  /* Raw LDAP attribute dump — char* → GStrv, owned */
  GHashTable *ldap_attrs;
};

G_DEFINE_TYPE (SbvUser, sbv_user, G_TYPE_OBJECT)

static void
sbv_user_finalize (GObject *object)
{
  SbvUser *self = SBV_USER (object);
  g_free (self->dn);
  g_free (self->sam);
  g_free (self->cn);
  g_free (self->display_name);
  g_free (self->given_name);
  g_free (self->sn);
  g_free (self->email);
  g_free (self->description);
  g_free (self->login_shell);
  g_free (self->unix_home_dir);
  g_free (self->gecos);
  g_free (self->home_directory);
  g_free (self->home_drive);
  g_free (self->script_path);
  g_free (self->profile_path);
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  G_OBJECT_CLASS (sbv_user_parent_class)->finalize (object);
}

static void sbv_user_class_init (SbvUserClass *klass) { G_OBJECT_CLASS (klass)->finalize = sbv_user_finalize; }

static void
sbv_user_init (SbvUser *self)
{
  self->enabled         = TRUE;
  self->account_expires = G_MAXINT64;
  self->uid_number      = -1;
  self->gid_number      = -1;
}

SbvUser *
sbv_user_new (void)
{
  return g_object_new (SBV_TYPE_USER, NULL);
}

const char *sbv_user_get_dn              (SbvUser *self) { return self->dn; }
const char *sbv_user_get_sam             (SbvUser *self) { return self->sam; }
const char *sbv_user_get_cn              (SbvUser *self) { return self->cn; }
const char *sbv_user_get_display_name    (SbvUser *self) { return self->display_name; }
const char *sbv_user_get_given_name      (SbvUser *self) { return self->given_name; }
const char *sbv_user_get_sn              (SbvUser *self) { return self->sn; }
const char *sbv_user_get_email           (SbvUser *self) { return self->email; }
const char *sbv_user_get_description     (SbvUser *self) { return self->description; }
gboolean    sbv_user_get_enabled         (SbvUser *self) { return self->enabled; }
gint64      sbv_user_get_uac             (SbvUser *self) { return self->uac; }
gint64      sbv_user_get_pwd_last_set    (SbvUser *self) { return self->pwd_last_set; }
gint64      sbv_user_get_account_expires (SbvUser *self) { return self->account_expires; }

void sbv_user_set_dn              (SbvUser *self, const char *v) { g_free (self->dn);           self->dn           = g_strdup (v); }
void sbv_user_set_sam             (SbvUser *self, const char *v) { g_free (self->sam);          self->sam          = g_strdup (v); }
void sbv_user_set_cn              (SbvUser *self, const char *v) { g_free (self->cn);            self->cn            = g_strdup (v); }
void sbv_user_set_display_name    (SbvUser *self, const char *v) { g_free (self->display_name); self->display_name = g_strdup (v); }
void sbv_user_set_given_name      (SbvUser *self, const char *v) { g_free (self->given_name);   self->given_name   = g_strdup (v); }
void sbv_user_set_sn              (SbvUser *self, const char *v) { g_free (self->sn);            self->sn            = g_strdup (v); }
void sbv_user_set_email           (SbvUser *self, const char *v) { g_free (self->email);        self->email        = g_strdup (v); }
void sbv_user_set_description     (SbvUser *self, const char *v) { g_free (self->description);  self->description  = g_strdup (v); }
void sbv_user_set_enabled         (SbvUser *self, gboolean    v) { self->enabled         = v; }
void sbv_user_set_uac             (SbvUser *self, gint64      v) { self->uac             = v; }
void sbv_user_set_pwd_last_set    (SbvUser *self, gint64      v) { self->pwd_last_set    = v; }
void sbv_user_set_account_expires (SbvUser *self, gint64      v) { self->account_expires = v; }
void sbv_user_set_uid_number      (SbvUser *self, gint        v) { self->uid_number      = v; }
void sbv_user_set_gid_number      (SbvUser *self, gint        v) { self->gid_number      = v; }
void sbv_user_set_login_shell     (SbvUser *self, const char *v) { g_free (self->login_shell); self->login_shell = g_strdup (v); }
void sbv_user_set_unix_home_dir   (SbvUser *self, const char *v) { g_free (self->unix_home_dir); self->unix_home_dir = g_strdup (v); }
void sbv_user_set_gecos           (SbvUser *self, const char *v) { g_free (self->gecos);       self->gecos       = g_strdup (v); }

gint        sbv_user_get_uid_number  (SbvUser *self) { return self->uid_number; }
gint        sbv_user_get_gid_number  (SbvUser *self) { return self->gid_number; }
const char *sbv_user_get_login_shell (SbvUser *self) { return self->login_shell; }
const char *sbv_user_get_unix_home_dir (SbvUser *self) { return self->unix_home_dir; }
const char *sbv_user_get_gecos       (SbvUser *self) { return self->gecos; }

/* Lockout accessors */
void     sbv_user_set_lockout_time (SbvUser *self, gint64   v) { self->lockout_time = v; }
void     sbv_user_set_locked_out   (SbvUser *self, gboolean v) { self->locked_out   = v; }
void     sbv_user_set_lockout_end  (SbvUser *self, gint64   v) { self->lockout_end  = v; }
gint64   sbv_user_get_lockout_time (SbvUser *self) { return self->lockout_time; }
gboolean sbv_user_get_locked_out   (SbvUser *self) { return self->locked_out; }
gint64   sbv_user_get_lockout_end  (SbvUser *self) { return self->lockout_end; }

/* Applies Samba's lockout rule. A zero or positive duration, or the
 * "forever" value G_MININT64, means the lockout lasts until an admin
 * clears it. Otherwise it ends at lockout_time + |duration|. */
gint64
sbv_user_lockout_end (gint64 lockout_time, gint64 lockout_duration, gint64 now)
{
  if (lockout_time <= 0) return 0;
  if (lockout_duration >= 0 || lockout_duration == G_MININT64) return G_MAXINT64;

  gint64 span = -lockout_duration;
  gint64 end  = (lockout_time > G_MAXINT64 - span) ? G_MAXINT64 - 1
                                                    : lockout_time + span;
  return end > now ? end : 0;
}

/* Password expiry accessors */
void     sbv_user_set_password_expired (SbvUser *self, gboolean v) { self->password_expired = v; }
gboolean sbv_user_get_password_expired (SbvUser *self) { return self->password_expired; }

/* userAccountControl bits for accounts whose passwords never expire:
 * DONT_EXPIRE_PASSWD, SMARTCARD_REQUIRED and the three trust account
 * types. */
#define NO_EXPIRY_UAC (0x10000 | 0x40000 | 0x0800 | 0x1000 | 0x2000)

/* Applies the AD expiry rule. Accounts flagged as never expiring are
 * never expired. A pwdLastSet of 0 means the password must be changed
 * now. A zero or positive maxPwdAge, or G_MININT64, means passwords
 * never age out. */
gboolean
sbv_user_password_expired (gint64 uac, gint64 pwd_last_set,
                           gint64 max_pwd_age, gint64 now)
{
  if (uac & NO_EXPIRY_UAC) return FALSE;
  if (pwd_last_set == 0) return TRUE;
  if (pwd_last_set < 0) return FALSE;
  if (max_pwd_age >= 0 || max_pwd_age == G_MININT64) return FALSE;

  gint64 span = -max_pwd_age;
  if (pwd_last_set > G_MAXINT64 - span) return FALSE;
  return now >= pwd_last_set + span;
}

/* Windows profile accessors */
void sbv_user_set_home_directory  (SbvUser *self, const char *v) { g_free (self->home_directory); self->home_directory = g_strdup (v); }
void sbv_user_set_home_drive      (SbvUser *self, const char *v) { g_free (self->home_drive);     self->home_drive     = g_strdup (v); }
void sbv_user_set_script_path     (SbvUser *self, const char *v) { g_free (self->script_path);    self->script_path    = g_strdup (v); }
void sbv_user_set_profile_path    (SbvUser *self, const char *v) { g_free (self->profile_path);   self->profile_path   = g_strdup (v); }
const char *sbv_user_get_home_directory (SbvUser *self) { return self->home_directory; }
const char *sbv_user_get_home_drive     (SbvUser *self) { return self->home_drive; }
const char *sbv_user_get_script_path    (SbvUser *self) { return self->script_path; }
const char *sbv_user_get_profile_path   (SbvUser *self) { return self->profile_path; }

/* Accepts "h", "H", "h:" or "H:" (spaces around are ignored) and returns
 * "H:". Returns NULL for anything else. Caller frees. */
char *
sbv_user_normalise_home_drive (const char *drive)
{
  if (!drive) return NULL;
  while (*drive == ' ') drive++;
  if (!g_ascii_isalpha (drive[0])) return NULL;
  const char *rest = drive + 1;
  if (*rest == ':') rest++;
  while (*rest == ' ') rest++;
  if (*rest) return NULL;
  return g_strdup_printf ("%c:", g_ascii_toupper (drive[0]));
}

GHashTable *sbv_user_get_ldap_attrs  (SbvUser *self) { return self->ldap_attrs; }
void
sbv_user_set_ldap_attrs (SbvUser *self, GHashTable *attrs)
{
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  self->ldap_attrs = attrs;
}
