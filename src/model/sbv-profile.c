#include "sbv-profile.h"

struct _SbvProfile {
  GObject      parent;
  char        *name;
  char        *host;
  int          port;
  char        *base_dn;
  SbvAuthType  auth_type;
  char        *bind_dn;
  gboolean     use_tls;
  gboolean     use_ldaps;
  gboolean     skip_cert;
};

G_DEFINE_TYPE (SbvProfile, sbv_profile, G_TYPE_OBJECT)

static void
sbv_profile_finalize (GObject *object)
{
  SbvProfile *self = SBV_PROFILE (object);
  g_free (self->name);
  g_free (self->host);
  g_free (self->base_dn);
  g_free (self->bind_dn);
  G_OBJECT_CLASS (sbv_profile_parent_class)->finalize (object);
}

static void
sbv_profile_class_init (SbvProfileClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_profile_finalize;
}

static void
sbv_profile_init (SbvProfile *self)
{
  self->port      = 389;
  self->auth_type = SBV_AUTH_KERBEROS;
}

SbvProfile *
sbv_profile_new (void)
{
  return g_object_new (SBV_TYPE_PROFILE, NULL);
}

SbvProfile *
sbv_profile_copy (SbvProfile *self)
{
  SbvProfile *copy = sbv_profile_new ();
  copy->name      = g_strdup (self->name);
  copy->host      = g_strdup (self->host);
  copy->port      = self->port;
  copy->base_dn   = g_strdup (self->base_dn);
  copy->auth_type = self->auth_type;
  copy->bind_dn   = g_strdup (self->bind_dn);
  copy->use_tls   = self->use_tls;
  copy->use_ldaps = self->use_ldaps;
  copy->skip_cert = self->skip_cert;
  return copy;
}

const char  *sbv_profile_get_name      (SbvProfile *self) { return self->name; }
const char  *sbv_profile_get_host      (SbvProfile *self) { return self->host; }
int          sbv_profile_get_port      (SbvProfile *self) { return self->port; }
const char  *sbv_profile_get_base_dn   (SbvProfile *self) { return self->base_dn; }
SbvAuthType  sbv_profile_get_auth_type (SbvProfile *self) { return self->auth_type; }
const char  *sbv_profile_get_bind_dn   (SbvProfile *self) { return self->bind_dn; }
gboolean     sbv_profile_get_use_tls   (SbvProfile *self) { return self->use_tls; }
gboolean     sbv_profile_get_use_ldaps (SbvProfile *self) { return self->use_ldaps; }
gboolean     sbv_profile_get_skip_cert (SbvProfile *self) { return self->skip_cert; }

void sbv_profile_set_name      (SbvProfile *self, const char *v) { g_free (self->name);    self->name    = g_strdup (v); }
void sbv_profile_set_host      (SbvProfile *self, const char *v) { g_free (self->host);    self->host    = g_strdup (v); }
void sbv_profile_set_port      (SbvProfile *self, int         v) { self->port      = v; }
void sbv_profile_set_base_dn   (SbvProfile *self, const char *v) { g_free (self->base_dn); self->base_dn = g_strdup (v); }
void sbv_profile_set_auth_type (SbvProfile *self, SbvAuthType v) { self->auth_type = v; }
void sbv_profile_set_bind_dn   (SbvProfile *self, const char *v) { g_free (self->bind_dn); self->bind_dn = g_strdup (v); }
void sbv_profile_set_use_tls   (SbvProfile *self, gboolean    v) { self->use_tls   = v; }
void sbv_profile_set_use_ldaps (SbvProfile *self, gboolean    v) { self->use_ldaps = v; }
void sbv_profile_set_skip_cert (SbvProfile *self, gboolean    v) { self->skip_cert = v; }
