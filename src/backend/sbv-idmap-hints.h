#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-profile.h"

G_BEGIN_DECLS

/* Hints describing the POSIX UID/GID ranges to use on a given DC.
 * Fields are -1 when not known / not published. The struct merges values
 * read from the DC's well-known LDAP locations with the per-profile range
 * configured in connections.ini. */
typedef struct {
  gint64 uid_min;        /* lower bound (inclusive) for new UIDs   */
  gint64 uid_max;        /* upper bound (inclusive) for new UIDs   */
  gint64 gid_min;        /* lower bound (inclusive) for new GIDs   */
  gint64 gid_max;        /* upper bound (inclusive) for new GIDs   */
  gint64 next_uid_hint;  /* DC-published "next free UID" hint       */
  gint64 next_gid_hint;  /* DC-published "next free GID" hint       */
  char  *source;         /* human-readable origin description       */
} SbvIdmapHints;

#define SBV_IDMAP_DEFAULT_UID_MIN 10000
#define SBV_IDMAP_DEFAULT_UID_MAX 29999
#define SBV_IDMAP_DEFAULT_GID_MIN 10000
#define SBV_IDMAP_DEFAULT_GID_MAX 29999

SbvIdmapHints *sbv_idmap_hints_new            (void);
void           sbv_idmap_hints_free           (SbvIdmapHints *self);

/* Build a hints struct populated from the per-profile config only.
 * Used as a fallback when no live DC is available, or as a base that
 * the async query then enriches. Never returns NULL. */
SbvIdmapHints *sbv_idmap_hints_from_profile   (SbvProfile *profile);

/* Asynchronously query the DC's well-known idmap location:
 *   CN=ypservers,CN=ypServ30,CN=RpcServices,CN=System,<base>
 *   reading msSFU30MaxUidNumber / msSFU30MaxGidNumber when present.
 *
 * The result merges the live values with the supplied profile's
 * configured range (profile range wins for min/max; live values fill
 * the next_*_hint fields). */
void           sbv_idmap_hints_query_async    (SbvConnection       *conn,
                                                SbvProfile          *profile,
                                                GCancellable        *cancellable,
                                                GAsyncReadyCallback  callback,
                                                gpointer             user_data);
SbvIdmapHints *sbv_idmap_hints_query_finish   (SbvConnection *conn,
                                                GAsyncResult  *result,
                                                GError       **error);

G_END_DECLS
