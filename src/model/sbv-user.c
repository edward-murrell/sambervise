#include "sbv-user.h"

struct _SbvUser {
  GObject  parent;
  char    *dn;
  char    *sam;
  char    *display_name;
  char    *email;
  char    *description;
  gboolean enabled;
};

G_DEFINE_TYPE (SbvUser, sbv_user, G_TYPE_OBJECT)

static void
sbv_user_finalize (GObject *object)
{
  SbvUser *self = SBV_USER (object);
  g_free (self->dn);
  g_free (self->sam);
  g_free (self->display_name);
  g_free (self->email);
  g_free (self->description);
  G_OBJECT_CLASS (sbv_user_parent_class)->finalize (object);
}

static void
sbv_user_class_init (SbvUserClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_user_finalize;
}

static void
sbv_user_init (SbvUser *self)
{
  self->enabled = TRUE;
}

SbvUser *
sbv_user_new (void)
{
  return g_object_new (SBV_TYPE_USER, NULL);
}

const char *sbv_user_get_dn           (SbvUser *self) { return self->dn; }
const char *sbv_user_get_sam          (SbvUser *self) { return self->sam; }
const char *sbv_user_get_display_name (SbvUser *self) { return self->display_name; }
const char *sbv_user_get_email        (SbvUser *self) { return self->email; }
const char *sbv_user_get_description  (SbvUser *self) { return self->description; }
gboolean    sbv_user_get_enabled      (SbvUser *self) { return self->enabled; }

void sbv_user_set_dn           (SbvUser *self, const char *v) { g_free (self->dn);           self->dn           = g_strdup (v); }
void sbv_user_set_sam          (SbvUser *self, const char *v) { g_free (self->sam);          self->sam          = g_strdup (v); }
void sbv_user_set_display_name (SbvUser *self, const char *v) { g_free (self->display_name); self->display_name = g_strdup (v); }
void sbv_user_set_email        (SbvUser *self, const char *v) { g_free (self->email);        self->email        = g_strdup (v); }
void sbv_user_set_description  (SbvUser *self, const char *v) { g_free (self->description);  self->description  = g_strdup (v); }
void sbv_user_set_enabled      (SbvUser *self, gboolean    v) { self->enabled = v; }
