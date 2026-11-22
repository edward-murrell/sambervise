#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"
#include "../backend/sbv-profiles.h"

G_BEGIN_DECLS

typedef void (*SbvConnectCallback) (SbvConnection *connection,
                                    SbvProfile    *profile,  /* in the store only if named */
                                    gpointer       user_data);

/* Opens the "Add Connection" dialog.
 * If edit_profile is non-NULL the dialog is pre-populated for editing.
 * On success, calls callback with the new live connection and the profile
 * it used. The profile is saved to the store only when it has a name. */
GtkWidget *sbv_connect_dialog_new (GtkWindow         *parent,
                                    GListStore        *profiles_store,
                                    SbvProfile        *edit_profile,
                                    SbvConnectCallback callback,
                                    gpointer           user_data);

/* Variant: opens the same dialog in "Edit Connection" mode — pre-populated
 * from edit_profile, the action button just persists changes (no connect
 * attempt) and `callback` fires with conn=NULL on save, or never if the
 * user cancels. Renaming the profile is handled (the old name is removed
 * before the new entry is upserted). */
GtkWidget *sbv_edit_dialog_new    (GtkWindow         *parent,
                                    GListStore        *profiles_store,
                                    SbvProfile        *edit_profile,
                                    SbvConnectCallback callback,
                                    gpointer           user_data);

G_END_DECLS
