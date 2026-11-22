#include "sbv-site.h"
#include "../backend/sbv-dn.h"

#include <gio/gio.h>
#include <string.h>

void
sbv_site_free (SbvSite *site)
{
  if (!site) return;
  g_free (site->dn);
  g_free (site->name);
  g_free (site->description);
  g_strfreev (site->servers);
  g_free (site);
}

SbvSubnet *
sbv_subnet_copy (const SbvSubnet *subnet)
{
  SbvSubnet *c   = g_new0 (SbvSubnet, 1);
  c->dn          = g_strdup (subnet->dn);
  c->name        = g_strdup (subnet->name);
  c->description = g_strdup (subnet->description);
  c->site_dn     = g_strdup (subnet->site_dn);
  return c;
}

void
sbv_subnet_free (SbvSubnet *subnet)
{
  if (!subnet) return;
  g_free (subnet->dn);
  g_free (subnet->name);
  g_free (subnet->description);
  g_free (subnet->site_dn);
  g_free (subnet);
}

gint
sbv_site_compare (gconstpointer a, gconstpointer b)
{
  const SbvSite *x = *(SbvSite * const *) a;
  const SbvSite *y = *(SbvSite * const *) b;
  return g_ascii_strcasecmp (x->name ? x->name : "", y->name ? y->name : "");
}

/* Parses "address/length" into its address bytes, family size and
 * length. Returns FALSE when it doesn't parse. */
static gboolean
parse_cidr (const char *name, guint8 bytes[16], gsize *size, guint *length)
{
  GInetAddressMask *mask = name ? g_inet_address_mask_new_from_string (name, NULL) : NULL;
  if (!mask) return FALSE;
  GInetAddress *addr = g_inet_address_mask_get_address (mask);
  *size   = g_inet_address_get_native_size (addr);
  *length = g_inet_address_mask_get_length (mask);
  memcpy (bytes, g_inet_address_to_bytes (addr), *size);
  g_object_unref (mask);
  return TRUE;
}

gint
sbv_subnet_compare (gconstpointer a, gconstpointer b)
{
  const SbvSubnet *x = *(SbvSubnet * const *) a;
  const SbvSubnet *y = *(SbvSubnet * const *) b;
  guint8 xb[16], yb[16];
  gsize  xs, ys;
  guint  xl, yl;
  gboolean xok = parse_cidr (x->name, xb, &xs, &xl);
  gboolean yok = parse_cidr (y->name, yb, &ys, &yl);

  if (!xok || !yok) {
    if (xok != yok) return xok ? -1 : 1;
    return g_strcmp0 (x->name, y->name);
  }
  if (xs != ys) return xs < ys ? -1 : 1;
  int c = memcmp (xb, yb, xs);
  if (c != 0) return c;
  return (gint) xl - (gint) yl;
}

char *
sbv_site_name_check (const char *name)
{
  if (!name || !*name)
    return g_strdup ("Enter a site name.");
  if (strlen (name) > 63)
    return g_strdup ("Site names can be at most 63 characters.");
  for (const char *p = name; *p; p++) {
    if (!g_ascii_isalnum (*p) && *p != '-')
      return g_strdup ("Use only letters, digits and hyphens. "
                       "The site name becomes part of DNS names.");
  }
  if (name[0] == '-' || name[strlen (name) - 1] == '-')
    return g_strdup ("Site names can't start or end with a hyphen.");
  return NULL;
}

/* Says whether the address before `slash` and the length after it are
 * each valid, so a mask that still fails to parse has host bits set. */
static gboolean
address_and_length_valid (const char *text, const char *slash)
{
  char         *addr_text = g_strndup (text, slash - text);
  GInetAddress *addr      = g_inet_address_new_from_string (addr_text);
  g_free (addr_text);
  if (!addr) return FALSE;

  guint   max = g_inet_address_get_native_size (addr) * 8;
  char   *end = NULL;
  guint64 len = g_ascii_strtoull (slash + 1, &end, 10);
  g_object_unref (addr);
  return end && *end == '\0' && end != slash + 1 && len <= max;
}

char *
sbv_subnet_normalise (const char *text, char **error)
{
  char *t = g_strstrip (g_strdup (text ? text : ""));
  char *out = NULL;
  *error = NULL;

  const char *slash = strchr (t, '/');
  if (!*t) {
    *error = g_strdup ("Enter a subnet, such as 10.1.0.0/16.");
    goto done;
  }
  if (!slash || !slash[1]) {
    *error = g_strdup ("Add the prefix length, such as /24.");
    goto done;
  }

  /* Leading zeros in an IPv4 octet are ambiguous, and the DC rejects them. */
  if (!strchr (t, ':')) {
    for (const char *p = t; p < slash; p++) {
      gboolean start = p == t || p[-1] == '.';
      if (start && p[0] == '0' && g_ascii_isdigit (p[1])) {
        *error = g_strdup ("Remove the leading zeros from the address.");
        goto done;
      }
    }
  }

  GError           *gerr = NULL;
  GInetAddressMask *mask = g_inet_address_mask_new_from_string (t, &gerr);
  if (!mask) {
    g_clear_error (&gerr);
    if (address_and_length_valid (t, slash))
      *error = g_strdup ("The address has bits set past the prefix length. "
                         "Use the network address, such as 10.1.0.0/16.");
    else
      *error = g_strdup_printf ("\"%s\" isn't a valid subnet. "
                                "Use the form 10.1.0.0/16 or 2001:db8::/32.", t);
    goto done;
  }

  if (g_inet_address_mask_get_length (mask) == 0) {
    *error = g_strdup ("The prefix length must be at least 1.");
  } else {
    /* to_string drops a full-width length, but the DC needs it. */
    char *addr = g_inet_address_to_string (g_inet_address_mask_get_address (mask));
    out = g_strdup_printf ("%s/%u", addr, g_inet_address_mask_get_length (mask));
    g_free (addr);
  }
  g_object_unref (mask);

done:
  g_free (t);
  return out;
}

char *
sbv_site_dn_for_server (const char *server_dn)
{
  char *servers = NULL;
  char *site    = NULL;
  if (!sbv_dn_split (server_dn, NULL, &servers)) return NULL;
  sbv_dn_split (servers, NULL, &site);
  g_free (servers);
  return site;
}
