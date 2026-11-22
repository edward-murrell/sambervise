#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Sites and subnets (R13). These live under CN=Sites in the
 * Configuration partition. Plain structs and pure helpers, with no I/O,
 * so they are unit-testable. */

/* A site and the DCs whose server objects sit in it. */
typedef struct {
  char  *dn;
  char  *name;          /* cn */
  char  *description;   /* or NULL */
  char **servers;       /* each DC's dNSHostName, or its name; never NULL */
} SbvSite;

/* A subnet and the site it maps to. */
typedef struct {
  char *dn;
  char *name;           /* the CIDR, e.g. "10.0.0.0/24" */
  char *description;    /* or NULL */
  char *site_dn;        /* siteObject, or NULL when unassigned */
} SbvSubnet;

/* Copy and free helpers. */
void       sbv_site_free          (SbvSite *site);
SbvSubnet *sbv_subnet_copy        (const SbvSubnet *subnet);
void       sbv_subnet_free        (SbvSubnet *subnet);

/* Orders sites by name. For g_ptr_array_sort on SbvSite pointers. */
gint       sbv_site_compare       (gconstpointer a, gconstpointer b);

/* Orders subnets IPv4 first, then by address and prefix length. For
 * g_ptr_array_sort on SbvSubnet pointers. */
gint       sbv_subnet_compare     (gconstpointer a, gconstpointer b);

/* Checks a new site name. Returns NULL when it is fine, otherwise a
 * message for the user. Caller frees. */
char      *sbv_site_name_check    (const char *name);

/* Turns user text such as " 10.1.0.0/16 " into the form the DC stores.
 * Returns the subnet, or NULL with `*error` set to a message for the
 * user. Caller frees the result or the message. */
char      *sbv_subnet_normalise   (const char *text, char **error);

/* Returns the site DN a server object sits in, i.e. the grandparent of
 * "CN=DC1,CN=Servers,CN=Site,...". NULL when the DN is too short.
 * Caller frees. */
char      *sbv_site_dn_for_server (const char *server_dn);

G_END_DECLS
