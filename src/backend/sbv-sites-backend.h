#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-site.h"

G_BEGIN_DECLS

/* Sites and subnets (R13), under CN=Sites in the Configuration
 * partition. */

/* Everything the Sites page shows. */
typedef struct {
  char      *sites_dn;   /* CN=Sites,<configuration> */
  GPtrArray *sites;      /* SbvSite, sorted by name */
  GPtrArray *subnets;    /* SbvSubnet, sorted by address */
} SbvSitesInfo;

/* Frees the info and its lists. */
void sbv_sites_info_free (SbvSitesInfo *info);

/* Reads every site, the DCs in each, and every subnet. The finish call
 * returns a new SbvSitesInfo. Free with sbv_sites_info_free. */
void          sbv_sites_list_async     (SbvConnection       *conn,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
SbvSitesInfo *sbv_sites_list_finish    (SbvConnection  *conn,
                                        GAsyncResult   *result,
                                        GError        **error);

/* Creates a site under `sites_dn`, with the NTDS Site Settings and
 * Servers objects a DC needs to join it. `description` may be NULL. */
void sbv_site_create_async   (SbvConnection       *conn,
                              const char          *sites_dn,
                              const char          *name,
                              const char          *description,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/* Saves a changed description on the site at `dn`, then renames it when
 * `new_name` differs from `old_name`. Descriptions may be NULL. */
void sbv_site_update_async   (SbvConnection       *conn,
                              const char          *dn,
                              const char          *old_name,
                              const char          *new_name,
                              const char          *old_description,
                              const char          *new_description,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/* Deletes the site at `dn` and the objects under it. Fails without
 * changing anything when a DC still sits in the site. */
void sbv_site_delete_async   (SbvConnection       *conn,
                              const char          *dn,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/* Creates `subnet` under CN=Subnets of `sites_dn`. Its `name` must
 * already be normalised and its `dn` is ignored. */
void sbv_subnet_create_async (SbvConnection       *conn,
                              const char          *sites_dn,
                              const SbvSubnet     *subnet,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/* Saves the site and description of `updated` where they differ from
 * `original`, at `original->dn`. */
void sbv_subnet_update_async (SbvConnection       *conn,
                              const SbvSubnet     *original,
                              const SbvSubnet     *updated,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/* Deletes the subnet at `dn`. */
void sbv_subnet_delete_async (SbvConnection       *conn,
                              const char          *dn,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/* Finishes any of the site and subnet create, update and delete calls
 * above. */
gboolean sbv_sites_op_finish (SbvConnection  *conn,
                              GAsyncResult   *result,
                              GError        **error);

G_END_DECLS
