#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* AD-integrated DNS data. Zones are dnsZone objects and names are
 * dnsNode objects in LDAP. Each node's dnsRecord values hold one record
 * each in the binary layout from MS-DNSP 2.3.2.2. These helpers decode
 * and build those values. Pure functions, no I/O. */

enum {
  SBV_DNS_TYPE_TOMBSTONE = 0,
  SBV_DNS_TYPE_A         = 1,
  SBV_DNS_TYPE_NS        = 2,
  SBV_DNS_TYPE_CNAME     = 5,
  SBV_DNS_TYPE_SOA       = 6,
  SBV_DNS_TYPE_PTR       = 12,
  SBV_DNS_TYPE_MX        = 15,
  SBV_DNS_TYPE_TXT       = 16,
  SBV_DNS_TYPE_AAAA      = 28,
  SBV_DNS_TYPE_SRV       = 33,
};

/* Rank of records the zone itself owns. Sambervise writes this. */
#define SBV_DNS_RANK_ZONE 0xF0

/* TTL samba-tool uses for new records. */
#define SBV_DNS_DEFAULT_TTL 900

/* One decoded dnsRecord value. */
typedef struct {
  guint16  type;
  guint8   rank;
  guint32  serial;
  guint32  ttl;
  guint32  timestamp;   /* hours since 1601; 0 for a static record */
  char    *value;       /* data as text, e.g. "192.168.1.5" or "host.example.com" */
  GBytes  *raw;         /* the whole value as stored, for exact deletes */
} SbvDnsRecord;

void          sbv_dns_record_free  (SbvDnsRecord *record);

/* Decodes one dnsRecord value. Unknown types keep their data as hex.
 * Returns NULL when the value is too short or malformed. */
SbvDnsRecord *sbv_dns_record_parse (const guint8 *data, gsize len);

/* Returns the type's name, e.g. "AAAA", or "TYPE<n>" for others.
 * Caller frees. */
char         *sbv_dns_type_name    (guint16 type);

/* TRUE for the types Sambervise can add and edit: A, AAAA, CNAME and
 * PTR. */
gboolean      sbv_dns_type_editable (guint16 type);

/* Builds a dnsRecord value for an editable type. `value` is an address
 * for A and AAAA, or a host name for CNAME and PTR. Returns NULL and
 * sets `error` when the value isn't valid for the type. */
GBytes       *sbv_dns_record_build (guint16      type,
                                    const char  *value,
                                    guint32      ttl,
                                    guint32      serial,
                                    GError     **error);

/* Returns a placeholder for the value of a new A or AAAA record. With a
 * `sample` address from the zone, it keeps that network: "192.168.1.5"
 * gives "192.168.1.x", and "fd12:3456:789a:1::5" gives
 * "fd12:3456:789a:1::x". Without one it gives "192.168.1.10" or
 * "fd00::10". Other types give "host.example.com". Caller frees. */
char         *sbv_dns_value_hint   (guint16 type, const char *sample);

/* Reads the serial from an SOA dnsRecord value. Returns FALSE when it
 * isn't a well-formed SOA record. */
gboolean      sbv_dns_soa_serial   (GBytes *raw, guint32 *serial);

/* Returns a copy of an SOA dnsRecord value with its serial, in both the
 * header and the SOA data, set to `serial`. NULL when `raw` isn't SOA. */
GBytes       *sbv_dns_soa_with_serial (GBytes *raw, guint32 serial);

/* Checks a host name for a CNAME or PTR target or a record name: dot
 * separated labels of 1 to 63 bytes, 255 bytes in all. A trailing dot is
 * allowed. "@" is allowed only when `allow_at` is TRUE. */
gboolean      sbv_dns_name_valid   (const char *name, gboolean allow_at);

G_END_DECLS
