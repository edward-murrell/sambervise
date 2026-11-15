#pragma once

#include <gtk/gtk.h>
#include "../model/sbv-domain.h"

G_BEGIN_DECLS

#define SBV_TYPE_POLICY_FORM (sbv_policy_form_get_type ())
G_DECLARE_FINAL_TYPE (SbvPolicyForm, sbv_policy_form, SBV, POLICY_FORM, GtkBox)

/* The password and lockout settings as two preference groups. The Domain
 * page and the fine-grained policy dialog both use it.
 *
 * Emits "changed" (no arguments) on every edit. */

/* Creates the form. `description` goes under the Password Policy title
 * and may be NULL. `max_history` caps the password history setting. */
GtkWidget *sbv_policy_form_new         (const char *description,
                                        int         max_history);

/* Shows `policy` and keeps it as the starting point. Copied. */
void       sbv_policy_form_set         (SbvPolicyForm         *self,
                                        const SbvDomainPolicy *policy);

/* Returns the policy as edited. Fields the form doesn't show, and
 * intervals whose shown number hasn't changed, keep the values given to
 * sbv_policy_form_set. */
SbvDomainPolicy sbv_policy_form_get    (SbvPolicyForm *self);

/* Returns TRUE when the form differs from what sbv_policy_form_set gave. */
gboolean   sbv_policy_form_is_changed  (SbvPolicyForm *self);

G_END_DECLS
