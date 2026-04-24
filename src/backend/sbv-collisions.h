#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"

G_BEGIN_DECLS

/* A single collision hit returned by the check functions. Owned by the
 * caller; freed with sbv_collision_hit_free. */
typedef struct {
  char *dn;          /* distinguishedName of the conflicting object  */
  char *sam;         /* sAMAccountName, if available                  */
} SbvCollisionHit;

/* Frees a single hit. Safe to call on NULL. */
void sbv_collision_hit_free (SbvCollisionHit *hit);

/* Frees a GPtrArray<SbvCollisionHit*> returned by the finish functions.
 * The array's free_func is set to sbv_collision_hit_free, so this is
 * just g_ptr_array_unref but exposed under a clearer name. */
void sbv_collision_hits_free (GPtrArray *hits);

/* Look up other user objects already using the supplied uidNumber.
 * `exclude_dn` is omitted from the result — typically the DN of the
 * user the caller is currently editing (NULL = no exclusion).
 * Returns a GPtrArray<SbvCollisionHit*> via finish (possibly empty).
 *
 * Filter: (&(objectClass=user)(uidNumber=<value>)). */
void       sbv_collisions_uid_check_async  (SbvConnection       *conn,
                                             gint64               uid,
                                             const char          *exclude_dn,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
GPtrArray *sbv_collisions_uid_check_finish (SbvConnection *conn,
                                             GAsyncResult  *result,
                                             GError       **error);

/* Look up other group objects already using the supplied gidNumber.
 * Filter: (&(objectClass=group)(gidNumber=<value>)). */
void       sbv_collisions_gid_check_async  (SbvConnection       *conn,
                                             gint64               gid,
                                             const char          *exclude_dn,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
GPtrArray *sbv_collisions_gid_check_finish (SbvConnection *conn,
                                             GAsyncResult  *result,
                                             GError       **error);

/* Find the lowest unused uidNumber within [range_min, range_max] (inclusive)
 * by scanning all user objects in that range. The result is delivered via
 * finish() as a gint64 inside a GTask boolean-style payload — see
 * sbv_collisions_next_free_uid_finish. Returns -1 if the entire range is
 * already taken. */
void   sbv_collisions_next_free_uid_async  (SbvConnection       *conn,
                                             gint64               range_min,
                                             gint64               range_max,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
gint64 sbv_collisions_next_free_uid_finish (SbvConnection *conn,
                                             GAsyncResult  *result,
                                             GError       **error);

/* Same shape as next_free_uid, but for gidNumber on group objects. */
void   sbv_collisions_next_free_gid_async  (SbvConnection       *conn,
                                             gint64               range_min,
                                             gint64               range_max,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
gint64 sbv_collisions_next_free_gid_finish (SbvConnection *conn,
                                             GAsyncResult  *result,
                                             GError       **error);

G_END_DECLS
