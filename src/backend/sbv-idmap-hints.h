#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-profile.h"
#include "../model/sbv-idmap-record.h"

G_BEGIN_DECLS

/* What the lookup found of hostadmin's ID mapping record. */
typedef enum {
  SBV_IDMAP_RECORD_NONE,        /* not looked up, or the domain has none */
  SBV_IDMAP_RECORD_VALID,
  SBV_IDMAP_RECORD_INVALID,     /* present, but the spec calls it invalid */
  SBV_IDMAP_RECORD_UNREADABLE,  /* the read itself failed */
} SbvIdmapRecordState;

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
  gboolean uid_range_set;  /* the profile set uid-min or uid-max     */
  gboolean gid_range_set;  /* the profile set gid-min or gid-max     */
  SbvIdmapRecordState record_state;
  SbvIdmapRecord *record;  /* owned; set when record_state is VALID  */
  char  *record_error;     /* owned; why it is INVALID or UNREADABLE */
} SbvIdmapHints;

#define SBV_IDMAP_DEFAULT_UID_MIN 10000
#define SBV_IDMAP_DEFAULT_UID_MAX 29999
#define SBV_IDMAP_DEFAULT_GID_MIN 10000
#define SBV_IDMAP_DEFAULT_GID_MAX 29999

SbvIdmapHints *sbv_idmap_hints_new            (void);
void           sbv_idmap_hints_free           (SbvIdmapHints *self);

/* Deep-copies hints. NULL gives NULL. */
SbvIdmapHints *sbv_idmap_hints_copy           (const SbvIdmapHints *self);

/* Build a hints struct populated from the per-profile config only.
 * Used as a fallback when no live DC is available, or as a base that
 * the async query then enriches. Never returns NULL. */
SbvIdmapHints *sbv_idmap_hints_from_profile   (SbvProfile *profile);

/* Asynchronously query the DC's well-known idmap location (see
 * sbv_idmap_hints_read_sfu30) for msSFU30MaxUidNumber and
 * msSFU30MaxGidNumber.
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

/* Reads everything the DC says about Unix IDs into `hints`, which
 * sbv_idmap_hints_from_profile made: hostadmin's ID mapping record at
 * CN=hostadmin-idmap,CN=System,<base>, whose range applies where the
 * profile set none, and the next-UID and next-GID values that ADUC's UNIX
 * Attributes tab keeps on CN=<NIS domain>,CN=ypservers,CN=ypServ30,
 * CN=RpcServices,CN=System,<base>. Then rewrites `hints->source`. Missing
 * objects are normal. `ld` is a bound handle, which the caller holds the
 * lock for. */
void           sbv_idmap_hints_read_dc        (struct ldap    *ld,
                                                const char     *base_dn,
                                                int             timeout_s,
                                                SbvIdmapHints  *hints);

/* Describes the record state in one line for the user, such as
 * "rid, 10000–999999" or "Invalid: the backend is missing.". Returns
 * NULL when the domain has no record. Caller frees. */
char          *sbv_idmap_hints_describe_record (const SbvIdmapHints *hints);

/* For a domain whose hostadmin record says `rid`, explains that hosts
 * ignore uidNumber and gidNumber, and gives the IDs hosts use. `rid` is
 * the account's own RID. `primary_rid` is a user's primary group RID,
 * and is ignored for a group. Either may be -1 when unknown. Returns NULL
 * for any other domain. Caller frees. */
char          *sbv_idmap_hints_rid_note       (const SbvIdmapHints *hints,
                                                gboolean             is_user,
                                                gint64               rid,
                                                gint64               primary_rid);

G_END_DECLS
