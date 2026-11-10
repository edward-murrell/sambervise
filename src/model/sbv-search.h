#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Folds search text for matching: trims spaces and lower-cases it.
 * Returns NULL when nothing is left, which means "match everything".
 * Caller frees. */
char     *sbv_search_fold    (const char *text);

/* Returns TRUE when needle is NULL, or when any of the n_fields fields
 * contains it, ignoring case. NULL fields are skipped. needle must come
 * from sbv_search_fold. */
gboolean  sbv_search_matches (const char        *needle,
                              const char *const *fields,
                              gsize              n_fields);

G_END_DECLS
