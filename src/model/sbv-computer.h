#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SBV_TYPE_COMPUTER (sbv_computer_get_type ())
G_DECLARE_FINAL_TYPE (SbvComputer, sbv_computer, SBV, COMPUTER, GObject)

SbvComputer *sbv_computer_new                  (void);

const char  *sbv_computer_get_dn               (SbvComputer *self);
const char  *sbv_computer_get_sam              (SbvComputer *self); /* sAMAccountName, ends with $ */
const char  *sbv_computer_get_cn               (SbvComputer *self);
const char  *sbv_computer_get_description      (SbvComputer *self);
const char  *sbv_computer_get_dns_hostname     (SbvComputer *self); /* dNSHostName */
const char  *sbv_computer_get_os               (SbvComputer *self); /* operatingSystem */
const char  *sbv_computer_get_os_version       (SbvComputer *self); /* operatingSystemVersion */
const char  *sbv_computer_get_os_service_pack  (SbvComputer *self); /* operatingSystemServicePack */
gboolean     sbv_computer_get_enabled          (SbvComputer *self);
gint64       sbv_computer_get_uac              (SbvComputer *self);
gint64       sbv_computer_get_pwd_last_set     (SbvComputer *self);
/* Returns a GStrv of servicePrincipalName values; caller does not own */
const char * const *sbv_computer_get_spn       (SbvComputer *self);
guint               sbv_computer_get_spn_count (SbvComputer *self);

void sbv_computer_set_dn               (SbvComputer *self, const char *dn);
void sbv_computer_set_sam              (SbvComputer *self, const char *sam);
void sbv_computer_set_cn               (SbvComputer *self, const char *cn);
void sbv_computer_set_description      (SbvComputer *self, const char *desc);
void sbv_computer_set_dns_hostname     (SbvComputer *self, const char *host);
void sbv_computer_set_os               (SbvComputer *self, const char *os);
void sbv_computer_set_os_version       (SbvComputer *self, const char *ver);
void sbv_computer_set_os_service_pack  (SbvComputer *self, const char *sp);
void sbv_computer_set_enabled          (SbvComputer *self, gboolean enabled);
void sbv_computer_set_uac              (SbvComputer *self, gint64 uac);
void sbv_computer_set_pwd_last_set     (SbvComputer *self, gint64 val);
/* Takes ownership of a NULL-terminated array of strings */
void sbv_computer_set_spn              (SbvComputer *self, char **spns);

/* Raw LDAP attributes — GHashTable<char*,GStrv>; takes/returns ownership */
GHashTable *sbv_computer_get_ldap_attrs (SbvComputer *self);
void        sbv_computer_set_ldap_attrs (SbvComputer *self, GHashTable *attrs);

G_END_DECLS
