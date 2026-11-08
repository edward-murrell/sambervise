#pragma once

#include <gtk/gtk.h>
#include "../model/sbv-ou.h"

G_BEGIN_DECLS

/* Builds a list row for one container, indented by its depth below the
 * domain root. The row holds a ref to `ou`. Used by the OUs panel and the
 * move dialog so both show the tree the same way. */
GtkWidget *sbv_ou_row_new    (SbvOu *ou);

/* Returns the SbvOu stored on a row built by sbv_ou_row_new (borrowed),
 * or NULL. */
SbvOu     *sbv_ou_row_get_ou (GtkListBoxRow *row);

G_END_DECLS
