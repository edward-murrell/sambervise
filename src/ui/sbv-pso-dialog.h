#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"
#include "../model/sbv-domain.h"

G_BEGIN_DECLS

/* Runs after a policy is created, saved or deleted. `message` suits a
 * toast, e.g. "Policy saved". */
typedef void (*SbvPsoDoneFn) (const char *message, gpointer user_data);

/* Opens the dialog that creates or edits a fine-grained password policy.
 * A `pso` with no DN is created on Save. Otherwise Save updates it, and a
 * Delete button removes it. `pso` is copied. `done` runs after a change
 * succeeds, and `user_data_free`, if set, frees `user_data` when the
 * dialog goes. */
void sbv_pso_dialog_show (GtkWindow      *parent,
                          SbvConnection  *conn,
                          const SbvPso   *pso,
                          SbvPsoDoneFn    done,
                          gpointer        user_data,
                          GDestroyNotify  user_data_free);

G_END_DECLS
