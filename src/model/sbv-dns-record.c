#include "sbv-dns-record.h"
#include "../sbv-error.h"

#include <arpa/inet.h>
#include <string.h>

/* Layout of the fixed header before the record data (MS-DNSP 2.3.2.2).
 * Everything is little-endian except the TTL, which is big-endian. */
#define HDR_LEN 24

/* Reads a little-endian 16-bit value. */
static guint16
le16 (const guint8 *p)
{
  return (guint16) (p[0] | (p[1] << 8));
}

/* Reads a little-endian 32-bit value. */
static guint32
le32 (const guint8 *p)
{
  return (guint32) p[0] | ((guint32) p[1] << 8)
       | ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}

/* Reads a big-endian 16-bit value. */
static guint16
be16 (const guint8 *p)
{
  return (guint16) ((p[0] << 8) | p[1]);
}

/* Reads a big-endian 32-bit value. */
static guint32
be32 (const guint8 *p)
{
  return ((guint32) p[0] << 24) | ((guint32) p[1] << 16)
       | ((guint32) p[2] << 8)  |  (guint32) p[3];
}

/* Writes a big-endian 32-bit value. */
static void
put_be32 (guint8 *p, guint32 v)
{
  p[0] = (guint8) (v >> 24);
  p[1] = (guint8) (v >> 16);
  p[2] = (guint8) (v >> 8);
  p[3] = (guint8) v;
}

/* Writes a little-endian 32-bit value. */
static void
put_le32 (guint8 *p, guint32 v)
{
  p[0] = (guint8) v;
  p[1] = (guint8) (v >> 8);
  p[2] = (guint8) (v >> 16);
  p[3] = (guint8) (v >> 24);
}

/* Decodes a DNS_COUNT_NAME at `*pos`: length, label count, then
 * length-prefixed labels and a zero byte. Advances `*pos`. Returns the
 * dotted name, or NULL when malformed. Caller frees. */
static char *
read_count_name (const guint8 *d, gsize len, gsize *pos)
{
  if (*pos + 2 > len) return NULL;
  guint8 count = d[*pos + 1];
  gsize  p     = *pos + 2;

  GString *out = g_string_new (NULL);
  for (guint i = 0; i < count; i++) {
    if (p >= len) goto bad;
    guint8 l = d[p++];
    if (p + l > len) goto bad;
    if (out->len) g_string_append_c (out, '.');
    g_string_append_len (out, (const char *) d + p, l);
    p += l;
  }
  if (p >= len || d[p] != 0) goto bad;
  *pos = p + 1;
  return g_string_free (out, FALSE);

bad:
  g_string_free (out, TRUE);
  return NULL;
}

/* Appends `name` as a DNS_COUNT_NAME. The name must have passed
 * sbv_dns_name_valid. */
static void
append_count_name (GByteArray *buf, const char *name)
{
  char  *trimmed = g_strdup (name);
  gsize  n       = strlen (trimmed);
  if (n && trimmed[n - 1] == '.') trimmed[--n] = '\0';

  char  **labels = g_strsplit (trimmed, ".", -1);
  guint8  count  = (guint8) g_strv_length (labels);
  guint8  total  = (guint8) (n + 2);   /* labels + length bytes + final zero */
  g_byte_array_append (buf, &total, 1);
  g_byte_array_append (buf, &count, 1);
  for (char **l = labels; *l; l++) {
    guint8 ll = (guint8) strlen (*l);
    g_byte_array_append (buf, &ll, 1);
    g_byte_array_append (buf, (const guint8 *) *l, ll);
  }
  guint8 zero = 0;
  g_byte_array_append (buf, &zero, 1);
  g_strfreev (labels);
  g_free (trimmed);
}

/* Formats data the decoder doesn't understand as hex. Caller frees. */
static char *
hex_string (const guint8 *d, gsize len)
{
  GString *s = g_string_new (NULL);
  for (gsize i = 0; i < len; i++) g_string_append_printf (s, "%02x", d[i]);
  return g_string_free (s, FALSE);
}

/* Turns the data part of a record into display text. Returns NULL when
 * it is malformed. Caller frees. */
static char *
data_text (guint16 type, const guint8 *d, gsize len)
{
  gsize pos = 0;
  switch (type) {
    case SBV_DNS_TYPE_A: {
      if (len != 4) return NULL;
      char buf[INET_ADDRSTRLEN];
      return inet_ntop (AF_INET, d, buf, sizeof buf) ? g_strdup (buf) : NULL;
    }
    case SBV_DNS_TYPE_AAAA: {
      if (len != 16) return NULL;
      char buf[INET6_ADDRSTRLEN];
      return inet_ntop (AF_INET6, d, buf, sizeof buf) ? g_strdup (buf) : NULL;
    }
    case SBV_DNS_TYPE_NS:
    case SBV_DNS_TYPE_CNAME:
    case SBV_DNS_TYPE_PTR:
      return read_count_name (d, len, &pos);
    case SBV_DNS_TYPE_MX: {
      if (len < 2) return NULL;
      pos = 2;
      char *host = read_count_name (d, len, &pos);
      if (!host) return NULL;
      char *s = g_strdup_printf ("%u %s", be16 (d), host);
      g_free (host);
      return s;
    }
    case SBV_DNS_TYPE_SRV: {
      if (len < 6) return NULL;
      pos = 6;
      char *host = read_count_name (d, len, &pos);
      if (!host) return NULL;
      char *s = g_strdup_printf ("%u %u %u %s", be16 (d), be16 (d + 2),
                                 be16 (d + 4), host);
      g_free (host);
      return s;
    }
    case SBV_DNS_TYPE_SOA: {
      if (len < 20) return NULL;
      pos = 20;
      char *mname = read_count_name (d, len, &pos);
      char *rname = mname ? read_count_name (d, len, &pos) : NULL;
      char *s = rname
        ? g_strdup_printf ("%s %s serial %u refresh %u retry %u expire %u minimum %u",
                           mname, rname, be32 (d), be32 (d + 4), be32 (d + 8),
                           be32 (d + 12), be32 (d + 16))
        : NULL;
      g_free (mname);
      g_free (rname);
      return s;
    }
    case SBV_DNS_TYPE_TXT: {
      GString *s = g_string_new (NULL);
      while (pos < len) {
        guint8 l = d[pos++];
        if (pos + l > len) {
          g_string_free (s, TRUE);
          return NULL;
        }
        if (s->len) g_string_append_c (s, ' ');
        g_string_append_c (s, '"');
        g_string_append_len (s, (const char *) d + pos, l);
        g_string_append_c (s, '"');
        pos += l;
      }
      return g_string_free (s, FALSE);
    }
    case SBV_DNS_TYPE_TOMBSTONE:
      return g_strdup ("(deleted)");
    default:
      return hex_string (d, len);
  }
}

void
sbv_dns_record_free (SbvDnsRecord *record)
{
  if (!record) return;
  g_free (record->value);
  g_clear_pointer (&record->raw, g_bytes_unref);
  g_free (record);
}

SbvDnsRecord *
sbv_dns_record_parse (const guint8 *data, gsize len)
{
  if (!data || len < HDR_LEN) return NULL;

  gsize data_len = le16 (data);
  if (HDR_LEN + data_len > len) return NULL;

  SbvDnsRecord *r = g_new0 (SbvDnsRecord, 1);
  r->type      = le16 (data + 2);
  r->rank      = data[5];
  r->serial    = le32 (data + 8);
  r->ttl       = be32 (data + 12);
  r->timestamp = le32 (data + 20);
  r->value     = data_text (r->type, data + HDR_LEN, data_len);
  if (!r->value) {
    sbv_dns_record_free (r);
    return NULL;
  }
  r->raw = g_bytes_new (data, len);
  return r;
}

char *
sbv_dns_type_name (guint16 type)
{
  switch (type) {
    case SBV_DNS_TYPE_A:     return g_strdup ("A");
    case SBV_DNS_TYPE_NS:    return g_strdup ("NS");
    case SBV_DNS_TYPE_CNAME: return g_strdup ("CNAME");
    case SBV_DNS_TYPE_SOA:   return g_strdup ("SOA");
    case SBV_DNS_TYPE_PTR:   return g_strdup ("PTR");
    case SBV_DNS_TYPE_MX:    return g_strdup ("MX");
    case SBV_DNS_TYPE_TXT:   return g_strdup ("TXT");
    case SBV_DNS_TYPE_AAAA:  return g_strdup ("AAAA");
    case SBV_DNS_TYPE_SRV:   return g_strdup ("SRV");
    default:                 return g_strdup_printf ("TYPE%u", type);
  }
}

gboolean
sbv_dns_type_editable (guint16 type)
{
  return type == SBV_DNS_TYPE_A || type == SBV_DNS_TYPE_AAAA
      || type == SBV_DNS_TYPE_CNAME || type == SBV_DNS_TYPE_PTR;
}

gboolean
sbv_dns_name_valid (const char *name, gboolean allow_at)
{
  if (!name || !*name) return FALSE;
  if (strcmp (name, "@") == 0) return allow_at;

  gsize n = strlen (name);
  if (name[n - 1] == '.') n--;
  if (n == 0 || n > 253) return FALSE;

  gsize label = 0;
  for (gsize i = 0; i < n; i++) {
    char c = name[i];
    if (c == '.') {
      if (label == 0) return FALSE;
      label = 0;
      continue;
    }
    if ((guchar) c <= ' ' || c == '\\' || c == '"' || c == '@') return FALSE;
    if (++label > 63) return FALSE;
  }
  return label > 0;
}

GBytes *
sbv_dns_record_build (guint16 type, const char *value, guint32 ttl,
                      guint32 serial, GError **error)
{
  GByteArray *buf = g_byte_array_sized_new (64);
  guint8      hdr[HDR_LEN] = { 0 };
  g_byte_array_append (buf, hdr, HDR_LEN);

  const char *v = value ? value : "";
  while (*v == ' ') v++;

  switch (type) {
    case SBV_DNS_TYPE_A: {
      guint8 a[4];
      if (inet_pton (AF_INET, v, a) != 1) goto bad_value;
      g_byte_array_append (buf, a, 4);
      break;
    }
    case SBV_DNS_TYPE_AAAA: {
      guint8 a[16];
      if (inet_pton (AF_INET6, v, a) != 1) goto bad_value;
      g_byte_array_append (buf, a, 16);
      break;
    }
    case SBV_DNS_TYPE_CNAME:
    case SBV_DNS_TYPE_PTR:
      if (!sbv_dns_name_valid (v, FALSE)) goto bad_value;
      append_count_name (buf, v);
      break;
    default:
      g_byte_array_free (buf, TRUE);
      g_set_error (error, SBV_ERROR, SBV_ERROR_LDAP,
                   "Sambervise can't edit this record type yet.");
      return NULL;
  }

  guint8 *h        = buf->data;
  guint16 data_len = (guint16) (buf->len - HDR_LEN);
  h[0] = (guint8) data_len;
  h[1] = (guint8) (data_len >> 8);
  h[2] = (guint8) type;
  h[3] = (guint8) (type >> 8);
  h[4] = 5;                        /* version */
  h[5] = SBV_DNS_RANK_ZONE;
  put_le32 (h + 8, serial);
  put_be32 (h + 12, ttl);
  return g_byte_array_free_to_bytes (buf);

bad_value:
  g_byte_array_free (buf, TRUE);
  g_set_error (error, SBV_ERROR, SBV_ERROR_LDAP,
               type == SBV_DNS_TYPE_A    ? "Enter an IPv4 address, such as 192.168.1.10."
             : type == SBV_DNS_TYPE_AAAA ? "Enter an IPv6 address, such as fd00::10."
                                         : "Enter a host name, such as host.example.com.");
  return NULL;
}

char *
sbv_dns_value_hint (guint16 type, const char *sample)
{
  if (type == SBV_DNS_TYPE_A) {
    guint8 a[4];
    if (sample && inet_pton (AF_INET, sample, a) == 1)
      return g_strdup_printf ("%u.%u.%u.x", a[0], a[1], a[2]);
    return g_strdup ("192.168.1.10");
  }
  if (type == SBV_DNS_TYPE_AAAA) {
    guint8 a[16];
    if (sample && inet_pton (AF_INET6, sample, a) == 1)
      return g_strdup_printf ("%x:%x:%x:%x::x",
                              (a[0] << 8) | a[1], (a[2] << 8) | a[3],
                              (a[4] << 8) | a[5], (a[6] << 8) | a[7]);
    return g_strdup ("fd00::10");
  }
  return g_strdup ("host.example.com");
}

gboolean
sbv_dns_soa_serial (GBytes *raw, guint32 *serial)
{
  gsize         len = 0;
  const guint8 *d   = raw ? g_bytes_get_data (raw, &len) : NULL;
  if (!d || len < HDR_LEN + 20 || le16 (d + 2) != SBV_DNS_TYPE_SOA) return FALSE;
  *serial = be32 (d + HDR_LEN);
  return TRUE;
}

GBytes *
sbv_dns_soa_with_serial (GBytes *raw, guint32 serial)
{
  guint32 old;
  if (!sbv_dns_soa_serial (raw, &old)) return NULL;

  gsize         len = 0;
  const guint8 *src = g_bytes_get_data (raw, &len);
  guint8       *d   = g_memdup2 (src, len);
  put_le32 (d + 8, serial);
  put_be32 (d + HDR_LEN, serial);
  return g_bytes_new_take (d, len);
}
