#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Domain-wide policy and FSMO role data. Plain structs and pure helpers,
 * with no I/O, so they are unit-testable. */

/* AD stores "never" for an interval as the most negative 64-bit value. */
#define SBV_INTERVAL_NEVER G_MININT64

/* pwdProperties bit: passwords must meet complexity rules. */
#define SBV_PWD_COMPLEX 0x1

/* Password and lockout attributes on the domain root. Intervals are AD
 * time spans: negative counts of 100 ns ticks, or SBV_INTERVAL_NEVER. */
typedef struct {
  gint64 min_pwd_length;
  gint64 pwd_history_length;
  gint64 max_pwd_age;
  gint64 min_pwd_age;
  gint64 pwd_properties;
  gint64 lockout_threshold;
  gint64 lockout_duration;
  gint64 lockout_observation_window;
} SbvDomainPolicy;

/* Converts an interval to whole days, rounded. Zero and "never" give 0. */
gint64 sbv_interval_to_days      (gint64 interval);

/* Converts an interval to whole minutes, rounded. Zero and "never"
 * give 0. */
gint64 sbv_interval_to_minutes   (gint64 interval);

/* Converts days to an interval. 0 gives SBV_INTERVAL_NEVER when
 * `zero_is_never` is TRUE, otherwise 0. */
gint64 sbv_interval_from_days    (gint64 days, gboolean zero_is_never);

/* Converts minutes to an interval, with the same rule for 0. */
gint64 sbv_interval_from_minutes (gint64 minutes, gboolean zero_is_never);

/* Checks a policy for combinations the DC rejects. Returns NULL when it
 * is fine, otherwise a message for the user. Caller frees. */
char  *sbv_domain_policy_check   (const SbvDomainPolicy *policy);

/* A fine-grained password policy: an msDS-PasswordSettings object (R12).
 * `policy` holds its eight settings, with SBV_PWD_COMPLEX in
 * pwd_properties for msDS-PasswordComplexityEnabled. */
typedef struct {
  char            *dn;           /* NULL until created */
  char            *name;         /* cn */
  char            *description;  /* or NULL */
  gint64           precedence;   /* lower wins */
  gboolean         reversible;   /* msDS-PasswordReversibleEncryptionEnabled */
  SbvDomainPolicy  policy;
  char           **applies_to;   /* msDS-PSOAppliesTo DNs, or NULL */
} SbvPso;

/* Makes a new policy with the settings of `base`, or zeros when NULL.
 * Free with sbv_pso_free. */
SbvPso  *sbv_pso_new        (const SbvDomainPolicy *base);

/* Deep-copies a policy. */
SbvPso  *sbv_pso_copy       (const SbvPso *pso);

void     sbv_pso_free       (SbvPso *pso);

/* Checks a policy for values the DC rejects. Returns NULL when it is
 * fine, otherwise a message for the user. Caller frees. */
char    *sbv_pso_check      (const SbvPso *pso);

/* Orders policies by precedence, then name. For g_ptr_array_sort on an
 * array of SbvPso pointers. */
gint     sbv_pso_compare    (gconstpointer a, gconstpointer b);

/* Says whether two lists of DNs hold the same DNs, in any order. NULL
 * counts as empty. */
gboolean sbv_dn_lists_equal (const char * const *a, const char * const *b);

/* One FSMO role and the DC that holds it. */
typedef struct {
  char     *role;        /* e.g. "PDC Emulator" */
  char     *holder_dn;   /* the holder's NTDS Settings DN */
  char     *server;      /* the holder's server name, e.g. "DC1" */
  char     *host;        /* the server's dNSHostName, or NULL */
  char     *site;        /* the server's site, or NULL */
  gboolean  deleted;     /* the holder DC has been deleted */
} SbvFsmoRole;

void sbv_fsmo_role_free (SbvFsmoRole *role);

/* Splits an fSMORoleOwner DN, such as "CN=NTDS Settings,CN=DC1,
 * CN=Servers,CN=Site1,CN=Sites,CN=Configuration,...", into the server
 * name, the server object's DN and the site name. Sets `*deleted` when
 * the DN names a deleted object. Any out pointer may be NULL. Returns
 * FALSE when the DN is too short to parse. Caller frees the outs. */
gboolean sbv_fsmo_parse_holder (const char  *holder_dn,
                                char       **server,
                                char       **server_dn,
                                char       **site,
                                gboolean    *deleted);

G_END_DECLS
