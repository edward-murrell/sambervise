#include "sbv-domain-panel.h"
#include "sbv-window.h"
#include "sbv-policy-form.h"
#include "sbv-pso-dialog.h"
#include "../backend/sbv-domain-backend.h"
#include "../backend/sbv-pso-backend.h"

#include "../backend/sbv-dn.h"

#include <adwaita.h>
#include <string.h>

struct _SbvDomainPanel {
  GtkBox         parent;

  SbvConnection *conn;
  SbvDomainPolicy *loaded;   /* policy as last read, or NULL */
  guint           load_gen;  /* drops replies from an older load */
  guint           pso_gen;   /* the same, for the policy list */

  GtkWidget *policy_stack;   /* "loading" | "form" | "error" */
  GtkWidget *policy_error;

  GtkWidget *form;           /* SbvPolicyForm */

  GtkWidget *save_btn;
  GtkWidget *revert_btn;
  GtkWidget *save_spinner;
  GtkWidget *save_error;

  GtkWidget *fsmo_group;
  GPtrArray *fsmo_rows;      /* rows added to fsmo_group */
  GtkWidget *fsmo_status;

  GtkWidget *pso_group;
  GPtrArray *pso_rows;       /* rows added to pso_group */
  GPtrArray *psos;           /* SbvPso, as last listed, or NULL */
  GtkWidget *pso_status;
  GtkWidget *pso_add_btn;
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

/* Enables Save and Revert while the form differs from what was read. */
static void
update_form_state (SbvDomainPanel *self)
{
  if (!self->loaded) return;
  gboolean changed = sbv_policy_form_is_changed (SBV_POLICY_FORM (self->form));
  gtk_widget_set_sensitive (self->save_btn,   changed);
  gtk_widget_set_sensitive (self->revert_btn, changed);
}

/* Any form edit. */
static void
on_form_changed (SbvPolicyForm *form, gpointer user_data)
{
  (void) form;
  update_form_state (SBV_DOMAIN_PANEL (user_data));
}

/* Puts the loaded policy into the form. */
static void
fill_form (SbvDomainPanel *self)
{
  sbv_policy_form_set (SBV_POLICY_FORM (self->form), self->loaded);
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

  SbvDomainPolicy p   = sbv_policy_form_get (SBV_POLICY_FORM (self->form));
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

/* ── Fine-grained password policies ─────────────────────────────────────── */

static void load_psos (SbvDomainPanel *self);

/* Removes the policy rows from the group. */
static void
clear_pso_rows (SbvDomainPanel *self)
{
  for (guint i = 0; i < self->pso_rows->len; i++)
    adw_preferences_group_remove (ADW_PREFERENCES_GROUP (self->pso_group),
                                  g_ptr_array_index (self->pso_rows, i));
  g_ptr_array_set_size (self->pso_rows, 0);
}

/* Shows `text` in the policy group's status line, or hides it for NULL. */
static void
set_pso_status (SbvDomainPanel *self, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (self->pso_status), text ? text : "");
  gtk_widget_set_visible (self->pso_status, text != NULL);
  if (error) {
    gtk_widget_remove_css_class (self->pso_status, "dim-label");
    gtk_widget_add_css_class (self->pso_status, "error");
  } else {
    gtk_widget_remove_css_class (self->pso_status, "error");
    gtk_widget_add_css_class (self->pso_status, "dim-label");
  }
}

/* Builds "Precedence 10. Applies to a, b and 2 more." for a policy row.
 * Caller frees. */
static char *
pso_summary (const SbvPso *pso)
{
  GString *s = g_string_new (NULL);
  g_string_append_printf (s, "Precedence %" G_GINT64_FORMAT ". ", pso->precedence);
  guint n = pso->applies_to ? g_strv_length (pso->applies_to) : 0;
  if (n == 0) {
    g_string_append (s, "Applies to nobody.");
    return g_string_free (s, FALSE);
  }
  g_string_append (s, "Applies to ");
  guint shown = MIN (n, 3);
  for (guint i = 0; i < shown; i++) {
    char *name = sbv_dn_rdn_value (pso->applies_to[i]);
    if (i > 0) g_string_append (s, (i == shown - 1 && shown == n) ? " and " : ", ");
    g_string_append (s, name ? name : pso->applies_to[i]);
    g_free (name);
  }
  if (n > shown) g_string_append_printf (s, " and %u more", n - shown);
  g_string_append_c (s, '.');
  return g_string_free (s, FALSE);
}

/* Runs after the dialog creates, saves or deletes a policy. */
static void
on_pso_done (const char *message, gpointer user_data)
{
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (user_data);
  GtkWidget      *win  = gtk_widget_get_ancestor (GTK_WIDGET (self), SBV_TYPE_WINDOW);
  if (win) sbv_window_show_toast (SBV_WINDOW (win), message);
  load_psos (self);
  g_signal_emit (self, signals[SIGNAL_POLICY_CHANGED], 0);
}

/* Opens the policy dialog for `pso`, new or existing. */
static void
open_pso_dialog (SbvDomainPanel *self, const SbvPso *pso)
{
  if (!self->conn) return;
  GtkWidget *top = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  sbv_pso_dialog_show (top ? GTK_WINDOW (top) : NULL, self->conn, pso,
                       on_pso_done, g_object_ref (self), g_object_unref);
}

/* A policy row was activated. */
static void
on_pso_row_activated (AdwActionRow *row, gpointer user_data)
{
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (user_data);
  guint           i    = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "pso-index"));
  if (self->psos && i < self->psos->len)
    open_pso_dialog (self, g_ptr_array_index (self->psos, i));
}

/* "+" on the policy group: start from the domain policy's values, with
 * a precedence after the last policy. */
static void
on_pso_add_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (user_data);
  SbvPso         *pso  = sbv_pso_new (self->loaded);
  if (self->psos && self->psos->len > 0) {
    SbvPso *last = g_ptr_array_index (self->psos, self->psos->len - 1);
    pso->precedence = (last->precedence / 10 + 1) * 10;
  }
  open_pso_dialog (self, pso);
  sbv_pso_free (pso);
}

/* Policy list completion: one row per policy, in precedence order. */
static void
on_psos_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LoadCtx        *ctx  = user_data;
  SbvDomainPanel *self = ctx->self;
  GError         *err  = NULL;

  GPtrArray *list = sbv_pso_list_finish (SBV_CONNECTION (source), result, &err);
  if (ctx->gen != self->pso_gen) {
    if (list) g_ptr_array_unref (list);
    g_clear_error (&err);
    load_ctx_free (ctx);
    return;
  }

  clear_pso_rows (self);
  g_clear_pointer (&self->psos, g_ptr_array_unref);
  if (!list) {
    set_pso_status (self, err->message, TRUE);
    g_error_free (err);
    load_ctx_free (ctx);
    return;
  }

  self->psos = list;
  gtk_widget_set_sensitive (self->pso_add_btn, TRUE);
  set_pso_status (self, list->len == 0 ? "No fine-grained policies" : NULL, FALSE);
  for (guint i = 0; i < list->len; i++) {
    SbvPso    *pso = g_ptr_array_index (list, i);
    GtkWidget *row = adw_action_row_new ();
    char      *t   = g_markup_escape_text (pso->name ? pso->name : "", -1);
    char      *sum = pso_summary (pso);
    char      *sub = g_markup_escape_text (sum, -1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), t);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
    if (pso->description && *pso->description)
      gtk_widget_set_tooltip_text (row, pso->description);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row),
                               gtk_image_new_from_icon_name ("go-next-symbolic"));
    g_object_set_data (G_OBJECT (row), "pso-index", GUINT_TO_POINTER (i));
    g_signal_connect (row, "activated", G_CALLBACK (on_pso_row_activated), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->pso_group), row);
    g_ptr_array_add (self->pso_rows, row);
    g_free (t);
    g_free (sum);
    g_free (sub);
  }
  load_ctx_free (ctx);
}

/* Starts a fresh read of the policy list. */
static void
load_psos (SbvDomainPanel *self)
{
  if (!self->conn) return;
  self->pso_gen++;
  LoadCtx *ctx = load_ctx_new (self);
  ctx->gen = self->pso_gen;
  sbv_pso_list_async (self->conn, NULL, on_psos_loaded, ctx);
}

/* Builds the policy list group with its "+" button. */
static GtkWidget *
build_psos (SbvDomainPanel *self)
{
  self->pso_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (self->pso_group),
                                   "Fine-Grained Password Policies");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (self->pso_group),
    "Override the domain policy for chosen users and groups");

  self->pso_add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (self->pso_add_btn, "flat");
  gtk_widget_set_valign (self->pso_add_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (self->pso_add_btn, "New password policy");
  gtk_widget_set_sensitive (self->pso_add_btn, FALSE);
  g_signal_connect (self->pso_add_btn, "clicked", G_CALLBACK (on_pso_add_clicked), self);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (self->pso_group),
                                           self->pso_add_btn);

  self->pso_status = gtk_label_new ("Loading…");
  gtk_label_set_wrap (GTK_LABEL (self->pso_status), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->pso_status), 0);
  gtk_widget_add_css_class (self->pso_status, "dim-label");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->pso_group), self->pso_status);
  return self->pso_group;
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

/* Builds the password and lockout form with Save and Revert. */
static GtkWidget *
build_policy (SbvDomainPanel *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);

  self->form = sbv_policy_form_new (
    "Applies to every account that no fine-grained policy covers.", 24);
  g_signal_connect (self->form, "changed", G_CALLBACK (on_form_changed), self);
  gtk_box_append (GTK_BOX (box), self->form);

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

  clear_pso_rows (self);
  g_clear_pointer (&self->psos, g_ptr_array_unref);
  gtk_widget_set_sensitive (self->pso_add_btn, FALSE);
  set_pso_status (self, "Loading…", FALSE);
  load_psos (self);
}

/* ── GObject ────────────────────────────────────────────────────────────── */

static void
sbv_domain_panel_finalize (GObject *object)
{
  SbvDomainPanel *self = SBV_DOMAIN_PANEL (object);
  g_clear_object (&self->conn);
  g_free (self->loaded);
  g_ptr_array_unref (self->fsmo_rows);
  g_ptr_array_unref (self->pso_rows);
  if (self->psos) g_ptr_array_unref (self->psos);
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

/* Builds the scrolling page: the domain policy form, then fine-grained
 * policies, then FSMO roles. */
static void
sbv_domain_panel_init (SbvDomainPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  self->fsmo_rows = g_ptr_array_new ();
  self->pso_rows  = g_ptr_array_new ();

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
  gtk_box_append (GTK_BOX (vbox), build_psos (self));
  gtk_box_append (GTK_BOX (vbox), build_fsmo (self));

  adw_clamp_set_child (ADW_CLAMP (clamp), vbox);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  gtk_box_append (GTK_BOX (self), scroll);
}
