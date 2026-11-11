#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

/* Invoked once the object has been moved, with its new DN. In pick mode
 * it gets the chosen container instead. `new_dn` is borrowed. */
typedef void (*SbvMoveCallback) (SbvConnection *conn,
                                 const char    *new_dn,
                                 gpointer       user_data);

/* Show a modal "Move" dialog for the object at `dn`. It lists the domain
 * root and every visible OU and container. The object's current container,
 * and the object itself and its descendants when it is an OU, can't be
 * picked. `label` is the name shown in the prompt, e.g. "alice". On
 * success the dialog closes and `callback` runs. */
void sbv_move_dialog_show (GtkWindow       *parent,
                           SbvConnection   *conn,
                           const char      *dn,
                           const char      *label,
                           SbvMoveCallback  callback,
                           gpointer         user_data);

/* Shows the same dialog in pick mode for moving several objects. Nothing
 * is greyed out. When the user picks a container the dialog closes and
 * `callback` runs with that container's DN as `new_dn`. The caller does
 * the move. `label` describes the objects, e.g. "5 users". */
void sbv_move_dialog_pick (GtkWindow       *parent,
                           SbvConnection   *conn,
                           const char      *label,
                           SbvMoveCallback  callback,
                           gpointer         user_data);

G_END_DECLS
