#include "sbv-policy-form.h"

#include <adwaita.h>
#include <string.h>

struct _SbvPolicyForm {
  GtkBox          parent;

  SbvDomainPolicy base;      /* as given to sbv_policy_form_set */
  gboolean        filling;   /* suppresses "changed" while filling */

  GtkWidget *password_group;
  GtkWidget *min_length;
  GtkWidget *history;
  GtkWidget *max_age;
  GtkWidget *min_age;
  GtkWidget *complex;
  GtkWidget *threshold;
  GtkWidget *duration;
  GtkWidget *window;
  GtkWidget *duration_row;
  GtkWidget *window_row;
};

G_DEFINE_TYPE (SbvPolicyForm, sbv_policy_form, GTK_TYPE_BOX)

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

SbvDomainPolicy
sbv_policy_form_get (SbvPolicyForm *self)
{
  SbvDomainPolicy p = self->base;

#define SPIN(w) ((gint64) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w)))
  p.min_pwd_length     = SPIN (self->min_length);
  p.pwd_history_length = SPIN (self->history);
  p.lockout_threshold  = SPIN (self->threshold);

  /* Keep the stored interval when the shown number hasn't changed, so a
   * value that isn't a whole number of days or minutes survives a save. */
  gint64 v;
  v = SPIN (self->max_age);
  if (v != sbv_interval_to_days (self->base.max_pwd_age))
    p.max_pwd_age = sbv_interval_from_days (v, TRUE);
  v = SPIN (self->min_age);
  if (v != sbv_interval_to_days (self->base.min_pwd_age))
    p.min_pwd_age = sbv_interval_from_days (v, FALSE);
  v = SPIN (self->duration);
  if (v != sbv_interval_to_minutes (self->base.lockout_duration))
    p.lockout_duration = sbv_interval_from_minutes (v, TRUE);
  v = SPIN (self->window);
  if (v != sbv_interval_to_minutes (self->base.lockout_observation_window))
    p.lockout_observation_window = sbv_interval_from_minutes (v, FALSE);
#undef SPIN

  if (gtk_switch_get_active (GTK_SWITCH (self->complex)))
    p.pwd_properties |= SBV_PWD_COMPLEX;
  else
    p.pwd_properties &= ~(gint64) SBV_PWD_COMPLEX;
  return p;
}

gboolean
sbv_policy_form_is_changed (SbvPolicyForm *self)
{
  SbvDomainPolicy p = sbv_policy_form_get (self);
  return memcmp (&p, &self->base, sizeof p) != 0;
}

/* Greys out the lockout timings while lockout is off. */
static void
update_sensitivity (SbvPolicyForm *self)
{
  gboolean lockout =
    gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->threshold)) > 0;
  gtk_widget_set_sensitive (self->duration_row, lockout);
  gtk_widget_set_sensitive (self->window_row,   lockout);
}

/* Any spin button edit. */
static void
on_value_changed (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  SbvPolicyForm *self = SBV_POLICY_FORM (user_data);
  update_sensitivity (self);
  if (!self->filling) g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

/* Switch edits arrive as a property notification. */
static void
on_switch_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void) obj;
  (void) pspec;
  SbvPolicyForm *self = SBV_POLICY_FORM (user_data);
  if (!self->filling) g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
sbv_policy_form_set (SbvPolicyForm *self, const SbvDomainPolicy *p)
{
  g_return_if_fail (SBV_IS_POLICY_FORM (self));
  self->base    = *p;
  self->filling = TRUE;
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->min_length), p->min_pwd_length);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->history),    p->pwd_history_length);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->max_age),    sbv_interval_to_days (p->max_pwd_age));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->min_age),    sbv_interval_to_days (p->min_pwd_age));
  gtk_switch_set_active (GTK_SWITCH (self->complex), (p->pwd_properties & SBV_PWD_COMPLEX) != 0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->threshold),  p->lockout_threshold);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->duration),   sbv_interval_to_minutes (p->lockout_duration));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->window),     sbv_interval_to_minutes (p->lockout_observation_window));
  self->filling = FALSE;
  update_sensitivity (self);
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

/* Adds a row with a spin button for a whole number from `lo` to `hi`. */
static GtkWidget *
add_spin_row (SbvPolicyForm *self, GtkWidget *group, const char *title,
              const char *subtitle, double lo, double hi, GtkWidget **row_out)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle) adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);

  GtkWidget *spin = gtk_spin_button_new_with_range (lo, hi, 1);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 0);
  gtk_widget_set_valign (spin, GTK_ALIGN_CENTER);
  g_signal_connect (spin, "value-changed", G_CALLBACK (on_value_changed), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), spin);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
  if (row_out) *row_out = row;
  return spin;
}

GtkWidget *
sbv_policy_form_new (const char *description, int max_history)
{
  SbvPolicyForm *self = g_object_new (SBV_TYPE_POLICY_FORM, NULL);
  GtkWidget     *pw   = self->password_group;
  if (description)
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (pw), description);
  gtk_spin_button_set_range (GTK_SPIN_BUTTON (self->history), 0, max_history);
  return GTK_WIDGET (self);
}

/* ── GObject ────────────────────────────────────────────────────────────── */

/* Registers the "changed" signal. */
static void
sbv_policy_form_class_init (SbvPolicyFormClass *klass)
{
  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/* Builds the Password Policy and Account Lockout groups. */
static void
sbv_policy_form_init (SbvPolicyForm *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 18);

  GtkWidget *pw = adw_preferences_group_new ();
  self->password_group = pw;
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (pw), "Password Policy");
  self->min_length = add_spin_row (self, pw, "Minimum length", "Characters",
                                   0, 255, NULL);
  self->history    = add_spin_row (self, pw, "Password history",
                                   "Previous passwords that can't be reused",
                                   0, 24, NULL);
  self->max_age    = add_spin_row (self, pw, "Maximum age",
                                   "Days. 0 means passwords never expire.",
                                   0, 999, NULL);
  self->min_age    = add_spin_row (self, pw, "Minimum age",
                                   "Days before a password can be changed again",
                                   0, 998, NULL);

  GtkWidget *crow = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (crow), "Require complex passwords");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (crow),
    "Three of: upper case, lower case, digits and symbols");
  self->complex = gtk_switch_new ();
  gtk_widget_set_valign (self->complex, GTK_ALIGN_CENTER);
  g_signal_connect (self->complex, "notify::active", G_CALLBACK (on_switch_notify), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (crow), self->complex);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (crow), self->complex);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (pw), crow);
  gtk_box_append (GTK_BOX (self), pw);

  GtkWidget *lo = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (lo), "Account Lockout");
  self->threshold = add_spin_row (self, lo, "Lockout threshold",
                                  "Failed logons before lockout. 0 turns lockout off.",
                                  0, 999, NULL);
  self->duration  = add_spin_row (self, lo, "Lockout duration",
                                  "Minutes. 0 keeps accounts locked until an admin unlocks them.",
                                  0, 99999, &self->duration_row);
  self->window    = add_spin_row (self, lo, "Reset failed logon count after",
                                  "Minutes", 1, 99999, &self->window_row);
  gtk_box_append (GTK_BOX (self), lo);
}
