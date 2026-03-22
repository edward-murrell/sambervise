#pragma once

#include <gio/gio.h>
#include "../model/sbv-profile.h"

G_BEGIN_DECLS

/* Returns a GListStore<SbvProfile> loaded from ~/.config/sambervise/connections.ini.
 * Returns an empty store (not NULL) on first run. */
GListStore *sbv_profiles_load  (GError **error);

/* Persist the full store back to disk. */
gboolean    sbv_profiles_save  (GListStore *store, GError **error);

/* Convenience: append or replace a profile (matched by name) and save. */
gboolean    sbv_profiles_upsert (GListStore *store, SbvProfile *profile,
                                  GError **error);

/* Remove by name and save. Returns FALSE if name not found (not an error). */
gboolean    sbv_profiles_remove (GListStore *store, const char *name,
                                  GError **error);

G_END_DECLS
