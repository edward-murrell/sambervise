#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SBV_TYPE_CONTACT (sbv_contact_get_type ())
G_DECLARE_FINAL_TYPE (SbvContact, sbv_contact, SBV, CONTACT, GObject)

/* AD `contact` object — a mail-enabled person without a logon account.
 * String getters return NULL when the underlying attribute is unset; setters
 * accept NULL or "" to clear. proxyAddresses is multi-valued (GStrv). */

SbvContact *sbv_contact_new                  (void);

const char  *sbv_contact_get_dn                          (SbvContact *self);
const char  *sbv_contact_get_cn                          (SbvContact *self);
const char  *sbv_contact_get_display_name                (SbvContact *self);
const char  *sbv_contact_get_given_name                  (SbvContact *self);
const char  *sbv_contact_get_sn                          (SbvContact *self);
const char  *sbv_contact_get_description                 (SbvContact *self);
const char  *sbv_contact_get_mail                        (SbvContact *self);
const char  *const *sbv_contact_get_proxy_addresses      (SbvContact *self);
const char  *sbv_contact_get_telephone_number            (SbvContact *self);
const char  *sbv_contact_get_mobile                      (SbvContact *self);
const char  *sbv_contact_get_fax                         (SbvContact *self); /* facsimileTelephoneNumber */
const char  *sbv_contact_get_office                      (SbvContact *self); /* physicalDeliveryOfficeName */
const char  *sbv_contact_get_street_address              (SbvContact *self);
const char  *sbv_contact_get_locality                    (SbvContact *self); /* l */
const char  *sbv_contact_get_state                       (SbvContact *self); /* st */
const char  *sbv_contact_get_postal_code                 (SbvContact *self);
const char  *sbv_contact_get_country                     (SbvContact *self); /* co (friendly country) */

void sbv_contact_set_dn               (SbvContact *self, const char *v);
void sbv_contact_set_cn               (SbvContact *self, const char *v);
void sbv_contact_set_display_name     (SbvContact *self, const char *v);
void sbv_contact_set_given_name       (SbvContact *self, const char *v);
void sbv_contact_set_sn               (SbvContact *self, const char *v);
void sbv_contact_set_description      (SbvContact *self, const char *v);
void sbv_contact_set_mail             (SbvContact *self, const char *v);
/* Takes ownership of `addrs` (a g_strfreev-able vector) — pass NULL to clear. */
void sbv_contact_set_proxy_addresses  (SbvContact *self, char **addrs);
void sbv_contact_set_telephone_number (SbvContact *self, const char *v);
void sbv_contact_set_mobile           (SbvContact *self, const char *v);
void sbv_contact_set_fax              (SbvContact *self, const char *v);
void sbv_contact_set_office           (SbvContact *self, const char *v);
void sbv_contact_set_street_address   (SbvContact *self, const char *v);
void sbv_contact_set_locality         (SbvContact *self, const char *v);
void sbv_contact_set_state            (SbvContact *self, const char *v);
void sbv_contact_set_postal_code      (SbvContact *self, const char *v);
void sbv_contact_set_country          (SbvContact *self, const char *v);

/* Raw LDAP attributes — GHashTable<char*,GStrv>; takes/returns ownership */
GHashTable *sbv_contact_get_ldap_attrs (SbvContact *self);
void        sbv_contact_set_ldap_attrs (SbvContact *self, GHashTable *attrs);

G_END_DECLS
