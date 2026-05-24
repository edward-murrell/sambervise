#include "sbv-contacts-backend.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* List of attributes we project into the SbvContact model on list. The raw
 * dump is collected separately so the detail pane can render anything not in
 * this projection. */
static const char *CONTACT_ATTRS[] = {
  "distinguishedName", "cn", "displayName", "givenName", "sn",
  "description", "mail", "proxyAddresses",
  "telephoneNumber", "mobile", "facsimileTelephoneNumber",
  "physicalDeliveryOfficeName",
  "streetAddress", "l", "st", "postalCode", "co",
  NULL
};

/* Worker: subtree search for objectClass=contact. AD users also inherit from
 * top/person/organizationalPerson, but `user` is its own structural class so
 * filtering on contact-only is enough — the directory's contact objects do
 * not carry objectClass=user. */
static void
list_thread (GTask *task, gpointer source, gpointer task_data,
             GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  (void) task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  const char *base   = sbv_connection_get_base_dn (conn);
  const char *filter = "(&(objectClass=contact)(!(objectClass=user)))";

  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 30, 0 };
  int rc = ldap_search_ext_s (ld, base, LDAP_SCOPE_SUBTREE,
                               filter, (char **) CONTACT_ATTRS, 0,
                               NULL, NULL, &tv, LDAP_NO_LIMIT, &result);
  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Search failed: %s", ldap_err2string (rc));
    return;
  }

  GListStore *store = g_list_store_new (SBV_TYPE_CONTACT);

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      SbvContact *contact = sbv_contact_new ();
      struct berval **bv;

#define GET_STR(attr, setter) \
      bv = ldap_get_values_len (ld, entry, (attr)); \
      if (bv && bv[0]) setter (contact, bv[0]->bv_val); \
      ldap_value_free_len (bv);

      GET_STR ("distinguishedName",         sbv_contact_set_dn)
      GET_STR ("cn",                        sbv_contact_set_cn)
      GET_STR ("displayName",               sbv_contact_set_display_name)
      GET_STR ("givenName",                 sbv_contact_set_given_name)
      GET_STR ("sn",                        sbv_contact_set_sn)
      GET_STR ("description",               sbv_contact_set_description)
      GET_STR ("mail",                      sbv_contact_set_mail)
      GET_STR ("telephoneNumber",           sbv_contact_set_telephone_number)
      GET_STR ("mobile",                    sbv_contact_set_mobile)
      GET_STR ("facsimileTelephoneNumber",  sbv_contact_set_fax)
      GET_STR ("physicalDeliveryOfficeName",sbv_contact_set_office)
      GET_STR ("streetAddress",             sbv_contact_set_street_address)
      GET_STR ("l",                         sbv_contact_set_locality)
      GET_STR ("st",                        sbv_contact_set_state)
      GET_STR ("postalCode",                sbv_contact_set_postal_code)
      GET_STR ("co",                        sbv_contact_set_country)

#undef GET_STR

      /* proxyAddresses: multi-valued. */
      bv = ldap_get_values_len (ld, entry, "proxyAddresses");
      if (bv) {
        int cnt = ldap_count_values_len (bv);
        char **sv = g_new0 (char *, cnt + 1);
        for (int j = 0; j < cnt; j++)
          sv[j] = g_strndup (bv[j]->bv_val, bv[j]->bv_len);
        sbv_contact_set_proxy_addresses (contact, sv);
        ldap_value_free_len (bv);
      }

      /* Raw LDAP attribute dump for the detail pane. */
      {
        GHashTable *raw = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, (GDestroyNotify) g_strfreev);
        BerElement *ber2 = NULL;
        char *aname;
        for (aname = ldap_first_attribute (ld, entry, &ber2);
             aname != NULL;
             aname = ldap_next_attribute (ld, entry, ber2))
        {
          struct berval **av = ldap_get_values_len (ld, entry, aname);
          if (av) {
            int cnt = ldap_count_values_len (av);
            char **sv = g_new0 (char *, cnt + 1);
            for (int j = 0; j < cnt; j++) {
              sv[j] = g_utf8_validate (av[j]->bv_val, (gssize) av[j]->bv_len, NULL)
                ? g_strndup (av[j]->bv_val, av[j]->bv_len)
                : g_strdup_printf ("<binary %zu bytes>", av[j]->bv_len);
            }
            g_hash_table_insert (raw, g_strdup (aname), sv);
            ldap_value_free_len (av);
          }
          ldap_memfree (aname);
        }
        if (ber2) ber_free (ber2, 0);
        sbv_contact_set_ldap_attrs (contact, raw);
      }

      g_list_store_append (store, contact);
      g_object_unref (contact);
    }

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_task_return_pointer (task, store, g_object_unref);
}

/* Public entry — see header. */
void
sbv_contacts_list_async (SbvConnection       *conn,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, list_thread);
  g_object_unref (task);
}

/* Returns the populated GListStore<SbvContact> or NULL on error. */
GListStore *
sbv_contacts_list_finish (SbvConnection *conn,
                           GAsyncResult  *result,
                           GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Create ─────────────────────────────────────────────────────────────── */

typedef struct {
  char *cn;
  char *given_name;
  char *sn;
  char *mail;
  char *telephone_number;
  char *container_dn;
} CreateData;

/* Frees a CreateData payload previously attached to a GTask. */
static void
create_data_free (CreateData *d)
{
  g_free (d->cn);
  g_free (d->given_name);
  g_free (d->sn);
  g_free (d->mail);
  g_free (d->telephone_number);
  g_free (d->container_dn);
  g_free (d);
}

/* Worker: builds the attribute set for a fresh AD `contact` object and issues
 * a single ldap_add_ext_s. AD requires the objectClass chain
 * top/person/organizationalPerson/contact for a contact entry. */
static void
create_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  CreateData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  char *dn = g_strdup_printf ("CN=%s,%s", d->cn, d->container_dn);

  char *oc_vals[]    = { "top", "person", "organizationalPerson", "contact", NULL };
  char *cn_vals[]    = { d->cn, NULL };
  char *gn_vals[]    = { d->given_name, NULL };
  char *sn_vals[]    = { d->sn, NULL };
  char *mail_vals[]  = { d->mail, NULL };
  char *tel_vals[]   = { d->telephone_number, NULL };

  LDAPMod oc_mod   = { LDAP_MOD_ADD, "objectClass",     { .modv_strvals = oc_vals   } };
  LDAPMod cn_mod   = { LDAP_MOD_ADD, "cn",              { .modv_strvals = cn_vals   } };
  LDAPMod gn_mod   = { LDAP_MOD_ADD, "givenName",       { .modv_strvals = gn_vals   } };
  LDAPMod sn_mod   = { LDAP_MOD_ADD, "sn",              { .modv_strvals = sn_vals   } };
  LDAPMod mail_mod = { LDAP_MOD_ADD, "mail",            { .modv_strvals = mail_vals } };
  LDAPMod tel_mod  = { LDAP_MOD_ADD, "telephoneNumber", { .modv_strvals = tel_vals  } };

  LDAPMod *mods[8];
  int n = 0;
  mods[n++] = &oc_mod;
  mods[n++] = &cn_mod;
  if (d->given_name       && *d->given_name)       mods[n++] = &gn_mod;
  if (d->sn               && *d->sn)               mods[n++] = &sn_mod;
  if (d->mail             && *d->mail)             mods[n++] = &mail_mod;
  if (d->telephone_number && *d->telephone_number) mods[n++] = &tel_mod;
  mods[n] = NULL;

  int rc = ldap_add_ext_s (ld, dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS) {
    g_free (dn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Create failed: %s", ldap_err2string (rc));
    return;
  }
  g_task_return_pointer (task, dn, g_free);
}

/* Public entry — validates required fields and dispatches to a worker. */
void
sbv_contacts_create_async (SbvConnection       *conn,
                            const char          *cn,
                            const char          *given_name,
                            const char          *sn,
                            const char          *mail,
                            const char          *telephone_number,
                            const char          *container_dn,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);

  if (!cn || !*cn || !container_dn || !*container_dn) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Name and container are required");
    g_object_unref (task);
    return;
  }

  CreateData *d = g_new0 (CreateData, 1);
  d->cn               = g_strdup (cn);
  d->given_name       = g_strdup (given_name);
  d->sn               = g_strdup (sn);
  d->mail             = g_strdup (mail);
  d->telephone_number = g_strdup (telephone_number);
  d->container_dn     = g_strdup (container_dn);

  g_task_set_task_data (task, d, (GDestroyNotify) create_data_free);
  g_task_run_in_thread (task, create_thread);
  g_object_unref (task);
}

/* Returns the DN of the newly-created contact (caller frees) or NULL on error. */
char *
sbv_contacts_create_finish (SbvConnection *conn,
                             GAsyncResult  *result,
                             GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Delete ─────────────────────────────────────────────────────────────── */

/* Worker: issues a single ldap_delete_ext_s. Contacts are leaves. */
static void
delete_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  char          *dn   = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  int rc = ldap_delete_ext_s (ld, dn, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Delete failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

/* Public entry — captures the DN now so the SbvContact can be freed before
 * completion. */
void
sbv_contacts_delete_async (SbvConnection       *conn,
                            SbvContact          *contact,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (sbv_contact_get_dn (contact)), g_free);
  g_task_run_in_thread (task, delete_thread);
  g_object_unref (task);
}

gboolean
sbv_contacts_delete_finish (SbvConnection *conn,
                             GAsyncResult  *result,
                             GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Update ─────────────────────────────────────────────────────────────── */

typedef struct {
  char  *dn;
  char  *cn;
  /* Scalar fields: NULL = skip, "" = clear, else set. */
  char  *display_name;
  char  *given_name;
  char  *sn;
  char  *description;
  char  *mail;
  char  *telephone_number;
  char  *mobile;
  char  *fax;
  char  *office;
  char  *street_address;
  char  *locality;
  char  *state;
  char  *postal_code;
  char  *country;
  /* Multi-valued: NULL = skip, empty vector = clear, else replace. */
  char **proxy_addresses;
  gboolean proxy_addresses_set;
} UpdateData;

/* Frees one update payload (all owned strings + the optional GStrv). */
static void
update_data_free (UpdateData *d)
{
  g_free (d->dn);
  g_free (d->cn);
  g_free (d->display_name);
  g_free (d->given_name);
  g_free (d->sn);
  g_free (d->description);
  g_free (d->mail);
  g_free (d->telephone_number);
  g_free (d->mobile);
  g_free (d->fax);
  g_free (d->office);
  g_free (d->street_address);
  g_free (d->locality);
  g_free (d->state);
  g_free (d->postal_code);
  g_free (d->country);
  g_strfreev (d->proxy_addresses);
  g_free (d);
}

/* Worker: optionally rename the entry (cn = RDN), then issue a single
 * modify with one MOD_REPLACE per scalar field that's non-NULL and one for
 * proxyAddresses if that was supplied. Empty strings collapse to
 * MOD_REPLACE with mod_values=NULL which the directory treats as delete. */
static void
update_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  UpdateData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  char *effective_dn = g_strdup (d->dn);

  /* CN is the RDN — needs a rename. */
  if (d->cn && *d->cn) {
    char       *new_rdn = g_strdup_printf ("CN=%s", d->cn);
    const char *comma   = strchr (d->dn, ',');
    const char *parent  = comma ? comma + 1 : "";
    int rc = ldap_rename_s (ld, d->dn, new_rdn, NULL, 1, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      g_free (new_rdn);
      g_free (effective_dn);
      sbv_connection_release_ldap (conn);
      g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                               "Rename failed: %s", ldap_err2string (rc));
      return;
    }
    g_free (effective_dn);
    effective_dn = g_strdup_printf ("CN=%s,%s", d->cn, parent);
    g_free (new_rdn);
  }

  /* Up to 15 scalar attributes + proxyAddresses + terminator. */
  LDAPMod  mods_buf[16];
  LDAPMod *mods[17];
  char    *slot_vals[16][2];
  int      n = 0;
  memset (mods_buf, 0, sizeof mods_buf);

#define ADD_STR_MOD(attr, val) \
  do { \
    if ((val) != NULL) { \
      mods_buf[n].mod_op   = LDAP_MOD_REPLACE; \
      mods_buf[n].mod_type = (char *)(attr); \
      if (*(val)) { \
        slot_vals[n][0] = (char *)(val); \
        slot_vals[n][1] = NULL; \
        mods_buf[n].mod_values = slot_vals[n]; \
      } \
      mods[n] = &mods_buf[n]; \
      n++; \
    } \
  } while (0)

  ADD_STR_MOD ("displayName",                d->display_name);
  ADD_STR_MOD ("givenName",                  d->given_name);
  ADD_STR_MOD ("sn",                         d->sn);
  ADD_STR_MOD ("description",                d->description);
  ADD_STR_MOD ("mail",                       d->mail);
  ADD_STR_MOD ("telephoneNumber",            d->telephone_number);
  ADD_STR_MOD ("mobile",                     d->mobile);
  ADD_STR_MOD ("facsimileTelephoneNumber",   d->fax);
  ADD_STR_MOD ("physicalDeliveryOfficeName", d->office);
  ADD_STR_MOD ("streetAddress",              d->street_address);
  ADD_STR_MOD ("l",                          d->locality);
  ADD_STR_MOD ("st",                         d->state);
  ADD_STR_MOD ("postalCode",                 d->postal_code);
  ADD_STR_MOD ("co",                         d->country);

#undef ADD_STR_MOD

  if (d->proxy_addresses_set) {
    mods_buf[n].mod_op   = LDAP_MOD_REPLACE;
    mods_buf[n].mod_type = "proxyAddresses";
    /* If the vector is empty, leave mod_values NULL → attribute delete. */
    if (d->proxy_addresses && d->proxy_addresses[0])
      mods_buf[n].mod_values = d->proxy_addresses;
    mods[n] = &mods_buf[n];
    n++;
  }

  mods[n] = NULL;

  int rc = (n > 0) ? ldap_modify_ext_s (ld, effective_dn, mods, NULL, NULL)
                   : LDAP_SUCCESS;
  g_free (effective_dn);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Attribute update failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

/* Public entry — see header. */
void
sbv_contacts_update_async (SbvConnection       *conn,
                            SbvContact          *contact,
                            const char          *cn,
                            const char          *display_name,
                            const char          *given_name,
                            const char          *sn,
                            const char          *description,
                            const char          *mail,
                            const char *const   *proxy_addresses,
                            const char          *telephone_number,
                            const char          *mobile,
                            const char          *fax,
                            const char          *office,
                            const char          *street_address,
                            const char          *locality,
                            const char          *state,
                            const char          *postal_code,
                            const char          *country,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  UpdateData *d = g_new0 (UpdateData, 1);
  d->dn               = g_strdup (sbv_contact_get_dn (contact));
  d->cn               = g_strdup (cn);
  d->display_name     = g_strdup (display_name);
  d->given_name       = g_strdup (given_name);
  d->sn               = g_strdup (sn);
  d->description      = g_strdup (description);
  d->mail             = g_strdup (mail);
  d->telephone_number = g_strdup (telephone_number);
  d->mobile           = g_strdup (mobile);
  d->fax              = g_strdup (fax);
  d->office           = g_strdup (office);
  d->street_address   = g_strdup (street_address);
  d->locality         = g_strdup (locality);
  d->state            = g_strdup (state);
  d->postal_code      = g_strdup (postal_code);
  d->country          = g_strdup (country);

  if (proxy_addresses) {
    d->proxy_addresses_set = TRUE;
    d->proxy_addresses     = g_strdupv ((char **) proxy_addresses);
  }

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) update_data_free);
  g_task_run_in_thread (task, update_thread);
  g_object_unref (task);
}

gboolean
sbv_contacts_update_finish (SbvConnection *conn,
                             GAsyncResult  *result,
                             GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
