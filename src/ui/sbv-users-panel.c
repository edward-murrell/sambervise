#include "sbv-users-panel.h"
#include "../backend/sbv-users-backend.h"
#include "../model/sbv-user.h"

#include <adwaita.h>
#include <string.h>

/* Windows FILETIME epoch offset in seconds */
#define WIN_EPOCH_OFFSET 11644473600LL
#define WIN_TICKS_PER_SEC 10000000LL

struct _SbvUsersPanel {
  GtkBox         parent;

  /* ── List pane ── */
  GtkWidget     *outer_stack;    /* "loading" | "empty" | "split" */
  GtkWidget     *search_entry;
  GtkWidget     *list_box;
  GtkWidget     *spinner;

  /* ── Detail pane ── */
  GtkWidget     *detail_stack;   /* "none" | "detail" */

  /* Identity */
  GtkWidget     *cn_entry;
  GtkWidget     *display_entry;
  GtkWidget     *given_entry;
  GtkWidget     *sn_entry;
  GtkWidget     *sync_parts_from_full; /* update givenName/sn from cn/displayName */
  GtkWidget     *sync_full_from_parts; /* update cn/displayName from givenName+sn */

  /* Contact */
  GtkWidget     *email_entry;

  /* Account */
  GtkWidget     *dn_label;
  GtkWidget     *sam_label;
  GtkWidget     *enabled_check;

  /* RFC2307 / POSIX (read-only) */
  GtkWidget     *rfc_uid_label;
  GtkWidget     *rfc_gid_label;
  GtkWidget     *rfc_shell_label;
  GtkWidget     *rfc_home_label;
  GtkWidget     *rfc_gecos_label;

  /* Save attributes */
  GtkWidget     *save_btn;
  GtkWidget     *save_error;

  /* Password */
  GtkWidget     *new_pw_entry;
  GtkWidget     *confirm_pw_entry;
  GtkWidget     *set_pw_btn;
  GtkWidget     *pw_error;

  /* Policy */
  GtkWidget     *force_change_check;
  GtkWidget     *never_expires_check;
  GtkWidget     *acct_never_expires_check;
  GtkWidget     *expires_date_entry;
  GtkWidget     *save_policy_btn;
  GtkWidget     *policy_error;

  /* State */
  SbvConnection *conn;           /* unowned */
  SbvUser       *selected_user;  /* owned ref */
  char          *filter_text;
};

G_DEFINE_TYPE (SbvUsersPanel, sbv_users_panel, GTK_TYPE_BOX)

/* ── Helpers ────────────────────────────────────────────────────────────── */

static gint64
date_entry_to_filetime (const char *text)
{
  /* Expects YYYY-MM-DD */
  int y, m, d;
  if (sscanf (text, "%d-%d-%d", &y, &m, &d) != 3)
    return G_MAXINT64;
  GDateTime *dt = g_date_time_new_utc (y, m, d, 0, 0, 0.0);
  if (!dt) return G_MAXINT64;
  gint64 unix_ts  = g_date_time_to_unix (dt);
  gint64 win_time = (unix_ts + WIN_EPOCH_OFFSET) * WIN_TICKS_PER_SEC;
  g_date_time_unref (dt);
  return win_time;
}

/* ── Row construction ───────────────────────────────────────────────────── */

static GtkWidget *
make_user_row (SbvUser *user)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-user",
                           g_object_ref (user), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top    (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *icon = gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text_box, TRUE);

  /* Prefer displayName, fall back to cn, then sam */
  const char *display = sbv_user_get_display_name (user);
  if (!display || !*display) display = sbv_user_get_cn (user);
  const char *sam     = sbv_user_get_sam (user);

  GtkWidget *name_label = gtk_label_new (display ? display : sam);
  gtk_label_set_xalign (GTK_LABEL (name_label), 0);
  gtk_widget_add_css_class (name_label, "heading");
  gtk_box_append (GTK_BOX (text_box), name_label);

  if (sam) {
    GtkWidget *sam_label = gtk_label_new (sam);
    gtk_label_set_xalign (GTK_LABEL (sam_label), 0);
    gtk_widget_add_css_class (sam_label, "dim-label");
    gtk_box_append (GTK_BOX (text_box), sam_label);
  }

  gtk_box_append (GTK_BOX (box), text_box);

  gboolean   enabled = sbv_user_get_enabled (user);
  GtkWidget *status  = gtk_label_new (enabled ? "Enabled" : "Disabled");
  gtk_widget_add_css_class (status, enabled ? "success" : "error");
  gtk_box_append (GTK_BOX (box), status);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* ── Filter ─────────────────────────────────────────────────────────────── */

static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->filter_text || !*self->filter_text)
    return TRUE;

  SbvUser *user = g_object_get_data (G_OBJECT (row), "sbv-user");
  if (!user) return TRUE;

  const char *fields[] = {
    sbv_user_get_display_name (user),
    sbv_user_get_cn (user),
    sbv_user_get_sam (user),
    sbv_user_get_email (user),
    NULL
  };

  for (int i = 0; fields[i]; i++) {
    if (!fields[i]) continue;
    char *lower = g_utf8_strdown (fields[i], -1);
    gboolean match = strstr (lower, self->filter_text) != NULL;
    g_free (lower);
    if (match) return TRUE;
  }
  return FALSE;
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  g_free (self->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  self->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* ── Load user into detail form ─────────────────────────────────────────── */

static void
load_user_into_detail (SbvUsersPanel *self, SbvUser *user)
{
  g_set_object (&self->selected_user, user);

#define SET_ENTRY(w, getter) \
  gtk_editable_set_text (GTK_EDITABLE (self->w), \
                         getter (user) ? getter (user) : "");

  SET_ENTRY (cn_entry,      sbv_user_get_cn)
  SET_ENTRY (display_entry, sbv_user_get_display_name)
  SET_ENTRY (given_entry,   sbv_user_get_given_name)
  SET_ENTRY (sn_entry,      sbv_user_get_sn)
  SET_ENTRY (email_entry,   sbv_user_get_email)

#undef SET_ENTRY

  gtk_label_set_text (GTK_LABEL (self->dn_label),
                      sbv_user_get_dn (user) ? sbv_user_get_dn (user) : "");
  gtk_label_set_text (GTK_LABEL (self->sam_label),
                      sbv_user_get_sam (user) ? sbv_user_get_sam (user) : "");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->enabled_check),
                                sbv_user_get_enabled (user));

  /* Clear password fields */
  gtk_editable_set_text (GTK_EDITABLE (self->new_pw_entry),     "");
  gtk_editable_set_text (GTK_EDITABLE (self->confirm_pw_entry), "");

  /* Policy */
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->force_change_check),
                                sbv_user_get_pwd_last_set (user) == 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->never_expires_check),
                                (sbv_user_get_uac (user) & 0x10000) != 0);

  gint64 expires = sbv_user_get_account_expires (user);
  gboolean acct_never = (expires == G_MAXINT64 || expires == 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->acct_never_expires_check),
                                acct_never);
  if (!acct_never) {
    gint64     unix_ts = (expires / WIN_TICKS_PER_SEC) - WIN_EPOCH_OFFSET;
    GDateTime *dt      = g_date_time_new_from_unix_utc (unix_ts);
    if (dt) {
      char *ds = g_date_time_format (dt, "%Y-%m-%d");
      gtk_editable_set_text (GTK_EDITABLE (self->expires_date_entry), ds);
      g_free (ds);
      g_date_time_unref (dt);
    }
  } else {
    gtk_editable_set_text (GTK_EDITABLE (self->expires_date_entry), "");
  }
  gtk_widget_set_sensitive (self->expires_date_entry, !acct_never);

  /* RFC2307 / POSIX */
  gint uid = sbv_user_get_uid_number (user);
  gint gid = sbv_user_get_gid_number (user);
  char *uid_str = (uid >= 0) ? g_strdup_printf ("%d", uid) : g_strdup ("\xe2\x80\x94");
  char *gid_str = (gid >= 0) ? g_strdup_printf ("%d", gid) : g_strdup ("\xe2\x80\x94");
  gtk_label_set_text (GTK_LABEL (self->rfc_uid_label),   uid_str);
  gtk_label_set_text (GTK_LABEL (self->rfc_gid_label),   gid_str);
  g_free (uid_str);
  g_free (gid_str);

  const char *shell = sbv_user_get_login_shell (user);
  const char *home  = sbv_user_get_home_dir    (user);
  const char *gecos = sbv_user_get_gecos       (user);
  gtk_label_set_text (GTK_LABEL (self->rfc_shell_label), shell ? shell : "\xe2\x80\x94");
  gtk_label_set_text (GTK_LABEL (self->rfc_home_label),  home  ? home  : "\xe2\x80\x94");
  gtk_label_set_text (GTK_LABEL (self->rfc_gecos_label), gecos ? gecos : "\xe2\x80\x94");

  /* Clear error labels */
  gtk_label_set_text (GTK_LABEL (self->save_error),   "");
  gtk_label_set_text (GTK_LABEL (self->pw_error),     "");
  gtk_label_set_text (GTK_LABEL (self->policy_error), "");
  gtk_widget_set_visible (self->save_error,   FALSE);
  gtk_widget_set_visible (self->pw_error,     FALSE);
  gtk_widget_set_visible (self->policy_error, FALSE);

  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "detail");
}

/* ── Row activated ──────────────────────────────────────────────────────── */

static void
on_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);

  if (!row) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    return;
  }

  SbvUser *user = g_object_get_data (G_OBJECT (row), "sbv-user");
  if (user)
    load_user_into_detail (self, user);
}

/* ── "Account never expires" toggled ────────────────────────────────────── */

static void
on_acct_never_expires_toggled (GtkCheckButton *btn, gpointer user_data)
{
  SbvUsersPanel *self  = SBV_USERS_PANEL (user_data);
  gboolean       never = gtk_check_button_get_active (btn);
  gtk_widget_set_sensitive (self->expires_date_entry, !never);
}

/* ── Save attributes ────────────────────────────────────────────────────── */

typedef struct {
  SbvUsersPanel *panel;
  gboolean       need_enabled_change;
  gboolean       new_enabled;
} SaveCtx;

static void
on_set_enabled_after_save (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SaveCtx       *ctx  = user_data;
  SbvUsersPanel *self = ctx->panel;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->save_btn, TRUE);

  if (!sbv_users_set_enabled_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->save_error), err->message);
    gtk_widget_set_visible (self->save_error, TRUE);
    g_error_free (err);
  } else {
    /* Update model */
    if (self->selected_user)
      sbv_user_set_enabled (self->selected_user, ctx->new_enabled);
    /* Refresh list row */
    sbv_users_panel_load (self, conn);
  }
  g_free (ctx);
}

static void
on_update_attrs_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SaveCtx       *ctx  = user_data;
  SbvUsersPanel *self = ctx->panel;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  if (!sbv_users_update_attrs_finish (conn, result, &err)) {
    gtk_widget_set_sensitive (self->save_btn, TRUE);
    gtk_label_set_text (GTK_LABEL (self->save_error), err->message);
    gtk_widget_set_visible (self->save_error, TRUE);
    g_error_free (err);
    g_free (ctx);
    return;
  }

  /* Update model with new values */
  if (self->selected_user) {
    sbv_user_set_cn           (self->selected_user,
                                gtk_editable_get_text (GTK_EDITABLE (self->cn_entry)));
    sbv_user_set_display_name (self->selected_user,
                                gtk_editable_get_text (GTK_EDITABLE (self->display_entry)));
    sbv_user_set_given_name   (self->selected_user,
                                gtk_editable_get_text (GTK_EDITABLE (self->given_entry)));
    sbv_user_set_sn           (self->selected_user,
                                gtk_editable_get_text (GTK_EDITABLE (self->sn_entry)));
    sbv_user_set_email        (self->selected_user,
                                gtk_editable_get_text (GTK_EDITABLE (self->email_entry)));
  }

  if (ctx->need_enabled_change) {
    sbv_users_set_enabled_async (conn, self->selected_user, ctx->new_enabled,
                                  NULL, on_set_enabled_after_save, ctx);
  } else {
    gtk_widget_set_sensitive (self->save_btn, TRUE);
    sbv_users_panel_load (self, conn);
    g_free (ctx);
  }
}

static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->selected_user || !self->conn) return;

  gtk_widget_set_visible (self->save_error, FALSE);
  gtk_widget_set_sensitive (self->save_btn, FALSE);

  const char *cn_text      = gtk_editable_get_text (GTK_EDITABLE (self->cn_entry));
  const char *display_text = gtk_editable_get_text (GTK_EDITABLE (self->display_entry));
  const char *given_text   = gtk_editable_get_text (GTK_EDITABLE (self->given_entry));
  const char *sn_text      = gtk_editable_get_text (GTK_EDITABLE (self->sn_entry));
  const char *email_text   = gtk_editable_get_text (GTK_EDITABLE (self->email_entry));

  /* Apply sync rules */
  char *cn_final      = g_strdup (cn_text);
  char *display_final = g_strdup (display_text);
  char *given_final   = g_strdup (given_text);
  char *sn_final      = g_strdup (sn_text);

  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (self->sync_full_from_parts)) &&
      (*given_final || *sn_final)) {
    g_free (cn_final);
    g_free (display_final);
    cn_final      = g_strdup_printf ("%s %s", given_final, sn_final);
    display_final = g_strdup (cn_final);
    gtk_editable_set_text (GTK_EDITABLE (self->cn_entry),      cn_final);
    gtk_editable_set_text (GTK_EDITABLE (self->display_entry), display_final);
  } else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (self->sync_parts_from_full)) &&
             *cn_final) {
    /* Split cn at first space */
    const char *sp = strchr (cn_final, ' ');
    g_free (given_final);
    g_free (sn_final);
    if (sp) {
      given_final = g_strndup (cn_final, sp - cn_final);
      sn_final    = g_strdup (sp + 1);
    } else {
      given_final = g_strdup (cn_final);
      sn_final    = g_strdup ("");
    }
    gtk_editable_set_text (GTK_EDITABLE (self->given_entry), given_final);
    gtk_editable_set_text (GTK_EDITABLE (self->sn_entry),    sn_final);
  }

  gboolean new_enabled = gtk_check_button_get_active (
    GTK_CHECK_BUTTON (self->enabled_check));
  gboolean enabled_changed = (new_enabled != sbv_user_get_enabled (self->selected_user));

  SaveCtx *ctx = g_new0 (SaveCtx, 1);
  ctx->panel               = self;
  ctx->need_enabled_change = enabled_changed;
  ctx->new_enabled         = new_enabled;

  sbv_users_update_attrs_async (self->conn, self->selected_user,
                                 cn_final, display_final,
                                 given_final, sn_final, email_text,
                                 NULL, on_update_attrs_done, ctx);
  g_free (cn_final);
  g_free (display_final);
  g_free (given_final);
  g_free (sn_final);
}

/* ── Set password ───────────────────────────────────────────────────────── */

static void
on_reset_pw_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->set_pw_btn, TRUE);

  if (!sbv_users_reset_password_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->pw_error), err->message);
    gtk_widget_set_visible (self->pw_error, TRUE);
    g_error_free (err);
  } else {
    gtk_editable_set_text (GTK_EDITABLE (self->new_pw_entry),     "");
    gtk_editable_set_text (GTK_EDITABLE (self->confirm_pw_entry), "");
  }
}

static void
on_set_pw_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->selected_user || !self->conn) return;

  const char *pw  = gtk_editable_get_text (GTK_EDITABLE (self->new_pw_entry));
  const char *pw2 = gtk_editable_get_text (GTK_EDITABLE (self->confirm_pw_entry));

  gtk_widget_set_visible (self->pw_error, FALSE);

  if (!*pw) {
    gtk_label_set_text (GTK_LABEL (self->pw_error), "Enter a new password.");
    gtk_widget_set_visible (self->pw_error, TRUE);
    return;
  }
  if (!g_str_equal (pw, pw2)) {
    gtk_label_set_text (GTK_LABEL (self->pw_error), "Passwords do not match.");
    gtk_widget_set_visible (self->pw_error, TRUE);
    return;
  }

  gtk_widget_set_sensitive (self->set_pw_btn, FALSE);
  sbv_users_reset_password_async (self->conn, self->selected_user, pw,
                                   NULL, on_reset_pw_done, self);
}

/* ── Save policy ────────────────────────────────────────────────────────── */

static void
on_save_policy_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->save_policy_btn, TRUE);

  if (!sbv_users_set_password_flags_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->policy_error), err->message);
    gtk_widget_set_visible (self->policy_error, TRUE);
    g_error_free (err);
  } else {
    /* Update model */
    if (self->selected_user) {
      gboolean fc = gtk_check_button_get_active (
        GTK_CHECK_BUTTON (self->force_change_check));
      sbv_user_set_pwd_last_set (self->selected_user, fc ? 0 : 1);

      gboolean ne = gtk_check_button_get_active (
        GTK_CHECK_BUTTON (self->never_expires_check));
      gint64 uac = sbv_user_get_uac (self->selected_user);
      if (ne) uac |= 0x10000; else uac &= ~(gint64)0x10000;
      sbv_user_set_uac (self->selected_user, uac);
    }
  }
}

static void
on_save_policy_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->selected_user || !self->conn) return;

  gtk_widget_set_visible (self->policy_error, FALSE);
  gtk_widget_set_sensitive (self->save_policy_btn, FALSE);

  gboolean force_change  = gtk_check_button_get_active (
    GTK_CHECK_BUTTON (self->force_change_check));
  gboolean never_expires = gtk_check_button_get_active (
    GTK_CHECK_BUTTON (self->never_expires_check));
  gboolean acct_never    = gtk_check_button_get_active (
    GTK_CHECK_BUTTON (self->acct_never_expires_check));

  gint64 account_expires;
  if (acct_never) {
    account_expires = G_MAXINT64;
  } else {
    const char *ds = gtk_editable_get_text (
      GTK_EDITABLE (self->expires_date_entry));
    account_expires = date_entry_to_filetime (ds);
    if (account_expires == G_MAXINT64 && *ds) {
      gtk_label_set_text (GTK_LABEL (self->policy_error),
                          "Invalid date. Use YYYY-MM-DD.");
      gtk_widget_set_visible (self->policy_error, TRUE);
      gtk_widget_set_sensitive (self->save_policy_btn, TRUE);
      return;
    }
  }

  sbv_users_set_password_flags_async (self->conn, self->selected_user,
                                       force_change, never_expires, account_expires,
                                       NULL, on_save_policy_done, self);
}

/* ── Load callback ──────────────────────────────────────────────────────── */

static void
on_users_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  GError        *err  = NULL;

  GListStore *store = sbv_users_list_finish (SBV_CONNECTION (source), result, &err);
  if (!store) {
    g_warning ("Failed to load users: %s", err->message);
    g_error_free (err);
    gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "empty");
    return;
  }

  /* Preserve selected sam for re-selection */
  char *selected_sam = NULL;
  if (self->selected_user)
    selected_sam = g_strdup (sbv_user_get_sam (self->selected_user));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  GtkListBoxRow *reselect = NULL;

  for (guint i = 0; i < n; i++) {
    SbvUser   *user = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget *row  = make_user_row (user);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
    if (selected_sam && g_str_equal (sbv_user_get_sam (user) ?: "", selected_sam))
      reselect = GTK_LIST_BOX_ROW (row);
    g_object_unref (user);
  }

  g_object_unref (store);
  g_free (selected_sam);

  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack),
                                     n == 0 ? "empty" : "split");

  if (reselect)
    gtk_list_box_select_row (GTK_LIST_BOX (self->list_box), reselect);
  else
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void
sbv_users_panel_load (SbvUsersPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "loading");
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  sbv_users_list_async (conn, NULL, on_users_loaded, self);
}

/* ── Helper: make a form section label ──────────────────────────────────── */

static GtkWidget *
make_section_label (const char *title)
{
  GtkWidget *lbl = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (lbl), 0);
  gtk_widget_add_css_class (lbl, "heading");
  gtk_widget_set_margin_top (lbl, 12);
  return lbl;
}

static GtkWidget *
make_field_row (const char *label_text, GtkWidget *widget)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lbl = gtk_label_new (label_text);
  gtk_label_set_xalign (GTK_LABEL (lbl), 1);
  gtk_widget_set_size_request (lbl, 140, -1);
  gtk_widget_add_css_class (lbl, "dim-label");
  gtk_box_append (GTK_BOX (box), lbl);
  gtk_widget_set_hexpand (widget, TRUE);
  gtk_box_append (GTK_BOX (box), widget);
  return box;
}

/* ── GObject init ───────────────────────────────────────────────────────── */

static void
sbv_users_panel_finalize (GObject *object)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (object);
  g_free (self->filter_text);
  g_clear_object (&self->selected_user);
  G_OBJECT_CLASS (sbv_users_panel_parent_class)->finalize (object);
}

static void sbv_users_panel_class_init (SbvUsersPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_users_panel_finalize;
}

static void
sbv_users_panel_init (SbvUsersPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  /* ── Outer stack ── */
  self->outer_stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->outer_stack, TRUE);

  /* Loading */
  {
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    self->spinner = gtk_spinner_new ();
    gtk_box_append (GTK_BOX (box), self->spinner);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), box, "loading");
  }

  /* Empty */
  {
    GtkWidget *empty = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty),
                                    "system-users-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Users Found");
    gtk_stack_add_named (GTK_STACK (self->outer_stack), empty, "empty");
  }

  /* Split view: list left, detail right */
  {
    GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);

    /* ── Left: search + list ── */
    GtkWidget *left_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top    (toolbar, 6);
    gtk_widget_set_margin_bottom (toolbar, 6);
    gtk_widget_set_margin_start  (toolbar, 8);
    gtk_widget_set_margin_end    (toolbar, 8);

    self->search_entry = gtk_search_entry_new ();
    gtk_widget_set_hexpand (self->search_entry, TRUE);
    gtk_widget_set_tooltip_text (self->search_entry, "Search users");
    g_signal_connect (self->search_entry, "search-changed",
                      G_CALLBACK (on_search_changed), self);
    gtk_box_append (GTK_BOX (toolbar), self->search_entry);

    gtk_box_append (GTK_BOX (left_box), toolbar);
    gtk_box_append (GTK_BOX (left_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *list_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (list_scroll, TRUE);
    gtk_widget_set_size_request (list_scroll, 240, -1);

    self->list_box = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list_box),
                                      GTK_SELECTION_SINGLE);
    gtk_list_box_set_filter_func (GTK_LIST_BOX (self->list_box),
                                   filter_func, self, NULL);
    gtk_widget_add_css_class (self->list_box, "boxed-list");
    g_signal_connect (self->list_box, "row-selected",
                      G_CALLBACK (on_row_selected), self);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (list_scroll),
                                    self->list_box);
    gtk_box_append (GTK_BOX (left_box), list_scroll);

    gtk_paned_set_start_child (GTK_PANED (paned), left_box);

    /* ── Right: detail stack ── */
    self->detail_stack = gtk_stack_new ();
    gtk_widget_set_hexpand (self->detail_stack, TRUE);

    /* "none" page */
    GtkWidget *none_page = adw_status_page_new ();
    adw_status_page_set_title (ADW_STATUS_PAGE (none_page), "Select a User");
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (none_page),
                                    "avatar-default-symbolic");
    gtk_stack_add_named (GTK_STACK (self->detail_stack), none_page, "none");

    /* "detail" page */
    GtkWidget *detail_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (detail_scroll, TRUE);

    GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top    (form, 16);
    gtk_widget_set_margin_bottom (form, 24);
    gtk_widget_set_margin_start  (form, 20);
    gtk_widget_set_margin_end    (form, 20);

    /* ── Identity ── */
    gtk_box_append (GTK_BOX (form), make_section_label ("Identity"));

    self->cn_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->cn_entry), "Full Name");
    gtk_box_append (GTK_BOX (form), make_field_row ("Full Name (cn)", self->cn_entry));

    self->display_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->display_entry), "Display Name");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Display Name", self->display_entry));

    self->sync_parts_from_full = gtk_check_button_new_with_label (
      "Update First/Last Name from Full Name on save");
    gtk_widget_set_margin_start (self->sync_parts_from_full, 148);
    gtk_box_append (GTK_BOX (form), self->sync_parts_from_full);

    self->given_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->given_entry), "First Name");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("First Name", self->given_entry));

    self->sn_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->sn_entry), "Last Name");
    gtk_box_append (GTK_BOX (form), make_field_row ("Last Name", self->sn_entry));

    self->sync_full_from_parts = gtk_check_button_new_with_label (
      "Update Full Name & Display Name from First/Last on save");
    gtk_widget_set_margin_start (self->sync_full_from_parts, 148);
    gtk_box_append (GTK_BOX (form), self->sync_full_from_parts);

    /* ── Contact ── */
    gtk_box_append (GTK_BOX (form), make_section_label ("Contact"));

    self->email_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->email_entry), "Email address");
    gtk_box_append (GTK_BOX (form), make_field_row ("Email", self->email_entry));

    /* ── Account ── */
    gtk_box_append (GTK_BOX (form), make_section_label ("Account"));

    self->dn_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->dn_label), 0);
    gtk_label_set_ellipsize (GTK_LABEL (self->dn_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable (GTK_LABEL (self->dn_label), TRUE);
    gtk_widget_add_css_class (self->dn_label, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("DN", self->dn_label));

    self->sam_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->sam_label), 0);
    gtk_widget_add_css_class (self->sam_label, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Username", self->sam_label));

    self->enabled_check = gtk_check_button_new_with_label ("Account enabled");
    gtk_widget_set_margin_start (self->enabled_check, 148);
    gtk_box_append (GTK_BOX (form), self->enabled_check);

    /* Save button + error */
    GtkWidget *save_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top (save_box, 8);
    self->save_btn = gtk_button_new_with_label ("Save Changes");
    gtk_widget_add_css_class (self->save_btn, "suggested-action");
    gtk_widget_set_halign (self->save_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_start (self->save_btn, 148);
    g_signal_connect (self->save_btn, "clicked",
                      G_CALLBACK (on_save_clicked), self);
    gtk_box_append (GTK_BOX (save_box), self->save_btn);

    self->save_error = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->save_error), 0);
    gtk_label_set_wrap (GTK_LABEL (self->save_error), TRUE);
    gtk_widget_add_css_class (self->save_error, "error");
    gtk_widget_set_margin_start (self->save_error, 148);
    gtk_widget_set_visible (self->save_error, FALSE);
    gtk_box_append (GTK_BOX (save_box), self->save_error);
    gtk_box_append (GTK_BOX (form), save_box);

    /* ── Password ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Set Password"));

    self->new_pw_entry = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (self->new_pw_entry), FALSE);
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->new_pw_entry), "New password");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("New Password", self->new_pw_entry));

    self->confirm_pw_entry = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (self->confirm_pw_entry), FALSE);
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->confirm_pw_entry),
                                     "Confirm password");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Confirm", self->confirm_pw_entry));

    GtkWidget *pw_btn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top (pw_btn_box, 4);
    self->set_pw_btn = gtk_button_new_with_label ("Set Password");
    gtk_widget_add_css_class (self->set_pw_btn, "suggested-action");
    gtk_widget_set_halign (self->set_pw_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_start (self->set_pw_btn, 148);
    g_signal_connect (self->set_pw_btn, "clicked",
                      G_CALLBACK (on_set_pw_clicked), self);
    gtk_box_append (GTK_BOX (pw_btn_box), self->set_pw_btn);

    self->pw_error = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->pw_error), 0);
    gtk_label_set_wrap (GTK_LABEL (self->pw_error), TRUE);
    gtk_widget_add_css_class (self->pw_error, "error");
    gtk_widget_set_margin_start (self->pw_error, 148);
    gtk_widget_set_visible (self->pw_error, FALSE);
    gtk_box_append (GTK_BOX (pw_btn_box), self->pw_error);
    gtk_box_append (GTK_BOX (form), pw_btn_box);

    /* ── Account Policy ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Account Policy"));

    self->force_change_check = gtk_check_button_new_with_label (
      "Must change password at next logon");
    gtk_widget_set_margin_start (self->force_change_check, 148);
    gtk_box_append (GTK_BOX (form), self->force_change_check);

    self->never_expires_check = gtk_check_button_new_with_label (
      "Password never expires");
    gtk_widget_set_margin_start (self->never_expires_check, 148);
    gtk_box_append (GTK_BOX (form), self->never_expires_check);

    self->acct_never_expires_check = gtk_check_button_new_with_label (
      "Account never expires");
    gtk_widget_set_margin_start (self->acct_never_expires_check, 148);
    g_signal_connect (self->acct_never_expires_check, "toggled",
                      G_CALLBACK (on_acct_never_expires_toggled), self);
    gtk_box_append (GTK_BOX (form), self->acct_never_expires_check);

    self->expires_date_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->expires_date_entry),
                                     "YYYY-MM-DD");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Expires on", self->expires_date_entry));

    GtkWidget *policy_btn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top (policy_btn_box, 4);
    self->save_policy_btn = gtk_button_new_with_label ("Save Policy");
    gtk_widget_add_css_class (self->save_policy_btn, "suggested-action");
    gtk_widget_set_halign (self->save_policy_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_start (self->save_policy_btn, 148);
    g_signal_connect (self->save_policy_btn, "clicked",
                      G_CALLBACK (on_save_policy_clicked), self);
    gtk_box_append (GTK_BOX (policy_btn_box), self->save_policy_btn);

    self->policy_error = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->policy_error), 0);
    gtk_label_set_wrap (GTK_LABEL (self->policy_error), TRUE);
    gtk_widget_add_css_class (self->policy_error, "error");
    gtk_widget_set_margin_start (self->policy_error, 148);
    gtk_widget_set_visible (self->policy_error, FALSE);
    gtk_box_append (GTK_BOX (policy_btn_box), self->policy_error);
    gtk_box_append (GTK_BOX (form), policy_btn_box);

    /* ── Unix Attributes (RFC2307) ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Unix Attributes (RFC2307)"));

    self->rfc_uid_label = gtk_label_new ("\xe2\x80\x94");
    gtk_label_set_xalign (GTK_LABEL (self->rfc_uid_label), 0);
    gtk_widget_add_css_class (self->rfc_uid_label, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("UID Number", self->rfc_uid_label));

    self->rfc_gid_label = gtk_label_new ("\xe2\x80\x94");
    gtk_label_set_xalign (GTK_LABEL (self->rfc_gid_label), 0);
    gtk_widget_add_css_class (self->rfc_gid_label, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("GID Number", self->rfc_gid_label));

    self->rfc_shell_label = gtk_label_new ("\xe2\x80\x94");
    gtk_label_set_xalign (GTK_LABEL (self->rfc_shell_label), 0);
    gtk_widget_add_css_class (self->rfc_shell_label, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Login Shell", self->rfc_shell_label));

    self->rfc_home_label = gtk_label_new ("\xe2\x80\x94");
    gtk_label_set_xalign (GTK_LABEL (self->rfc_home_label), 0);
    gtk_label_set_ellipsize (GTK_LABEL (self->rfc_home_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class (self->rfc_home_label, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Home Directory", self->rfc_home_label));

    self->rfc_gecos_label = gtk_label_new ("\xe2\x80\x94");
    gtk_label_set_xalign (GTK_LABEL (self->rfc_gecos_label), 0);
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("GECOS", self->rfc_gecos_label));

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (detail_scroll), form);
    gtk_stack_add_named (GTK_STACK (self->detail_stack), detail_scroll, "detail");

    gtk_paned_set_end_child (GTK_PANED (paned), self->detail_stack);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), paned, "split");
  }

  gtk_box_append (GTK_BOX (self), self->outer_stack);
}

GtkWidget *
sbv_users_panel_new (void)
{
  return g_object_new (SBV_TYPE_USERS_PANEL, NULL);
}
