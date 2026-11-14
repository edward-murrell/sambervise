#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-dns-record.h"

G_BEGIN_DECLS

/* AD-integrated DNS over LDAP (R10). Zones live under CN=MicrosoftDNS in
 * the DomainDnsZones and ForestDnsZones partitions, or under CN=System
 * for older zones. No DNS RPC is used. Every change also raises the
 * zone's SOA serial by one, so secondary servers see it. */

/* One zone. */
typedef struct {
  char *name;        /* e.g. "example.com" */
  char *dn;
  char *partition;   /* "Domain", "Forest" or "Legacy" */
} SbvDnsZone;

void sbv_dns_zone_free (SbvDnsZone *zone);

/* One record and the name it sits on. */
typedef struct {
  char         *name;   /* node name, "@" for the zone itself */
  char         *dn;     /* the dnsNode DN */
  SbvDnsRecord *record;
} SbvDnsEntry;

void sbv_dns_entry_free (SbvDnsEntry *entry);

/* Lists every zone, forward zones first. The finish call returns a
 * GPtrArray of SbvDnsZone that frees its items. */
void       sbv_dns_zones_list_async   (SbvConnection       *conn,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);
GPtrArray *sbv_dns_zones_list_finish  (SbvConnection  *conn,
                                       GAsyncResult   *result,
                                       GError        **error);

/* Lists the live records in the zone at `zone_dn`, sorted by name and
 * type. Deleted names are left out. The finish call returns a GPtrArray
 * of SbvDnsEntry that frees its items. */
void       sbv_dns_records_list_async  (SbvConnection       *conn,
                                        const char          *zone_dn,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
GPtrArray *sbv_dns_records_list_finish (SbvConnection  *conn,
                                        GAsyncResult   *result,
                                        GError        **error);

/* Adds a record of an editable type to `name` in the zone, creating the
 * name if needed. Refuses a duplicate, and a CNAME next to other
 * records. */
void       sbv_dns_record_add_async    (SbvConnection       *conn,
                                        const char          *zone_dn,
                                        const char          *name,
                                        guint16              type,
                                        const char          *value,
                                        guint32              ttl,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);

/* Replaces `old` on the name at `node_dn` with a new value and TTL of
 * the same type. Fails if `old` has changed on the DC since it was
 * read. */
void       sbv_dns_record_update_async (SbvConnection       *conn,
                                        const char          *zone_dn,
                                        const char          *node_dn,
                                        SbvDnsRecord        *old,
                                        const char          *value,
                                        guint32              ttl,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);

/* Deletes `old` from the name at `node_dn`. The name goes too when it
 * has no records left, unless it is the zone itself. */
void       sbv_dns_record_delete_async (SbvConnection       *conn,
                                        const char          *zone_dn,
                                        const char          *node_dn,
                                        SbvDnsRecord        *old,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);

/* Finish call for add, update and delete. */
gboolean   sbv_dns_record_change_finish (SbvConnection  *conn,
                                         GAsyncResult   *result,
                                         GError        **error);

G_END_DECLS
