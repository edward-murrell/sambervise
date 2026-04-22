#include "sbv-computer.h"

struct _SbvComputer {
  GObject  parent;
  char    *dn;
  char    *sam;
  char    *cn;
  char    *description;
  char    *dns_hostname;
  char    *os;
  char    *os_version;
  char    *os_service_pack;
  gboolean enabled;
  gint64   uac;
  gint64   pwd_last_set;
  char   **spn;          /* GStrv of servicePrincipalName values, owned */
  /* Raw LDAP attribute dump — char* → GStrv, owned */
  GHashTable *ldap_attrs;
};

G_DEFINE_TYPE (SbvComputer, sbv_computer, G_TYPE_OBJECT)

static void
sbv_computer_finalize (GObject *object)
{
  SbvComputer *self = SBV_COMPUTER (object);
  g_free (self->dn);
  g_free (self->sam);
  g_free (self->cn);
  g_free (self->description);
  g_free (self->dns_hostname);
  g_free (self->os);
  g_free (self->os_version);
  g_free (self->os_service_pack);
  g_strfreev (self->spn);
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  G_OBJECT_CLASS (sbv_computer_parent_class)->finalize (object);
}

static void sbv_computer_class_init (SbvComputerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_computer_finalize;
}

static void
sbv_computer_init (SbvComputer *self)
{
  self->enabled = TRUE;
}

SbvComputer *
sbv_computer_new (void)
{
  return g_object_new (SBV_TYPE_COMPUTER, NULL);
}

const char *sbv_computer_get_dn              (SbvComputer *self) { return self->dn; }
const char *sbv_computer_get_sam             (SbvComputer *self) { return self->sam; }
const char *sbv_computer_get_cn              (SbvComputer *self) { return self->cn; }
const char *sbv_computer_get_description     (SbvComputer *self) { return self->description; }
const char *sbv_computer_get_dns_hostname    (SbvComputer *self) { return self->dns_hostname; }
const char *sbv_computer_get_os              (SbvComputer *self) { return self->os; }
const char *sbv_computer_get_os_version      (SbvComputer *self) { return self->os_version; }
const char *sbv_computer_get_os_service_pack (SbvComputer *self) { return self->os_service_pack; }
gboolean    sbv_computer_get_enabled         (SbvComputer *self) { return self->enabled; }
gint64      sbv_computer_get_uac             (SbvComputer *self) { return self->uac; }
gint64      sbv_computer_get_pwd_last_set    (SbvComputer *self) { return self->pwd_last_set; }

const char * const *
sbv_computer_get_spn (SbvComputer *self)
{
  return (const char * const *) self->spn;
}

guint
sbv_computer_get_spn_count (SbvComputer *self)
{
  if (!self->spn) return 0;
  return g_strv_length (self->spn);
}

void sbv_computer_set_dn              (SbvComputer *self, const char *v) { g_free (self->dn);              self->dn              = g_strdup (v); }
void sbv_computer_set_sam             (SbvComputer *self, const char *v) { g_free (self->sam);             self->sam             = g_strdup (v); }
void sbv_computer_set_cn              (SbvComputer *self, const char *v) { g_free (self->cn);              self->cn              = g_strdup (v); }
void sbv_computer_set_description     (SbvComputer *self, const char *v) { g_free (self->description);     self->description     = g_strdup (v); }
void sbv_computer_set_dns_hostname    (SbvComputer *self, const char *v) { g_free (self->dns_hostname);    self->dns_hostname    = g_strdup (v); }
void sbv_computer_set_os              (SbvComputer *self, const char *v) { g_free (self->os);              self->os              = g_strdup (v); }
void sbv_computer_set_os_version      (SbvComputer *self, const char *v) { g_free (self->os_version);      self->os_version      = g_strdup (v); }
void sbv_computer_set_os_service_pack (SbvComputer *self, const char *v) { g_free (self->os_service_pack); self->os_service_pack = g_strdup (v); }
void sbv_computer_set_enabled         (SbvComputer *self, gboolean v)    { self->enabled      = v; }
void sbv_computer_set_uac             (SbvComputer *self, gint64 v)      { self->uac          = v; }
void sbv_computer_set_pwd_last_set    (SbvComputer *self, gint64 v)      { self->pwd_last_set = v; }

void
sbv_computer_set_spn (SbvComputer *self, char **spns)
{
  g_strfreev (self->spn);
  self->spn = spns;
}

GHashTable *sbv_computer_get_ldap_attrs (SbvComputer *self) { return self->ldap_attrs; }
void
sbv_computer_set_ldap_attrs (SbvComputer *self, GHashTable *attrs)
{
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  self->ldap_attrs = attrs;
}
