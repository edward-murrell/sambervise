#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Pure string helpers for LDAP distinguished names (RFC 4514). No LDAP
 * handle or I/O, so they are safe on any thread and unit-testable. DN
 * comparisons are ASCII case-insensitive, which matches AD attribute-type
 * and value matching for the names Sambervise handles. */

/* Escapes a raw attribute value for use inside an RDN, e.g. "Smith, Bob"
 * becomes "Smith\, Bob". Caller frees. */
char     *sbv_dn_escape_value     (const char *value);

/* Builds "<type>=<escaped value>,<parent>", e.g. ("CN", "Smith, Bob",
 * "OU=Staff,DC=example,DC=com") gives "CN=Smith\, Bob,OU=Staff,...".
 * A NULL or empty `parent` gives the RDN alone. Caller frees. */
char     *sbv_dn_build            (const char *type,
                                   const char *value,
                                   const char *parent);

/* Splits `dn` at its first unescaped comma. `rdn` gets the leading RDN
 * exactly as written (still escaped) and `parent` gets the rest. Either out
 * pointer may be NULL. Returns FALSE (and sets outs to NULL) when the DN has
 * no parent, e.g. a bare "DC=com". Caller frees outs. */
gboolean  sbv_dn_split            (const char *dn,
                                   char      **rdn,
                                   char      **parent);

/* Returns the unescaped value of the leading RDN, e.g. "Smith, Bob" for
 * "CN=Smith\, Bob,OU=Staff,DC=example,DC=com". Returns NULL if `dn` has no
 * "=". Caller frees. */
char     *sbv_dn_rdn_value        (const char *dn);

/* TRUE when `dn` equals `ancestor` or sits anywhere beneath it. */
gboolean  sbv_dn_is_within        (const char *dn,
                                   const char *ancestor);

/* Case-insensitive DN equality. NULL equals only NULL. */
gboolean  sbv_dn_equal            (const char *a,
                                   const char *b);

/* Turns a DC-only DN into a DNS-style domain name, e.g.
 * "DC=example,DC=com" → "example.com". Falls back to a copy of `dn` when
 * any component is not a DC. Caller frees. */
char     *sbv_dn_domain_name      (const char *dn);

/* Returns the unescaped RDN values between `base` and `dn`, ordered from the
 * top down. "OU=Sydney,OU=Sales,DC=example,DC=com" under
 * "DC=example,DC=com" gives { "Sales", "Sydney", NULL }. Returns an empty
 * vector when `dn` equals `base`, and NULL when `dn` is not within `base`.
 * Caller frees with g_strfreev. */
char    **sbv_dn_path_components  (const char *dn,
                                   const char *base);

G_END_DECLS
