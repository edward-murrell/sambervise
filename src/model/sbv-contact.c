#include "sbv-contact.h"

struct _SbvContact {
  GObject  parent;
  char    *dn;
  char    *cn;
  char    *display_name;
  char    *given_name;
  char    *sn;
  char    *description;
  char    *mail;
  char   **proxy_addresses;   /* GStrv or NULL */
  char    *telephone_number;
  char    *mobile;
  char    *fax;
  char    *office;
  char    *street_address;
  char    *locality;
  char    *state;
  char    *postal_code;
  char    *country;
  /* Raw LDAP attribute dump — char* → GStrv, owned */
  GHashTable *ldap_attrs;
};

G_DEFINE_TYPE (SbvContact, sbv_contact, G_TYPE_OBJECT)

static void
sbv_contact_finalize (GObject *object)
{
  SbvContact *self = SBV_CONTACT (object);
  g_free (self->dn);
  g_free (self->cn);
  g_free (self->display_name);
  g_free (self->given_name);
  g_free (self->sn);
  g_free (self->description);
  g_free (self->mail);
  g_strfreev (self->proxy_addresses);
  g_free (self->telephone_number);
  g_free (self->mobile);
  g_free (self->fax);
  g_free (self->office);
  g_free (self->street_address);
  g_free (self->locality);
  g_free (self->state);
  g_free (self->postal_code);
  g_free (self->country);
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  G_OBJECT_CLASS (sbv_contact_parent_class)->finalize (object);
}

static void sbv_contact_class_init (SbvContactClass *klass) { G_OBJECT_CLASS (klass)->finalize = sbv_contact_finalize; }
static void sbv_contact_init       (SbvContact *self)       { (void) self; }

/* Allocates a fresh contact with all fields unset (NULL). */
SbvContact *
sbv_contact_new (void)
{
  return g_object_new (SBV_TYPE_CONTACT, NULL);
}

/* Trivial string getters — return the underlying NUL-terminated value or NULL. */
const char *sbv_contact_get_dn               (SbvContact *self) { return self->dn; }
const char *sbv_contact_get_cn               (SbvContact *self) { return self->cn; }
const char *sbv_contact_get_display_name     (SbvContact *self) { return self->display_name; }
const char *sbv_contact_get_given_name       (SbvContact *self) { return self->given_name; }
const char *sbv_contact_get_sn               (SbvContact *self) { return self->sn; }
const char *sbv_contact_get_description      (SbvContact *self) { return self->description; }
const char *sbv_contact_get_mail             (SbvContact *self) { return self->mail; }
const char *sbv_contact_get_telephone_number (SbvContact *self) { return self->telephone_number; }
const char *sbv_contact_get_mobile           (SbvContact *self) { return self->mobile; }
const char *sbv_contact_get_fax              (SbvContact *self) { return self->fax; }
const char *sbv_contact_get_office           (SbvContact *self) { return self->office; }
const char *sbv_contact_get_street_address   (SbvContact *self) { return self->street_address; }
const char *sbv_contact_get_locality         (SbvContact *self) { return self->locality; }
const char *sbv_contact_get_state            (SbvContact *self) { return self->state; }
const char *sbv_contact_get_postal_code      (SbvContact *self) { return self->postal_code; }
const char *sbv_contact_get_country          (SbvContact *self) { return self->country; }

/* Returns the multi-valued proxyAddresses vector (NULL-terminated) or NULL. */
const char *const *
sbv_contact_get_proxy_addresses (SbvContact *self)
{
  return (const char *const *) self->proxy_addresses;
}

/* Trivial string setters — duplicate the input. Caller may pass NULL to clear. */
void sbv_contact_set_dn               (SbvContact *self, const char *v) { g_free (self->dn);                self->dn               = g_strdup (v); }
void sbv_contact_set_cn               (SbvContact *self, const char *v) { g_free (self->cn);                self->cn               = g_strdup (v); }
void sbv_contact_set_display_name     (SbvContact *self, const char *v) { g_free (self->display_name);      self->display_name     = g_strdup (v); }
void sbv_contact_set_given_name       (SbvContact *self, const char *v) { g_free (self->given_name);        self->given_name       = g_strdup (v); }
void sbv_contact_set_sn               (SbvContact *self, const char *v) { g_free (self->sn);                self->sn               = g_strdup (v); }
void sbv_contact_set_description      (SbvContact *self, const char *v) { g_free (self->description);       self->description      = g_strdup (v); }
void sbv_contact_set_mail             (SbvContact *self, const char *v) { g_free (self->mail);              self->mail             = g_strdup (v); }
void sbv_contact_set_telephone_number (SbvContact *self, const char *v) { g_free (self->telephone_number);  self->telephone_number = g_strdup (v); }
void sbv_contact_set_mobile           (SbvContact *self, const char *v) { g_free (self->mobile);            self->mobile           = g_strdup (v); }
void sbv_contact_set_fax              (SbvContact *self, const char *v) { g_free (self->fax);               self->fax              = g_strdup (v); }
void sbv_contact_set_office           (SbvContact *self, const char *v) { g_free (self->office);            self->office           = g_strdup (v); }
void sbv_contact_set_street_address   (SbvContact *self, const char *v) { g_free (self->street_address);    self->street_address   = g_strdup (v); }
void sbv_contact_set_locality         (SbvContact *self, const char *v) { g_free (self->locality);          self->locality         = g_strdup (v); }
void sbv_contact_set_state            (SbvContact *self, const char *v) { g_free (self->state);             self->state            = g_strdup (v); }
void sbv_contact_set_postal_code      (SbvContact *self, const char *v) { g_free (self->postal_code);       self->postal_code      = g_strdup (v); }
void sbv_contact_set_country          (SbvContact *self, const char *v) { g_free (self->country);           self->country          = g_strdup (v); }

/* Takes ownership of `addrs` — the contact frees it on dispose. */
void
sbv_contact_set_proxy_addresses (SbvContact *self, char **addrs)
{
  g_strfreev (self->proxy_addresses);
  self->proxy_addresses = addrs;
}

GHashTable *sbv_contact_get_ldap_attrs (SbvContact *self) { return self->ldap_attrs; }
void
sbv_contact_set_ldap_attrs (SbvContact *self, GHashTable *attrs)
{
  g_clear_pointer (&self->ldap_attrs, g_hash_table_unref);
  self->ldap_attrs = attrs;
}
