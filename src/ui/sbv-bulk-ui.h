#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-bulk.h"

G_BEGIN_DECLS

/* Dialogs shared by the bulk actions on the Users and Computers panels. */

/* Runs when the user accepts a confirmation. */
typedef void (*SbvBulkAcceptFn) (gpointer user_data);

/* Asks "<heading>" and lists the first few of `names`, with "and N more"
 * for the rest. `accept_label` names the button, e.g. "_Disable".
 * `destructive` styles it red. `on_accept` runs only on accept. */
void sbv_bulk_ui_confirm (GtkWidget          *from,
                          const char         *heading,
                          const char         *accept_label,
                          gboolean            destructive,
                          const char * const *names,
                          SbvBulkAcceptFn     on_accept,
                          gpointer            user_data);

/* Runs when the user accepts the password prompt. `password` is only
 * valid during the call. */
typedef void (*SbvBulkPasswordFn) (const char *password,
                                   gboolean    must_change,
                                   gpointer    user_data);

/* Asks for one new password for all of `names`, typed twice, and whether
 * users must change it at next logon (on by default). */
void sbv_bulk_ui_ask_password (GtkWidget          *from,
                               const char * const *names,
                               SbvBulkPasswordFn   on_accept,
                               gpointer            user_data);

/* Reports a finished run. Full success shows a toast with `done_text`,
 * e.g. "Disabled 5 users". Any failure opens a dialog that lists each
 * failed object by name and the reason. */
void sbv_bulk_ui_report (GtkWidget     *from,
                         const char    *done_text,
                         SbvBulkResult *result);

/* Returns a display name for a list row. Caller frees. */
typedef char *(*SbvBulkRowNameFn) (GtkListBoxRow *row);

/* Returns the selected rows of `source` that its filter shows, in list
 * order. Hidden rows can stay selected, and bulk actions skip them.
 * Free with g_ptr_array_unref. */
GPtrArray *sbv_bulk_ui_visible_selection (GtkListBox *source);

/* Makes Ctrl+Click on a selected row of `source` unselect it.
 * sbv_bulk_ui_selection_new calls this for its source list. */
void sbv_bulk_ui_enable_ctrl_deselect (GtkListBox *source);

/* Builds the "Selected" box for a bulk page: a list of the selected
 * accounts, each with a button that deselects it, and a Clear Selection
 * button. Also enables Ctrl+Click deselect on `source`. Fill it with
 * sbv_bulk_ui_selection_update. */
GtkWidget *sbv_bulk_ui_selection_new (GtkListBox *source);

/* Rebuilds the box from the visible selection of its source list, using
 * `name_fn` for the labels. */
void       sbv_bulk_ui_selection_update (GtkWidget        *box,
                                         SbvBulkRowNameFn  name_fn);

/* Builds the "a, b, c and 2 more" preview used in confirmations. Shows
 * at most `max` names. Caller frees. */
char *sbv_bulk_ui_preview (const char * const *names, guint max);

G_END_DECLS
