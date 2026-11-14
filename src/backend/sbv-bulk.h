#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-user.h"

G_BEGIN_DECLS

/* Operations that touch many objects in one run. Each run holds the
 * connection lock for its whole length and sends one LDAP request per
 * object, so a failure on one object doesn't stop the rest. */

typedef enum {
  SBV_BULK_ENABLE,          /* clear ACCOUNTDISABLE (users or computers) */
  SBV_BULK_DISABLE,         /* set ACCOUNTDISABLE (users or computers) */
  SBV_BULK_RESET_PASSWORD,  /* users only; needs a sealed connection */
  SBV_BULK_MOVE,            /* any object; keeps its RDN */
  SBV_BULK_ADD_TO_GROUP,    /* any object; adds it to one group's member */
  SBV_BULK_DELETE,          /* any leaf object; skips critical system
                             * objects; can't be undone */
} SbvBulkAction;

/* One object that failed, with a message fit to show the user. */
typedef struct {
  char *dn;
  char *message;
} SbvBulkFailure;

/* Outcome of a run. `failures` holds SbvBulkFailure pointers. */
typedef struct {
  guint      total;
  guint      succeeded;
  GPtrArray *failures;
} SbvBulkResult;

void sbv_bulk_result_free (SbvBulkResult *result);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SbvBulkResult, sbv_bulk_result_free)

/* Runs `action` on each DN in `dns`.
 *   SBV_BULK_RESET_PASSWORD: `arg` is the new password. When
 *     `must_change` is TRUE the user must change it at next logon.
 *   SBV_BULK_MOVE: `arg` is the destination container DN. Objects
 *     already there count as succeeded.
 *   SBV_BULK_ADD_TO_GROUP: `arg` is the group DN. Objects already in the
 *     group count as succeeded.
 * The finish call fails only when the run couldn't start, such as when
 * the connection has dropped. Per-object errors land in the result. */
void           sbv_bulk_run_async  (SbvConnection       *conn,
                                    SbvBulkAction        action,
                                    const char * const  *dns,
                                    const char          *arg,
                                    gboolean             must_change,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);
SbvBulkResult *sbv_bulk_run_finish (SbvConnection *conn,
                                    GAsyncResult  *result,
                                    GError       **error);

/* Copies settings from `template_user` onto the new user at `new_dn`:
 * account options (see sbv_user_template_uac), account expiry, the
 * Windows profile paths with the username swapped (see
 * sbv_user_template_path) and every group in its memberOf.
 * `total` counts the settings step plus one per group. */
void           sbv_bulk_apply_user_template_async  (SbvConnection       *conn,
                                                    const char          *new_dn,
                                                    const char          *new_sam,
                                                    SbvUser             *template_user,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
SbvBulkResult *sbv_bulk_apply_user_template_finish (SbvConnection *conn,
                                                    GAsyncResult  *result,
                                                    GError       **error);

G_END_DECLS
