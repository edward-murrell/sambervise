#include "sbv-domain-panel.h"
#include "sbv-window.h"
#include "../backend/sbv-domain-backend.h"

#include <adwaita.h>
#include <string.h>

struct _SbvDomainPanel {
  GtkBox         parent;

  SbvConnection *conn;
  SbvDomainPolicy *loaded;   /* policy as last read, or NULL */
  guint           load_gen;  /* drops replies from an older load */

  GtkWidget *policy_stack;   /* "loading" | "form" | "error" */
  GtkWidget *policy_error;

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

  GtkWidget *save_btn;
  GtkWidget *revert_btn;
  GtkWidget *save_spinner;
  GtkWidget *save_error;

  GtkWidget *fsmo_group;
  GPtrArray *fsmo_rows;      /* rows added to fsmo_group */
  GtkWidget *fsmo_status;
};

G_DEFINE_TYPE (SbvDomainPanel, sbv_domain_panel, GTK_TYPE_BOX)

enum { SIGNAL_POLICY_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* Reply context: the panel plus the load it belongs to. */
typedef struct {
  SbvDomainPanel *self;   /* owned ref */
  guint           gen;
} LoadCtx;

/* Makes a reply context for the current load. */
static LoadCtx *
load_ctx_new (SbvDomainPanel *self)
{
  LoadCtx *ctx = g_new0 (LoadCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->gen  = self->load_gen;
  return ctx;
}

/* Frees a reply context. */
static void
load_ctx_free (LoadCtx *ctx)
{
  g_object_unref (ctx->self);
  g_free (ctx);
}

/* ── Policy form ────────────────────────────────────────────────────────── */

/* Reads the form into a policy, keeping pwdProperties bits the form
 * doesn't show. */
static SbvDomainPolicy
form_policy (SbvDomainPanel *self)
{
  SbvDomainPolicy p = *self->loaded;

#define SPIN(w) ((gint64) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w)))
  p.min_pwd_length     = SPIN (self->min_length);
  p.pwd_history_length = SPIN (self->history);
  p.lockout_threshold  = SPIN (self->threshold);

  /* Keep the stored interval when the shown number hasn't changed, so a
   * value that isn't a whole number of days or minutes survives a save. */
  gint64 v;
  v = SPIN (self->max_age);
  if (v != sbv_interval_to_days (self->loaded->max_pwd_age))
    p.max_pwd_age = sbv_interval_from_days (v, TRUE);
  v = SPIN (self->min_age);
  if (v != sbv_interval_to_days (self->loaded->min_pwd_age))
    p.min_pwd_age = sbv_interval_from_days (v, FALSE);
  v = SPIN (self->duration);
  if (v != sbv_interval_to_minutes (self->loaded->lockout_duration))
    p.lockout_duration = sbv_interval_from_minutes (v, TRUE);
  v = SPIN (self->window);
  if (v != sbv_interval_to_minutes (self->loaded->lockout_observation_window))
    p.lockout_observation_window = sbv_interval_from_minutes (v, FALSE);
#undef SPIN

  if (gtk_switch_get_active (GTK_SWITCH (self->complex)))
    p.pwd_properties |= SBV_PWD_COMPLEX;
  else
    p.pwd_properties &= ~(gint64) SBV_PWD_COMPLEX;
  return p;
}

/* Enables Save and Revert while the form differs from what was read, and
 * greys out the lockout timings when lockout is off. */
static void
update_form_state (SbvDomainPanel *self)
{
  if (!self->loaded) return;
  SbvDomainPolicy p       = form_policy (self);
  gboolean        changed = memcmp (&p, self->loaded, sizeof p) != 0;
  gtk_widget_set_sensitive (self->save_btn,   changed);
  gtk_widget_set_sensitive (self->revert_btn, changed);

  gboolean lockout = p.lockout_threshold > 0;
  gtk_widget_set_sensitive (self->duration_row, lockout);
  gtk_widget_set_sensitive (self->window_row,   lockout);
}

/* Any form edit. */
static void
on_form_changed (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  update_form_state (SBV_DOMAIN_PANEL (user_data));
}

/* Switch edits arrive as a property notification. */
static void
on_switch_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void) obj;
  (void) pspec;
  update_form_state (SBV_DOMAIN_PANEL (user_data));
}

/* Puts the loaded policy into the form. */
static void
fill_form (SbvDomainPanel *self)
{
  SbvDomainPolicy *p = self->loaded;
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->min_length), p->min_pwd_length);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->history),    p->pwd_history_length);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->max_age),    sbv_interval_to_days (p->max_pwd_age));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->min_age),    sbv_interval_to_days (p->min_pwd_age));
  gtk_switch_set_active (GTK_SWITCH (self->complex), (p->pwd_properties & SBV_PWD_COMPLEX) != 0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->threshold),  p->lockout_threshold);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->duration),   sbv_interval_to_minutes (p->lockout_duration));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->window),     sbv_interval_to_minutes (p->lockout_observation_window));
  gtk_widget_set_visible (self->save_error, FALSE);
  update_form_state (self);
}

/* Policy read completion. */
static void
on_policy_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadCtx        *ctx  = user_data;
  SbvDomainPanel *self = ctx->self;
  GError         *err  = NULL;

  SbvDomainPolicy *p = sbv_domain_policy_read_finish (SBV_CONNECTION (source), result, &err);
  if (ctx->gen != self->load_gen) {
    g_free (p);
    g_clear_error (&err);
    load_ctx_free (ctx);
    return;
  }

  if (!p) {
    gtk_label_set_text (GTK_LABEL (self->policy_error), err->message);
    gtk_stack_set_visible_child_name (GTK_STACK (self->policy_stack), "error");
    g_error_free (err);
  } else {
    g_free (self->loaded);
    self->loaded = p;
    fill_form (self);
    gtk_stack_set_visible_child_name (GTK_STACK (self->policy_stack), "form");
  }
  load_ctx_free (ctx);
}

/* Toggles the busy state while a save is on. */
static void
set_saving (SbvDomainPanel *self, gboolean busy)
{
  gtk_widget_set_visible (self->save_spinner, busy);
  if (busy) gtk_spinner_start (GTK_SPINNER (self->save_spinner));
  else      gtk_spinner_stop  (GTK_SPINNER (self->save_spinner));
  gtk_widget_set_sensitive (self->policy_stack, !busy);
  if (busy) {
    gtk_widget_set_sensitive (self->save_btn,   FALSE);
    gtk_widget_set_sensitive (self->revert_btn, FALSE);
  } else {
    update_form_state (self);
  }
}

/* Save completion: re-read the policy so the form shows what the DC
 * stored, or show the DC's reason. */
static void
on_policy_saved (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (user_data);
  GError         *err  = NULL;

  gboolean ok = sbv_domain_policy_write_finish (SBV_CONNECTION (source), result, &err);
  set_saving (self, FALSE);
  if (!ok) {
    gtk_label_set_text (GTK_LABEL (self->save_error), err->message);
    gtk_widget_set_visible (self->save_error, TRUE);
    g_error_free (err);
  } else {
    GtkWidget *win = gtk_widget_get_ancestor (GTK_WIDGET (self), SBV_TYPE_WINDOW);
    if (win) sbv_window_show_toast (SBV_WINDOW (win), "Domain policy saved");
    g_signal_emit (self, signals[SIGNAL_POLICY_CHANGED], 0);
    if (self->conn) {
      self->load_gen++;
      sbv_domain_policy_read_async (self->conn, NULL, on_policy_loaded,
                                    load_ctx_new (self));
    }
  }
  g_object_unref (self);
}

/* "Save Changes": check the values, then write the changed ones. */
static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (user_data);
  if (!self->conn || !self->loaded) return;

  SbvDomainPolicy p   = form_policy (self);
  char           *msg = sbv_domain_policy_check (&p);
  if (msg) {
    gtk_label_set_text (GTK_LABEL (self->save_error), msg);
    gtk_widget_set_visible (self->save_error, TRUE);
    g_free (msg);
    return;
  }

  gtk_widget_set_visible (self->save_error, FALSE);
  set_saving (self, TRUE);
  sbv_domain_policy_write_async (self->conn, self->loaded, &p, NULL,
                                 on_policy_saved, g_object_ref (self));
}

/* "Revert": put back the values last read. */
static void
on_revert_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (user_data);
  if (self->loaded) fill_form (self);
}

/* ── FSMO roles ─────────────────────────────────────────────────────────── */

/* Removes the role rows from the group. */
static void
clear_fsmo_rows (SbvDomainPanel *self)
{
  for (guint i = 0; i < self->fsmo_rows->len; i++)
    adw_preferences_group_remove (ADW_PREFERENCES_GROUP (self->fsmo_group),
                                  g_ptr_array_index (self->fsmo_rows, i));
  g_ptr_array_set_size (self->fsmo_rows, 0);
}

/* Adds a row to the FSMO group and remembers it for clearing. */
static void
add_fsmo_row (SbvDomainPanel *self, GtkWidget *row)
{
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->fsmo_group), row);
  g_ptr_array_add (self->fsmo_rows, row);
}

/* FSMO read completion: one row per role, naming the holder DC. */
static void
on_fsmo_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadCtx        *ctx  = user_data;
  SbvDomainPanel *self = ctx->self;
  GError         *err  = NULL;

  GPtrArray *roles = sbv_domain_fsmo_read_finish (SBV_CONNECTION (source), result, &err);
  if (ctx->gen != self->load_gen) {
    if (roles) g_ptr_array_unref (roles);
    g_clear_error (&err);
    load_ctx_free (ctx);
    return;
  }

  gtk_widget_set_visible (self->fsmo_status, FALSE);
  clear_fsmo_rows (self);
  if (!roles) {
    gtk_label_set_text (GTK_LABEL (self->fsmo_status), err->message);
    gtk_widget_set_visible (self->fsmo_status, TRUE);
    g_error_free (err);
    load_ctx_free (ctx);
    return;
  }

  for (guint i = 0; i < roles->len; i++) {
    SbvFsmoRole *r   = g_ptr_array_index (roles, i);
    GtkWidget   *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), r->role);

    char *subtitle;
    if (!r->holder_dn)
      subtitle = g_strdup ("Couldn't read the holder");
    else if (r->deleted)
      subtitle = g_strdup_printf ("%s, a deleted DC. Seize this role with samba-tool fsmo.",
                                  r->server ? r->server : "Unknown");
    else if (r->host)
      subtitle = g_strdup_printf ("%s, site %s", r->host, r->site ? r->site : "unknown");
    else
      subtitle = g_strdup_printf ("%s, site %s", r->server ? r->server : r->holder_dn,
                                  r->site ? r->site : "unknown");
    char *escaped = g_markup_escape_text (subtitle, -1);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), escaped);
    g_free (escaped);
    g_free (subtitle);
    if (r->holder_dn) gtk_widget_set_tooltip_text (row, r->holder_dn);

    if (!r->holder_dn || r->deleted) {
      GtkWidget *icon = gtk_image_new_from_icon_name ("dialog-warning-symbolic");
      gtk_widget_add_css_class (icon, "warning");
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
    }
    add_fsmo_row (self, row);
  }
  g_ptr_array_unref (roles);
  load_ctx_free (ctx);
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

/* Adds a row with a spin button for a whole number from `lo` to `hi`. */
static GtkWidget *
add_spin_row (GtkWidget *group, const char *title, const char *subtitle,
              double lo, double hi, GtkWidget **row_out, SbvDomainPanel *self)
{
  GtkWidget *row  = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle) adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);

  GtkWidget *spin = gtk_spin_button_new_with_range (lo, hi, 1);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 0);
  gtk_widget_set_valign (spin, GTK_ALIGN_CENTER);
  g_signal_connect (spin, "value-changed", G_CALLBACK (on_form_changed), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), spin);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
  if (row_out) *row_out = row;
  return spin;
}

/* Builds the password and lockout groups with Save and Revert. */
static GtkWidget *
build_policy (SbvDomainPanel *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);

  GtkWidget *pw = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (pw), "Password Policy");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (pw),
    "Applies to every account that no fine-grained policy covers.");
  self->min_length = add_spin_row (pw, "Minimum length", "Characters",
                                   0, 255, NULL, self);
  self->history    = add_spin_row (pw, "Password history",
                                   "Previous passwords that can't be reused",
                                   0, 24, NULL, self);
  self->max_age    = add_spin_row (pw, "Maximum age", "Days. 0 means passwords never expire.",
                                   0, 999, NULL, self);
  self->min_age    = add_spin_row (pw, "Minimum age", "Days before a password can be changed again",
                                   0, 998, NULL, self);

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
  gtk_box_append (GTK_BOX (box), pw);

  GtkWidget *lo = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (lo), "Account Lockout");
  self->threshold = add_spin_row (lo, "Lockout threshold",
                                  "Failed logons before lockout. 0 turns lockout off.",
                                  0, 999, NULL, self);
  self->duration  = add_spin_row (lo, "Lockout duration",
                                  "Minutes. 0 keeps accounts locked until an admin unlocks them.",
                                  0, 99999, &self->duration_row, self);
  self->window    = add_spin_row (lo, "Reset failed logon count after",
                                  "Minutes",
                                  1, 99999, &self->window_row, self);
  gtk_box_append (GTK_BOX (box), lo);

  self->save_error = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->save_error), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->save_error), 0);
  gtk_widget_add_css_class (self->save_error, "error");
  gtk_widget_set_visible (self->save_error, FALSE);
  gtk_box_append (GTK_BOX (box), self->save_error);

  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (actions, GTK_ALIGN_END);
  self->save_spinner = gtk_spinner_new ();
  gtk_widget_set_visible (self->save_spinner, FALSE);
  gtk_box_append (GTK_BOX (actions), self->save_spinner);
  self->revert_btn = gtk_button_new_with_label ("Revert");
  g_signal_connect (self->revert_btn, "clicked", G_CALLBACK (on_revert_clicked), self);
  gtk_box_append (GTK_BOX (actions), self->revert_btn);
  self->save_btn = gtk_button_new_with_label ("Save Changes");
  gtk_widget_add_css_class (self->save_btn, "suggested-action");
  g_signal_connect (self->save_btn, "clicked", G_CALLBACK (on_save_clicked), self);
  gtk_box_append (GTK_BOX (actions), self->save_btn);
  gtk_box_append (GTK_BOX (box), actions);

  /* Loading and error pages share a stack with the form. */
  self->policy_stack = gtk_stack_new ();
  gtk_stack_set_vhomogeneous (GTK_STACK (self->policy_stack), FALSE);
  GtkWidget *spin = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (spin));
  gtk_widget_set_margin_top (spin, 24);
  gtk_stack_add_named (GTK_STACK (self->policy_stack), spin, "loading");
  gtk_stack_add_named (GTK_STACK (self->policy_stack), box, "form");
  self->policy_error = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->policy_error), TRUE);
  gtk_widget_add_css_class (self->policy_error, "error");
  gtk_stack_add_named (GTK_STACK (self->policy_stack), self->policy_error, "error");
  return self->policy_stack;
}

/* Builds the read-only FSMO role group. */
static GtkWidget *
build_fsmo (SbvDomainPanel *self)
{
  self->fsmo_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (self->fsmo_group),
                                   "Operations Masters (FSMO)");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (self->fsmo_group),
    "The DC that holds each role. Transfer or seize roles with samba-tool fsmo.");

  self->fsmo_status = gtk_label_new ("Loading…");
  gtk_label_set_wrap (GTK_LABEL (self->fsmo_status), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->fsmo_status), 0);
  gtk_widget_add_css_class (self->fsmo_status, "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->fsmo_group), self->fsmo_status);
  return self->fsmo_group;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

GtkWidget *
sbv_domain_panel_new (void)
{
  return g_object_new (SBV_TYPE_DOMAIN_PANEL, NULL);
}

void
sbv_domain_panel_load (SbvDomainPanel *self, SbvConnection *conn)
{
  g_return_if_fail (SBV_IS_DOMAIN_PANEL (self));
  g_set_object (&self->conn, conn);
  if (!conn) return;

  self->load_gen++;
  gtk_stack_set_visible_child_name (GTK_STACK (self->policy_stack), "loading");
  g_clear_pointer (&self->loaded, g_free);
  clear_fsmo_rows (self);
  gtk_label_set_text (GTK_LABEL (self->fsmo_status), "Loading…");
  gtk_widget_set_visible (self->fsmo_status, TRUE);

  sbv_domain_policy_read_async (conn, NULL, on_policy_loaded, load_ctx_new (self));
  sbv_domain_fsmo_read_async   (conn, NULL, on_fsmo_loaded,   load_ctx_new (self));
}

/* ── GObject ────────────────────────────────────────────────────────────── */

static void
sbv_domain_panel_finalize (GObject *object)
{
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (object);
  g_clear_object (&self->conn);
  g_free (self->loaded);
  g_ptr_array_unref (self->fsmo_rows);
  G_OBJECT_CLASS (sbv_domain_panel_parent_class)->finalize (object);
}

/* Registers finalize and the "policy-changed" signal. */
static void
sbv_domain_panel_class_init (SbvDomainPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_domain_panel_finalize;
  signals[SIGNAL_POLICY_CHANGED] =
    g_signal_new ("policy-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/* Builds the scrolling page: policy form above, FSMO roles below. */
static void
sbv_domain_panel_init (SbvDomainPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  self->fsmo_rows = g_ptr_array_new ();

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 720);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
  gtk_widget_set_margin_top    (vbox, 24);
  gtk_widget_set_margin_bottom (vbox, 24);
  gtk_widget_set_margin_start  (vbox, 12);
  gtk_widget_set_margin_end    (vbox, 12);
  gtk_box_append (GTK_BOX (vbox), build_policy (self));
  gtk_box_append (GTK_BOX (vbox), build_fsmo (self));

  adw_clamp_set_child (ADW_CLAMP (clamp), vbox);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  gtk_box_append (GTK_BOX (self), scroll);
}
