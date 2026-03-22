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
  G_OBJECT_CLASS (sbv_user_parent_class)->finalize (object);
}

static void sbv_user_class_init (SbvUserClass *klass) { G_OBJECT_CLASS (klass)->finalize = sbv_user_finalize; }

static void
sbv_user_init (SbvUser *self)
{
  self->enabled         = TRUE;
  self->account_expires = G_MAXINT64;
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
