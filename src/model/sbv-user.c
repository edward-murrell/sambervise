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
  char       *home_dir;
  char       *gecos;
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
  g_free (self->home_dir);
  g_free (self->gecos);
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
void sbv_user_set_home_dir        (SbvUser *self, const char *v) { g_free (self->home_dir);    self->home_dir    = g_strdup (v); }
void sbv_user_set_gecos           (SbvUser *self, const char *v) { g_free (self->gecos);       self->gecos       = g_strdup (v); }

gint        sbv_user_get_uid_number  (SbvUser *self) { return self->uid_number; }
gint        sbv_user_get_gid_number  (SbvUser *self) { return self->gid_number; }
const char *sbv_user_get_login_shell (SbvUser *self) { return self->login_shell; }
const char *sbv_user_get_home_dir    (SbvUser *self) { return self->home_dir; }
const char *sbv_user_get_gecos       (SbvUser *self) { return self->gecos; }

GHashTable *sbv_user_get_ldap_attrs  (SbvUser *self) { return self->ldap_attrs; }
void
sbv_user_set_ldap_attrs (SbvUser *self, GHashTable *attrs)
{
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  self->ldap_attrs = attrs;
}
