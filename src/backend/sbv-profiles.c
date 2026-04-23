#include "sbv-profiles.h"

static char *
get_config_path (void)
{
  return g_build_filename (g_get_user_config_dir (),
                            "sambervise", "connections.ini", NULL);
}

/* ── Load ───────────────────────────────────────────────────────────────── */

GListStore *
sbv_profiles_load (GError **error)
{
  GListStore *store = g_list_store_new (SBV_TYPE_PROFILE);
  char       *path  = get_config_path ();
  GKeyFile   *kf    = g_key_file_new ();

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL)) {
    /* First run or missing file — return empty store */
    g_key_file_free (kf);
    g_free (path);
    return store;
  }

  char **groups = g_key_file_get_groups (kf, NULL);
  for (int i = 0; groups && groups[i]; i++) {
    const char *name = groups[i];
    SbvProfile *p    = sbv_profile_new ();

    sbv_profile_set_name (p, name);

    char *host = g_key_file_get_string (kf, name, "host", NULL);
    if (host) sbv_profile_set_host (p, host);
    g_free (host);

    GError *err = NULL;
    int port = g_key_file_get_integer (kf, name, "port", &err);
    if (!err)
      sbv_profile_set_port (p, port);
    else
      g_clear_error (&err);

    char *base_dn = g_key_file_get_string (kf, name, "base-dn", NULL);
    if (base_dn) sbv_profile_set_base_dn (p, base_dn);
    g_free (base_dn);

    char *auth = g_key_file_get_string (kf, name, "auth-type", NULL);
    if (auth && g_str_equal (auth, "simple"))
      sbv_profile_set_auth_type (p, SBV_AUTH_SIMPLE);
    else
      sbv_profile_set_auth_type (p, SBV_AUTH_KERBEROS);
    g_free (auth);

    char *bind_dn = g_key_file_get_string (kf, name, "bind-dn", NULL);
    if (bind_dn) sbv_profile_set_bind_dn (p, bind_dn);
    g_free (bind_dn);

    sbv_profile_set_use_tls   (p, g_key_file_get_boolean (kf, name, "use-tls",   NULL));
    sbv_profile_set_use_ldaps (p, g_key_file_get_boolean (kf, name, "use-ldaps", NULL));
    sbv_profile_set_skip_cert (p, g_key_file_get_boolean (kf, name, "skip-cert", NULL));

    /* POSIX ID range (any missing key leaves the field at -1 = unset). */
    {
      static const struct { const char *key; void (*setter)(SbvProfile *, gint64); } id_keys[] = {
        { "uid-min", sbv_profile_set_uid_min },
        { "uid-max", sbv_profile_set_uid_max },
        { "gid-min", sbv_profile_set_gid_min },
        { "gid-max", sbv_profile_set_gid_max },
      };
      for (size_t k = 0; k < G_N_ELEMENTS (id_keys); k++) {
        GError *e = NULL;
        gint64 v = g_key_file_get_int64 (kf, name, id_keys[k].key, &e);
        if (!e) id_keys[k].setter (p, v);
        else    g_clear_error (&e);
      }
    }

    g_list_store_append (store, p);
    g_object_unref (p);
  }

  g_strfreev (groups);
  g_key_file_free (kf);
  g_free (path);

  (void) error; /* load errors are non-fatal */
  return store;
}

/* ── Save ───────────────────────────────────────────────────────────────── */

gboolean
sbv_profiles_save (GListStore *store, GError **error)
{
  GKeyFile *kf   = g_key_file_new ();
  guint     n    = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint i = 0; i < n; i++) {
    SbvProfile *p    = g_list_model_get_item (G_LIST_MODEL (store), i);
    const char *name = sbv_profile_get_name (p);
    if (!name || !*name) {
      g_object_unref (p);
      continue;
    }

    g_key_file_set_string  (kf, name, "host",      sbv_profile_get_host (p) ?: "");
    g_key_file_set_integer (kf, name, "port",      sbv_profile_get_port (p));
    g_key_file_set_string  (kf, name, "base-dn",   sbv_profile_get_base_dn (p) ?: "");
    g_key_file_set_string  (kf, name, "auth-type",
      sbv_profile_get_auth_type (p) == SBV_AUTH_SIMPLE ? "simple" : "kerberos");
    g_key_file_set_string  (kf, name, "bind-dn",   sbv_profile_get_bind_dn (p) ?: "");
    g_key_file_set_boolean (kf, name, "use-tls",   sbv_profile_get_use_tls (p));
    g_key_file_set_boolean (kf, name, "use-ldaps", sbv_profile_get_use_ldaps (p));
    g_key_file_set_boolean (kf, name, "skip-cert", sbv_profile_get_skip_cert (p));

    /* Only write range keys that are actually set. */
    if (sbv_profile_get_uid_min (p) >= 0)
      g_key_file_set_int64 (kf, name, "uid-min", sbv_profile_get_uid_min (p));
    if (sbv_profile_get_uid_max (p) >= 0)
      g_key_file_set_int64 (kf, name, "uid-max", sbv_profile_get_uid_max (p));
    if (sbv_profile_get_gid_min (p) >= 0)
      g_key_file_set_int64 (kf, name, "gid-min", sbv_profile_get_gid_min (p));
    if (sbv_profile_get_gid_max (p) >= 0)
      g_key_file_set_int64 (kf, name, "gid-max", sbv_profile_get_gid_max (p));

    g_object_unref (p);
  }

  char *dir  = g_build_filename (g_get_user_config_dir (), "sambervise", NULL);
  g_mkdir_with_parents (dir, 0700);
  g_free (dir);

  char    *path = get_config_path ();
  gboolean ok   = g_key_file_save_to_file (kf, path, error);
  g_key_file_free (kf);
  g_free (path);
  return ok;
}

/* ── Upsert ─────────────────────────────────────────────────────────────── */

gboolean
sbv_profiles_upsert (GListStore *store, SbvProfile *profile, GError **error)
{
  const char *name = sbv_profile_get_name (profile);
  guint       n    = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint i = 0; i < n; i++) {
    SbvProfile *p = g_list_model_get_item (G_LIST_MODEL (store), i);
    gboolean match = g_str_equal (sbv_profile_get_name (p) ?: "", name ?: "");
    g_object_unref (p);
    if (match) {
      g_list_store_remove (store, i);
      g_list_store_insert (store, i, profile);
      return sbv_profiles_save (store, error);
    }
  }

  g_list_store_append (store, profile);
  return sbv_profiles_save (store, error);
}

/* ── Remove ─────────────────────────────────────────────────────────────── */

gboolean
sbv_profiles_remove (GListStore *store, const char *name, GError **error)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    SbvProfile *p = g_list_model_get_item (G_LIST_MODEL (store), i);
    gboolean match = g_str_equal (sbv_profile_get_name (p) ?: "", name ?: "");
    g_object_unref (p);
    if (match) {
      g_list_store_remove (store, i);
      return sbv_profiles_save (store, error);
    }
  }
  return TRUE; /* not found — not an error */
}
