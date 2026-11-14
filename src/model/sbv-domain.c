#include "sbv-domain.h"
#include "../backend/sbv-dn.h"

#include <string.h>

#define TICKS_PER_MINUTE G_GINT64_CONSTANT (600000000)
#define TICKS_PER_DAY    G_GINT64_CONSTANT (864000000000)

/* Converts an interval to whole `unit`s, rounding to the nearest. */
static gint64
interval_to_units (gint64 interval, gint64 unit)
{
  if (interval == SBV_INTERVAL_NEVER || interval >= 0) return 0;
  return (-interval + unit / 2) / unit;
}

gint64
sbv_interval_to_days (gint64 interval)
{
  return interval_to_units (interval, TICKS_PER_DAY);
}

gint64
sbv_interval_to_minutes (gint64 interval)
{
  return interval_to_units (interval, TICKS_PER_MINUTE);
}

gint64
sbv_interval_from_days (gint64 days, gboolean zero_is_never)
{
  if (days <= 0) return zero_is_never ? SBV_INTERVAL_NEVER : 0;
  return -days * TICKS_PER_DAY;
}

gint64
sbv_interval_from_minutes (gint64 minutes, gboolean zero_is_never)
{
  if (minutes <= 0) return zero_is_never ? SBV_INTERVAL_NEVER : 0;
  return -minutes * TICKS_PER_MINUTE;
}

char *
sbv_domain_policy_check (const SbvDomainPolicy *p)
{
  gint64 max_days = sbv_interval_to_days (p->max_pwd_age);
  gint64 min_days = sbv_interval_to_days (p->min_pwd_age);
  if (max_days > 0 && min_days >= max_days)
    return g_strdup ("Minimum password age must be less than the maximum age.");

  if (p->lockout_threshold > 0) {
    gint64 duration = sbv_interval_to_minutes (p->lockout_duration);
    gint64 window   = sbv_interval_to_minutes (p->lockout_observation_window);
    if (window <= 0)
      return g_strdup ("Reset the failed logon count after at least 1 minute.");
    if (duration > 0 && window > duration)
      return g_strdup ("The failed logon count must reset within the lockout duration.");
  }
  return NULL;
}

void
sbv_fsmo_role_free (SbvFsmoRole *role)
{
  if (!role) return;
  g_free (role->role);
  g_free (role->holder_dn);
  g_free (role->server);
  g_free (role->host);
  g_free (role->site);
  g_free (role);
}

gboolean
sbv_fsmo_parse_holder (const char  *holder_dn,
                       char       **server,
                       char       **server_dn,
                       char       **site,
                       gboolean    *deleted)
{
  if (server)    *server    = NULL;
  if (server_dn) *server_dn = NULL;
  if (site)      *site      = NULL;
  if (deleted)   *deleted   = FALSE;
  if (!holder_dn || !*holder_dn) return FALSE;

  /* A deleted object's RDN carries "\0ADEL:<guid>". */
  if (deleted)
    *deleted = strstr (holder_dn, "\\0ADEL:") != NULL
            || strstr (holder_dn, "\nDEL:")   != NULL;

  /* NTDS Settings -> server -> Servers container -> site. */
  char *srv_dn = NULL;
  if (!sbv_dn_split (holder_dn, NULL, &srv_dn)) return FALSE;

  char *servers_dn = NULL;
  sbv_dn_split (srv_dn, NULL, &servers_dn);

  if (server)    *server = sbv_dn_rdn_value (srv_dn);
  if (site && servers_dn) {
    char *site_dn = NULL;
    if (sbv_dn_split (servers_dn, NULL, &site_dn))
      *site = sbv_dn_rdn_value (site_dn);
    g_free (site_dn);
  }
  g_free (servers_dn);

  if (server_dn) *server_dn = srv_dn;
  else           g_free (srv_dn);
  return TRUE;
}
