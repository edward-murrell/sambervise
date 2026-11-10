#include "sbv-users-panel.h"
#include "sbv-move-dialog.h"
#include "sbv-member-of.h"
#include "sbv-create-user-dialog.h"
#include "../backend/sbv-users-backend.h"
#include "../backend/sbv-collisions.h"
#include "../backend/sbv-idmap-hints.h"
#include "../model/sbv-user.h"
#include "../model/sbv-search.h"

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
  GtkWidget     *add_btn;
  GtkWidget     *disabled_toggle;  /* show only disabled accounts */
  GtkWidget     *locked_toggle;    /* show only locked-out accounts */
  GtkWidget     *expired_toggle;   /* show only expired passwords */
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
  GtkWidget     *lockout_label;
  GtkWidget     *unlock_btn;
  GtkWidget     *unlock_error;

  /* Windows profile (editable) */
  GtkWidget     *prof_path_entry;
  GtkWidget     *prof_script_entry;
  GtkWidget     *prof_home_entry;
  GtkWidget     *prof_drive_entry;
  GtkWidget     *prof_save_btn;
  GtkWidget     *prof_error;

  /* RFC2307 / POSIX (editable) */
  GtkWidget     *rfc_uid_entry;
  GtkWidget     *rfc_gid_entry;
  GtkWidget     *rfc_shell_entry;
  GtkWidget     *rfc_home_entry;
  GtkWidget     *rfc_gecos_entry;
  GtkWidget     *rfc_save_btn;
  GtkWidget     *rfc_error;
  GtkWidget     *rfc_uid_collision; /* warning under uid entry */
  GtkWidget     *rfc_gid_collision; /* warning under gid entry */
  guint          uid_check_timer;   /* g_timeout_add id for uid debounce */
  guint          gid_check_timer;   /* g_timeout_add id for gid debounce */
  GtkWidget     *rfc_uid_suggest_btn;
  GtkWidget     *rfc_gid_suggest_btn;
  /* Cached idmap hints for the active connection. Loaded lazily on the
   * first "Suggest" press; cleared by sbv_users_panel_load. */
  SbvIdmapHints *idmap_hints;

  /* Raw LDAP attributes (dynamic, rebuilt on selection) */
  GtkWidget     *raw_attrs_box;

  /* Paned split — position initialised on first map */
  GtkWidget     *paned;
  gboolean       paned_init;

  /* Save attributes */
  GtkWidget     *save_btn;
  GtkWidget     *save_error;

  /* Delete */
  GtkWidget     *delete_btn;
  GtkWidget     *delete_error;

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

  /* Member Of */
  GtkWidget     *member_of;

  /* State */
  SbvConnection *conn;           /* unowned */
  SbvUser       *selected_user;  /* owned ref */
  char          *filter_text;
  /* When non-NULL the next reload re-selects the row whose DN matches this
   * string (and then clears it). Used by the create flow to highlight the
   * just-created user; we match by DN rather than sam because the create
   * dialog allows free-form cn so DN is the most reliable identifier here.
   * For a normal reload (after save) this stays NULL and the loader falls
   * back to matching selected_user's sam. */
  char          *pending_select_dn;
};

G_DEFINE_TYPE (SbvUsersPanel, sbv_users_panel, GTK_TYPE_BOX)

enum { SIGNAL_MEMBERSHIP_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

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

/* ── Forward declarations ───────────────────────────────────────────────── */
static void populate_raw_attrs  (GtkWidget *box, GHashTable *attrs, int label_width);
void        sbv_users_panel_load (SbvUsersPanel *self, SbvConnection *conn);

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

  if (sbv_user_get_locked_out (user)) {
    GtkWidget *locked = gtk_label_new ("Locked");
    gtk_widget_add_css_class (locked, "warning");
    gtk_box_append (GTK_BOX (box), locked);
  }

  if (sbv_user_get_password_expired (user)) {
    GtkWidget *expired = gtk_label_new ("Expired");
    gtk_widget_add_css_class (expired, "warning");
    gtk_widget_set_tooltip_text (expired, "Password has expired");
    gtk_box_append (GTK_BOX (box), expired);
  }

  gboolean   enabled = sbv_user_get_enabled (user);
  GtkWidget *status  = gtk_label_new (enabled ? "Enabled" : "Disabled");
  gtk_widget_add_css_class (status, enabled ? "success" : "error");
  gtk_box_append (GTK_BOX (box), status);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* ── Lockout ────────────────────────────────────────────────────────────── */

/* Formats a FILETIME as local "YYYY-MM-DD HH:MM". Caller frees. */
static char *
format_filetime (gint64 filetime)
{
  gint64     unix_ts = (filetime / WIN_TICKS_PER_SEC) - WIN_EPOCH_OFFSET;
  GDateTime *dt      = g_date_time_new_from_unix_local (unix_ts);
  if (!dt) return g_strdup ("?");
  char *s = g_date_time_format (dt, "%Y-%m-%d %H:%M");
  g_date_time_unref (dt);
  return s;
}

/* Shows the user's lockout state, and the Unlock button only when the
 * account is locked. */
static void
update_lockout_display (SbvUsersPanel *self, SbvUser *user)
{
  gtk_widget_set_visible (self->unlock_error, FALSE);
  gtk_widget_set_sensitive (self->unlock_btn, TRUE);

  if (!sbv_user_get_locked_out (user)) {
    gtk_label_set_text (GTK_LABEL (self->lockout_label), "Not locked out");
    gtk_widget_remove_css_class (self->lockout_label, "warning");
    gtk_widget_set_visible (self->unlock_btn, FALSE);
    return;
  }

  gint64 since = sbv_user_get_lockout_time (user);
  gint64 end   = sbv_user_get_lockout_end (user);
  GString *msg = g_string_new ("Locked out");
  if (since > 0) {
    char *s = format_filetime (since);
    g_string_append_printf (msg, " since %s", s);
    g_free (s);
  }
  g_string_append (msg, ".");
  if (end == G_MAXINT64) {
    g_string_append (msg, " Stays locked until unlocked.");
  } else if (end > 0) {
    char *s = format_filetime (end);
    g_string_append_printf (msg, " Unlocks at %s.", s);
    g_free (s);
  }
  gtk_label_set_text (GTK_LABEL (self->lockout_label), msg->str);
  gtk_widget_add_css_class (self->lockout_label, "warning");
  gtk_widget_set_visible (self->unlock_btn, TRUE);
  g_string_free (msg, TRUE);
}

/* Unlock completion: show the error, or reload so the list and pane show
 * the cleared lockout. */
static void
on_unlock_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->unlock_btn, TRUE);
  if (!sbv_users_unlock_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->unlock_error), err->message);
    gtk_widget_set_visible (self->unlock_error, TRUE);
    g_error_free (err);
    return;
  }
  sbv_users_panel_load (self, conn);
}

/* "Unlock" button: clears the lockout straight away. No Save needed. */
static void
on_unlock_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->selected_user || !self->conn) return;
  gtk_widget_set_visible (self->unlock_error, FALSE);
  gtk_widget_set_sensitive (self->unlock_btn, FALSE);
  sbv_users_unlock_async (self->conn, self->selected_user, NULL,
                          on_unlock_done, self);
}

/* ── Filter ─────────────────────────────────────────────────────────────── */

/* Returns TRUE when a toggle is pressed. */
static gboolean
toggle_on (GtkWidget *toggle)
{
  return gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (toggle));
}

/* Shows a row when it matches the search text and every active toggle.
 * Each toggle narrows the list further. */
static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  SbvUser       *user = g_object_get_data (G_OBJECT (row), "sbv-user");
  if (!user) return TRUE;

  if (toggle_on (self->disabled_toggle) && sbv_user_get_enabled (user))
    return FALSE;
  if (toggle_on (self->locked_toggle) && !sbv_user_get_locked_out (user))
    return FALSE;
  if (toggle_on (self->expired_toggle) && !sbv_user_get_password_expired (user))
    return FALSE;

  const char *fields[] = {
    sbv_user_get_display_name (user),
    sbv_user_get_cn (user),
    sbv_user_get_sam (user),
    sbv_user_get_email (user),
  };
  return sbv_search_matches (self->filter_text, fields, G_N_ELEMENTS (fields));
}

/* Re-runs the list filter when the search entry changes. */
static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  g_free (self->filter_text);
  self->filter_text = sbv_search_fold (gtk_editable_get_text (GTK_EDITABLE (entry)));
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* Re-runs the list filter when a filter toggle changes. */
static void
on_filter_toggled (GtkToggleButton *button, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  (void) button;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* Makes one filter toggle and adds it to the filter bar. */
static GtkWidget *
add_filter_toggle (SbvUsersPanel *self, GtkWidget *bar,
                   const char *label, const char *tooltip)
{
  GtkWidget *toggle = gtk_toggle_button_new_with_label (label);
  gtk_widget_set_tooltip_text (toggle, tooltip);
  g_signal_connect (toggle, "toggled", G_CALLBACK (on_filter_toggled), self);
  gtk_box_append (GTK_BOX (bar), toggle);
  return toggle;
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
  update_lockout_display (self, user);
  sbv_member_of_set (SBV_MEMBER_OF (self->member_of), self->conn,
                     sbv_user_get_dn (user), sbv_user_get_member_of (user),
                     sbv_user_get_primary_group_id (user));

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

  /* Windows profile */
  {
    const char *pp = sbv_user_get_profile_path   (user);
    const char *sp = sbv_user_get_script_path    (user);
    const char *hd = sbv_user_get_home_directory (user);
    const char *dr = sbv_user_get_home_drive     (user);
    gtk_editable_set_text (GTK_EDITABLE (self->prof_path_entry),   pp ? pp : "");
    gtk_editable_set_text (GTK_EDITABLE (self->prof_script_entry), sp ? sp : "");
    gtk_editable_set_text (GTK_EDITABLE (self->prof_home_entry),   hd ? hd : "");
    gtk_editable_set_text (GTK_EDITABLE (self->prof_drive_entry),  dr ? dr : "");
    gtk_widget_set_visible (self->prof_error, FALSE);
  }

  /* RFC2307 / POSIX */
  gint uid = sbv_user_get_uid_number (user);
  gint gid = sbv_user_get_gid_number (user);
  char *uid_str = (uid >= 0) ? g_strdup_printf ("%d", uid) : g_strdup ("");
  char *gid_str = (gid >= 0) ? g_strdup_printf ("%d", gid) : g_strdup ("");
  gtk_editable_set_text (GTK_EDITABLE (self->rfc_uid_entry),   uid_str);
  gtk_editable_set_text (GTK_EDITABLE (self->rfc_gid_entry),   gid_str);
  g_free (uid_str);
  g_free (gid_str);

  const char *shell = sbv_user_get_login_shell (user);
  const char *home  = sbv_user_get_unix_home_dir (user);
  const char *gecos = sbv_user_get_gecos       (user);
  gtk_editable_set_text (GTK_EDITABLE (self->rfc_shell_entry), shell ? shell : "");
  gtk_editable_set_text (GTK_EDITABLE (self->rfc_home_entry),  home  ? home  : "");
  gtk_editable_set_text (GTK_EDITABLE (self->rfc_gecos_entry), gecos ? gecos : "");
  gtk_widget_set_visible (self->rfc_error, FALSE);

  /* Cancel any in-flight collision debounce timers and clear the labels;
   * the changed-handlers below will rearm with the freshly-loaded values. */
  if (self->uid_check_timer) { g_source_remove (self->uid_check_timer); self->uid_check_timer = 0; }
  if (self->gid_check_timer) { g_source_remove (self->gid_check_timer); self->gid_check_timer = 0; }
  if (self->rfc_uid_collision) {
    gtk_label_set_text (GTK_LABEL (self->rfc_uid_collision), "");
    gtk_widget_set_visible (self->rfc_uid_collision, FALSE);
  }
  if (self->rfc_gid_collision) {
    gtk_label_set_text (GTK_LABEL (self->rfc_gid_collision), "");
    gtk_widget_set_visible (self->rfc_gid_collision, FALSE);
  }

  /* Raw LDAP attributes */
  populate_raw_attrs (self->raw_attrs_box, sbv_user_get_ldap_attrs (user), 140);

  /* Clear error labels */
  gtk_label_set_text (GTK_LABEL (self->save_error),   "");
  gtk_label_set_text (GTK_LABEL (self->pw_error),     "");
  gtk_label_set_text (GTK_LABEL (self->policy_error), "");
  gtk_label_set_text (GTK_LABEL (self->delete_error), "");
  gtk_widget_set_visible (self->save_error,   FALSE);
  gtk_widget_set_visible (self->pw_error,     FALSE);
  gtk_widget_set_visible (self->policy_error, FALSE);
  gtk_widget_set_visible (self->delete_error, FALSE);

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

/* ── UID/GID collision detection ────────────────────────────────────────── */

/* Debounce window between the last keystroke and the LDAP probe. Short
 * enough to feel live, long enough to avoid hammering the DC mid-typing. */
#define COLLISION_DEBOUNCE_MS 350

/* Renders a collision result into the supplied label. NULL or empty
 * `hits` clears the label. */
static void
render_collision_label (GtkWidget *label, GPtrArray *hits, const char *kind)
{
  if (!hits || hits->len == 0) {
    gtk_label_set_text (GTK_LABEL (label), "");
    gtk_widget_set_visible (label, FALSE);
    return;
  }

  /* Show up to two conflicts plus a "+N more" suffix if there are more. */
  GString *s = g_string_new (NULL);
  g_string_append_printf (s, "%s in use by ", kind);
  guint shown = MIN (hits->len, 2u);
  for (guint i = 0; i < shown; i++) {
    SbvCollisionHit *h = g_ptr_array_index (hits, i);
    if (i > 0) g_string_append (s, ", ");
    g_string_append (s, h->sam ? h->sam : h->dn);
  }
  if (hits->len > shown)
    g_string_append_printf (s, " (+%u more)", hits->len - shown);

  gtk_label_set_text (GTK_LABEL (label), s->str);
  gtk_widget_set_visible (label, TRUE);
  g_string_free (s, TRUE);
}

/* Per-check context so the completion callback can find the right label
 * and verify the user hasn't typed something else in the meantime. */
typedef struct {
  SbvUsersPanel *panel;
  GtkWidget     *entry;          /* the input we read from on completion   */
  GtkWidget     *label;          /* label to update                         */
  const char    *kind;           /* "UID" or "GID" — used in label text     */
  gint64         value;          /* value we asked the DC about             */
  gboolean       is_uid;         /* TRUE → UID check, FALSE → GID           */
} CollisionCtx;

/* Async-completion callback for either UID or GID checks. Drops the result
 * if the user has since typed a different value into the entry (the next
 * debounce tick will issue a fresh check). */
static void
on_collision_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CollisionCtx  *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  GPtrArray *hits = ctx->is_uid
    ? sbv_collisions_uid_check_finish (conn, result, &err)
    : sbv_collisions_gid_check_finish (conn, result, &err);

  /* Only render if the entry still holds the same numeric value. */
  const char *now_text = gtk_editable_get_text (GTK_EDITABLE (ctx->entry));
  gint64 now_val = *now_text ? g_ascii_strtoll (now_text, NULL, 10) : -1;

  if (now_val == ctx->value) {
    if (hits)
      render_collision_label (ctx->label, hits, ctx->kind);
    else {
      /* Search failed (e.g. transient network issue) — leave the label
       * empty rather than alarming the user with a noisy message. */
      gtk_label_set_text (GTK_LABEL (ctx->label), "");
      gtk_widget_set_visible (ctx->label, FALSE);
    }
  }

  if (hits) sbv_collision_hits_free (hits);
  if (err)  g_error_free (err);
  g_free (ctx);
}

/* Fires the UID collision check for whatever's currently in rfc_uid_entry. */
static gboolean
fire_uid_check (gpointer user_data)
{
  SbvUsersPanel *self = user_data;
  self->uid_check_timer = 0;

  if (!self->conn) return G_SOURCE_REMOVE;

  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->rfc_uid_entry));
  if (!*text) {
    gtk_label_set_text (GTK_LABEL (self->rfc_uid_collision), "");
    gtk_widget_set_visible (self->rfc_uid_collision, FALSE);
    return G_SOURCE_REMOVE;
  }

  gint64 uid = g_ascii_strtoll (text, NULL, 10);

  CollisionCtx *ctx = g_new0 (CollisionCtx, 1);
  ctx->panel  = self;
  ctx->entry  = self->rfc_uid_entry;
  ctx->label  = self->rfc_uid_collision;
  ctx->kind   = "UID";
  ctx->value  = uid;
  ctx->is_uid = TRUE;

  const char *exclude = self->selected_user
    ? sbv_user_get_dn (self->selected_user) : NULL;

  sbv_collisions_uid_check_async (self->conn, uid, exclude,
                                   NULL, on_collision_done, ctx);
  return G_SOURCE_REMOVE;
}

/* Fires the GID collision check for whatever's currently in rfc_gid_entry. */
static gboolean
fire_gid_check (gpointer user_data)
{
  SbvUsersPanel *self = user_data;
  self->gid_check_timer = 0;

  if (!self->conn) return G_SOURCE_REMOVE;

  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->rfc_gid_entry));
  if (!*text) {
    gtk_label_set_text (GTK_LABEL (self->rfc_gid_collision), "");
    gtk_widget_set_visible (self->rfc_gid_collision, FALSE);
    return G_SOURCE_REMOVE;
  }

  gint64 gid = g_ascii_strtoll (text, NULL, 10);

  CollisionCtx *ctx = g_new0 (CollisionCtx, 1);
  ctx->panel  = self;
  ctx->entry  = self->rfc_gid_entry;
  ctx->label  = self->rfc_gid_collision;
  ctx->kind   = "GID";
  ctx->value  = gid;
  ctx->is_uid = FALSE;

  /* The user's own DN isn't a meaningful exclusion when checking groups,
   * but pass it through anyway — the filter is on group objects so a
   * user DN will simply never match. */
  sbv_collisions_gid_check_async (self->conn, gid, NULL,
                                   NULL, on_collision_done, ctx);
  return G_SOURCE_REMOVE;
}

/* "changed" handler on rfc_uid_entry: debounce a collision check. */
static void
on_uid_entry_changed (GtkEditable *e, gpointer user_data)
{
  (void) e;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (self->uid_check_timer)
    g_source_remove (self->uid_check_timer);
  self->uid_check_timer =
    g_timeout_add (COLLISION_DEBOUNCE_MS, fire_uid_check, self);
}

/* "changed" handler on rfc_gid_entry: debounce a collision check. */
static void
on_gid_entry_changed (GtkEditable *e, gpointer user_data)
{
  (void) e;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (self->gid_check_timer)
    g_source_remove (self->gid_check_timer);
  self->gid_check_timer =
    g_timeout_add (COLLISION_DEBOUNCE_MS, fire_gid_check, self);
}

/* ── UID/GID auto-assignment (Suggest buttons) ──────────────────────────── */

/* Per-suggest context: which entry to fill once the next-free probe
 * returns. */
typedef struct {
  SbvUsersPanel *panel;
  GtkWidget     *entry;     /* rfc_uid_entry or rfc_gid_entry          */
  GtkWidget     *button;    /* the suggest button to re-enable          */
  gboolean       is_uid;    /* TRUE → uid scan, FALSE → gid scan        */
} SuggestCtx;

/* Async-completion: fill the entry with the suggested value and re-enable
 * the button. The entry's "changed" handler will then run the existing
 * collision check, which should report no collision for the value we
 * picked. */
static void
on_suggest_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SuggestCtx    *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gint64 v = ctx->is_uid
    ? sbv_collisions_next_free_uid_finish (conn, result, &err)
    : sbv_collisions_next_free_gid_finish (conn, result, &err);

  gtk_widget_set_sensitive (ctx->button, TRUE);

  if (err) {
    g_warning ("Suggest %s failed: %s", ctx->is_uid ? "UID" : "GID",
                err->message);
    g_error_free (err);
  } else if (v < 0) {
    g_warning ("Suggest %s: configured range exhausted",
                ctx->is_uid ? "UID" : "GID");
  } else {
    char buf[32];
    g_snprintf (buf, sizeof buf, "%" G_GINT64_FORMAT, v);
    gtk_editable_set_text (GTK_EDITABLE (ctx->entry), buf);
  }

  g_free (ctx);
}

/* Issues the next-free scan using the (already-loaded) cached hints. */
static void
suggest_with_hints (SbvUsersPanel *self, gboolean is_uid,
                    GtkWidget *entry, GtkWidget *button)
{
  gint64 lo = is_uid ? self->idmap_hints->uid_min : self->idmap_hints->gid_min;
  gint64 hi = is_uid ? self->idmap_hints->uid_max : self->idmap_hints->gid_max;

  /* Honour the next_*_hint published by the DC if it falls in range —
   * starting the scan from the hint shortens the work the DC has to do
   * and matches the DC's own bookkeeping. */
  gint64 hint = is_uid ? self->idmap_hints->next_uid_hint
                       : self->idmap_hints->next_gid_hint;
  if (hint > lo && hint <= hi) lo = hint;

  SuggestCtx *ctx = g_new0 (SuggestCtx, 1);
  ctx->panel  = self;
  ctx->entry  = entry;
  ctx->button = button;
  ctx->is_uid = is_uid;

  if (is_uid)
    sbv_collisions_next_free_uid_async (self->conn, lo, hi,
                                         NULL, on_suggest_done, ctx);
  else
    sbv_collisions_next_free_gid_async (self->conn, lo, hi,
                                         NULL, on_suggest_done, ctx);
}

/* Lazy hints-load context, kept while the idmap probe is in flight so we
 * can resume the suggest after hints arrive. */
typedef struct {
  SbvUsersPanel *panel;
  gboolean       is_uid;
  GtkWidget     *entry;
  GtkWidget     *button;
} HintsThenSuggestCtx;

/* Completion of the idmap-hints probe. Stores the hints on the panel and
 * proceeds with the deferred suggest call. */
static void
on_hints_then_suggest (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  HintsThenSuggestCtx *ctx  = user_data;
  SbvUsersPanel       *self = ctx->panel;
  SbvConnection       *conn = SBV_CONNECTION (source);
  GError              *err  = NULL;

  SbvIdmapHints *hints = sbv_idmap_hints_query_finish (conn, result, &err);
  if (!hints) {
    g_warning ("Idmap hints probe failed: %s",
                err ? err->message : "(no detail)");
    if (err) g_error_free (err);
    /* Fall back to profile-only hints so suggest still works. */
    SbvProfile *profile = sbv_connection_get_profile (conn);
    hints = sbv_idmap_hints_from_profile (profile);
  }

  g_clear_pointer (&self->idmap_hints, sbv_idmap_hints_free);
  self->idmap_hints = hints;

  suggest_with_hints (self, ctx->is_uid, ctx->entry, ctx->button);
  g_free (ctx);
}

/* Common entry point for both suggest buttons. Ensures hints are loaded
 * (one DC round-trip the first time, cached thereafter) before scanning. */
static void
do_suggest (SbvUsersPanel *self, gboolean is_uid,
            GtkWidget *entry, GtkWidget *button)
{
  if (!self->conn) return;
  gtk_widget_set_sensitive (button, FALSE);

  if (self->idmap_hints) {
    suggest_with_hints (self, is_uid, entry, button);
    return;
  }

  HintsThenSuggestCtx *ctx = g_new0 (HintsThenSuggestCtx, 1);
  ctx->panel  = self;
  ctx->is_uid = is_uid;
  ctx->entry  = entry;
  ctx->button = button;

  SbvProfile *profile = sbv_connection_get_profile (self->conn);
  sbv_idmap_hints_query_async (self->conn, profile, NULL,
                                on_hints_then_suggest, ctx);
}

/* "Suggest" button next to the UID entry. */
static void
on_suggest_uid_clicked (GtkButton *btn, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  do_suggest (self, TRUE, self->rfc_uid_entry, GTK_WIDGET (btn));
}

/* "Suggest" button next to the GID entry. */
static void
on_suggest_gid_clicked (GtkButton *btn, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  do_suggest (self, FALSE, self->rfc_gid_entry, GTK_WIDGET (btn));
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
    sbv_users_panel_load (self, conn);
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
    sbv_users_panel_load (self, conn);
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

/* ── Create / Delete ────────────────────────────────────────────────────── */

/* Called when the create-user dialog finishes successfully. Records the
 * new DN so the upcoming reload auto-selects that row. We match by DN
 * (not sam) because the create dialog allows arbitrary cn values; the DN
 * is the only identifier the dialog produces that we can compare against
 * later. */
static void
on_user_created (SbvConnection *conn, const char *new_dn, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  g_free (self->pending_select_dn);
  self->pending_select_dn = g_strdup (new_dn);
  sbv_users_panel_load (self, conn);
}

/* Toolbar "+" button: opens the create-user dialog. */
static void
on_add_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->conn) return;

  GtkWidget *parent = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  sbv_create_user_dialog_show (parent ? GTK_WINDOW (parent) : NULL,
                                self->conn, on_user_created, self);
}

/* Async-completion callback for the user delete operation. On error the
 * detail pane shows the message; on success the list is reloaded and the
 * selection is dropped (the formerly-selected row no longer exists). */
static void
on_delete_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->delete_btn, TRUE);

  if (!sbv_users_delete_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->delete_error), err->message);
    gtk_widget_set_visible (self->delete_error, TRUE);
    g_error_free (err);
    return;
  }

  /* Drop the now-stale selection so the list reload doesn't try to
   * re-select a deleted entry. */
  g_clear_object (&self->selected_user);
  sbv_users_panel_load (self, conn);
}

/* Type-to-confirm dialog state. Lives until the dialog destroys. */
typedef struct {
  SbvUsersPanel *panel;
  GtkWindow     *dialog;
  GtkWidget     *entry;
  GtkWidget     *delete_btn;
  char          *expected_sam;
} ConfirmDelCtx;

/* Frees the per-dialog context. */
static void
confirm_del_ctx_free (ConfirmDelCtx *ctx)
{
  g_free (ctx->expected_sam);
  g_free (ctx);
}

/* Enables the destructive button only when the typed text exactly matches
 * the target's sAMAccountName. */
static void
on_confirm_entry_changed (GtkEditable *editable, gpointer user_data)
{
  ConfirmDelCtx *ctx   = user_data;
  const char    *typed = gtk_editable_get_text (editable);
  gtk_widget_set_sensitive (ctx->delete_btn,
                             g_str_equal (typed, ctx->expected_sam));
}

/* Confirm-button handler: dispatches the delete and closes the dialog. */
static void
on_confirm_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  ConfirmDelCtx *ctx  = user_data;
  SbvUsersPanel *self = ctx->panel;
  if (!self->conn || !self->selected_user) {
    gtk_window_destroy (ctx->dialog);
    return;
  }

  gtk_widget_set_visible (self->delete_error, FALSE);
  gtk_widget_set_sensitive (self->delete_btn, FALSE);

  sbv_users_delete_async (self->conn, self->selected_user,
                           NULL, on_delete_done, self);
  gtk_window_destroy (ctx->dialog);
}

/* Builds and presents the type-to-confirm modal. */
static void
show_delete_confirm (SbvUsersPanel *self)
{
  if (!self->selected_user) return;
  const char *sam = sbv_user_get_sam (self->selected_user);
  if (!sam || !*sam) return;

  ConfirmDelCtx *ctx = g_new0 (ConfirmDelCtx, 1);
  ctx->panel        = self;
  ctx->expected_sam = g_strdup (sam);

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Delete User");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 420, 220);
  GtkWidget *parent = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  if (parent) gtk_window_set_transient_for (GTK_WINDOW (win),
                                             GTK_WINDOW (parent));
  ctx->dialog = GTK_WINDOW (win);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);
  g_signal_connect_swapped (cancel_btn, "clicked",
                             G_CALLBACK (gtk_window_destroy), win);

  ctx->delete_btn = gtk_button_new_with_label ("Delete");
  gtk_widget_add_css_class (ctx->delete_btn, "destructive-action");
  gtk_widget_set_sensitive (ctx->delete_btn, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->delete_btn);
  g_signal_connect (ctx->delete_btn, "clicked",
                    G_CALLBACK (on_confirm_delete_clicked), ctx);

  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top    (body, 16);
  gtk_widget_set_margin_bottom (body, 16);
  gtk_widget_set_margin_start  (body, 20);
  gtk_widget_set_margin_end    (body, 20);

  char *warn_text = g_strdup_printf (
    "Permanently delete user <b>%s</b>?\n"
    "This cannot be undone.", sam);
  GtkWidget *warn = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (warn), warn_text);
  gtk_label_set_xalign (GTK_LABEL (warn), 0);
  gtk_label_set_wrap   (GTK_LABEL (warn), TRUE);
  g_free (warn_text);
  gtk_box_append (GTK_BOX (body), warn);

  char *prompt_text = g_strdup_printf (
    "Type the username (<tt>%s</tt>) to confirm:", sam);
  GtkWidget *prompt = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (prompt), prompt_text);
  gtk_label_set_xalign (GTK_LABEL (prompt), 0);
  gtk_widget_add_css_class (prompt, "dim-label");
  gtk_widget_set_margin_top (prompt, 4);
  g_free (prompt_text);
  gtk_box_append (GTK_BOX (body), prompt);

  ctx->entry = gtk_entry_new ();
  gtk_widget_add_css_class (ctx->entry, "monospace");
  g_signal_connect (ctx->entry, "changed",
                    G_CALLBACK (on_confirm_entry_changed), ctx);
  gtk_box_append (GTK_BOX (body), ctx->entry);

  gtk_box_append (GTK_BOX (vbox), body);

  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect_swapped (win, "destroy",
                             G_CALLBACK (confirm_del_ctx_free), ctx);
  gtk_window_present (GTK_WINDOW (win));
  gtk_widget_grab_focus (ctx->entry);
}

/* Detail-pane "Delete" button: routes to the type-to-confirm dialog. */
static void
on_delete_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  show_delete_confirm (SBV_USERS_PANEL (user_data));
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

  /* Decide what to re-select. pending_select_dn (set by the create flow)
   * wins, matched by full DN; otherwise fall back to selected_user's sam
   * which preserves selection across normal save reloads. */
  char *target_dn  = NULL;
  char *target_sam = NULL;
  if (self->pending_select_dn) {
    target_dn = self->pending_select_dn;        /* takes ownership */
    self->pending_select_dn = NULL;
  } else if (self->selected_user) {
    target_sam = g_strdup (sbv_user_get_sam (self->selected_user));
  }

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  GtkListBoxRow *reselect = NULL;

  for (guint i = 0; i < n; i++) {
    SbvUser   *user = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget *row  = make_user_row (user);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
    if (!reselect) {
      if (target_dn && g_str_equal (sbv_user_get_dn (user) ?: "", target_dn))
        reselect = GTK_LIST_BOX_ROW (row);
      else if (target_sam && g_str_equal (sbv_user_get_sam (user) ?: "", target_sam))
        reselect = GTK_LIST_BOX_ROW (row);
    }
    g_object_unref (user);
  }

  g_object_unref (store);
  g_free (target_dn);
  g_free (target_sam);

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
  /* Drop cached hints when the connection changes — they're tied to the
   * profile's UID/GID range and the DC's published values. */
  if (self->conn != conn)
    g_clear_pointer (&self->idmap_hints, sbv_idmap_hints_free);

  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "loading");
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  sbv_users_list_async (conn, NULL, on_users_loaded, self);
}

/* Reloads the list after a group membership change, keeping the user
 * selected so the Member Of section shows the new state. */
static void
on_member_of_changed (SbvMemberOf *section, gpointer user_data)
{
  (void) section;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (self->conn) sbv_users_panel_load (self, self->conn);
  g_signal_emit (self, signals[SIGNAL_MEMBERSHIP_CHANGED], 0);
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

/* ── Raw attrs helpers ──────────────────────────────────────────────────── */

static GtkWidget *
make_raw_attr_row (const char *attr, const char *value, int label_width)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (box, 2);
  gtk_widget_set_margin_bottom (box, 2);
  GtkWidget *lbl = gtk_label_new (attr);
  gtk_label_set_xalign (GTK_LABEL (lbl), 1.0);
  gtk_widget_set_size_request (lbl, label_width, -1);
  gtk_widget_add_css_class (lbl, "dim-label");
  gtk_box_append (GTK_BOX (box), lbl);
  GtkWidget *val = gtk_label_new (value);
  gtk_label_set_xalign (GTK_LABEL (val), 0.0);
  gtk_label_set_selectable (GTK_LABEL (val), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (val), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (val, TRUE);
  gtk_widget_add_css_class (val, "monospace");
  gtk_box_append (GTK_BOX (box), val);
  return box;
}

static void
populate_raw_attrs (GtkWidget *box, GHashTable *attrs, int label_width)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (box)) != NULL)
    gtk_box_remove (GTK_BOX (box), child);
  if (!attrs) return;
  GList *keys = g_list_sort (g_hash_table_get_keys (attrs), (GCompareFunc) g_strcmp0);
  for (GList *l = keys; l; l = l->next) {
    const char  *attr   = l->data;
    char       **vals   = g_hash_table_lookup (attrs, attr);
    char        *joined = (vals && vals[0]) ? g_strjoinv ("; ", vals)
                                            : g_strdup ("\xe2\x80\x94");
    gtk_box_append (GTK_BOX (box), make_raw_attr_row (attr, joined, label_width));
    g_free (joined);
  }
  g_list_free (keys);
}

/* ── Paned position ─────────────────────────────────────────────────────── */

static void
on_paned_map (GtkWidget *widget, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->paned_init) {
    int w = gtk_widget_get_width (widget);
    if (w > 0) {
      self->paned_init = TRUE;
      gtk_paned_set_position (GTK_PANED (widget), 280);
    }
  }
}

/* ── RFC2307 save ───────────────────────────────────────────────────────── */

/* Shows a profile-section error under its Save button. */
static void
show_profile_error (SbvUsersPanel *self, const char *msg)
{
  gtk_label_set_text (GTK_LABEL (self->prof_error), msg);
  gtk_widget_set_visible (self->prof_error, TRUE);
}

/* Profile save completion: show the error, or reload to pick up the
 * stored values. */
static void
on_profile_save_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->prof_save_btn, TRUE);

  if (!sbv_users_set_profile_attrs_finish (conn, result, &err)) {
    show_profile_error (self, err->message);
    g_error_free (err);
    return;
  }

  gtk_widget_set_visible (self->prof_error, FALSE);
  sbv_users_panel_load (self, conn);
}

/* Checks the home drive, then writes the four profile attributes. A drive
 * letter only makes sense with a home folder to map it to. */
static void
on_profile_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->selected_user || !self->conn) return;

  gtk_widget_set_visible (self->prof_error, FALSE);

  char *path   = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->prof_path_entry))));
  char *script = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->prof_script_entry))));
  char *home   = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->prof_home_entry))));
  char *drive_s = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->prof_drive_entry))));
  char *drive  = NULL;
  gboolean ok  = TRUE;

  if (*drive_s) {
    drive = sbv_user_normalise_home_drive (drive_s);
    if (!drive) {
      show_profile_error (self, "Home drive must be a single letter, such as H:.");
      ok = FALSE;
    } else if (!*home) {
      show_profile_error (self, "A home drive needs a home folder to map to.");
      ok = FALSE;
    }
  }

  if (ok) {
    gtk_widget_set_sensitive (self->prof_save_btn, FALSE);
    sbv_users_set_profile_attrs_async (self->conn, self->selected_user,
                                       home, drive, script, path,
                                       NULL, on_profile_save_done, self);
  }
  g_free (path);
  g_free (script);
  g_free (home);
  g_free (drive_s);
  g_free (drive);
}

static void
on_rfc_save_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  gtk_widget_set_sensitive (self->rfc_save_btn, TRUE);

  if (!sbv_users_set_unix_attrs_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->rfc_error), err->message);
    gtk_widget_set_visible (self->rfc_error, TRUE);
    g_error_free (err);
    return;
  }

  gtk_widget_set_visible (self->rfc_error, FALSE);
  sbv_users_panel_load (self, conn);
}

static void
on_rfc_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->selected_user || !self->conn) return;

  gtk_widget_set_visible (self->rfc_error, FALSE);
  gtk_widget_set_sensitive (self->rfc_save_btn, FALSE);

  const char *uid_s = gtk_editable_get_text (GTK_EDITABLE (self->rfc_uid_entry));
  const char *gid_s = gtk_editable_get_text (GTK_EDITABLE (self->rfc_gid_entry));
  const char *shell = gtk_editable_get_text (GTK_EDITABLE (self->rfc_shell_entry));
  const char *home  = gtk_editable_get_text (GTK_EDITABLE (self->rfc_home_entry));
  const char *gecos = gtk_editable_get_text (GTK_EDITABLE (self->rfc_gecos_entry));

  gint uid = *uid_s ? (gint) strtol (uid_s, NULL, 10) : -1;
  gint gid = *gid_s ? (gint) strtol (gid_s, NULL, 10) : -1;

  sbv_users_set_unix_attrs_async (self->conn, self->selected_user,
                                   uid, gid, shell, home, gecos,
                                   NULL, on_rfc_save_done, self);
}

/* ── Move ───────────────────────────────────────────────────────────────── */

/* Move-dialog completion: reload so the list and DN show the new location;
 * reselect it by its new DN. */
static void
on_moved (SbvConnection *conn, const char *new_dn, gpointer user_data)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  g_free (self->pending_select_dn);
  self->pending_select_dn = g_strdup (new_dn);
  sbv_users_panel_load (self, conn);
}

/* "Move…" button beside the DN: pick a new OU or container for the
 * selected user. */
static void
on_move_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvUsersPanel *self = SBV_USERS_PANEL (user_data);
  if (!self->conn || !self->selected_user) return;
  GtkWidget *parent = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  sbv_move_dialog_show (parent ? GTK_WINDOW (parent) : NULL, self->conn,
                        sbv_user_get_dn (self->selected_user),
                        sbv_user_get_sam (self->selected_user),
                        on_moved, self);
}

/* ── GObject init ───────────────────────────────────────────────────────── */

static void
sbv_users_panel_finalize (GObject *object)
{
  SbvUsersPanel *self = SBV_USERS_PANEL (object);
  g_free (self->filter_text);
  g_free (self->pending_select_dn);
  if (self->uid_check_timer) g_source_remove (self->uid_check_timer);
  if (self->gid_check_timer) g_source_remove (self->gid_check_timer);
  g_clear_pointer (&self->idmap_hints, sbv_idmap_hints_free);
  g_clear_object (&self->selected_user);
  G_OBJECT_CLASS (sbv_users_panel_parent_class)->finalize (object);
}

/* Registers finalize and the "membership-changed" signal, which fires
 * after a user joins or leaves a group, so the window can reload panels that show the other side. */
static void sbv_users_panel_class_init (SbvUsersPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_users_panel_finalize;
  signals[SIGNAL_MEMBERSHIP_CHANGED] =
    g_signal_new ("membership-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
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
    self->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *paned = self->paned;
    g_signal_connect (paned, "map", G_CALLBACK (on_paned_map), self);

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

    self->add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_add_css_class (self->add_btn, "flat");
    gtk_widget_set_tooltip_text (self->add_btn, "Create user");
    g_signal_connect (self->add_btn, "clicked",
                      G_CALLBACK (on_add_btn_clicked), self);
    gtk_box_append (GTK_BOX (toolbar), self->add_btn);

    gtk_box_append (GTK_BOX (left_box), toolbar);

    GtkWidget *filter_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_bottom (filter_bar, 6);
    gtk_widget_set_margin_start  (filter_bar, 8);
    gtk_widget_set_margin_end    (filter_bar, 8);
    self->disabled_toggle = add_filter_toggle (self, filter_bar, "Disabled",
                                               "Show only disabled accounts");
    self->locked_toggle   = add_filter_toggle (self, filter_bar, "Locked",
                                               "Show only locked-out accounts");
    self->expired_toggle  = add_filter_toggle (self, filter_bar, "Expired",
                                               "Show only accounts whose password has expired");
    gtk_box_append (GTK_BOX (left_box), filter_bar);

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
    GtkWidget *no_match = gtk_label_new ("No users match");
    gtk_widget_add_css_class (no_match, "dim-label");
    gtk_widget_set_margin_top    (no_match, 12);
    gtk_widget_set_margin_bottom (no_match, 12);
    gtk_list_box_set_placeholder (GTK_LIST_BOX (self->list_box), no_match);
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
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

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
    {
      GtkWidget *dn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_hexpand (self->dn_label, TRUE);
      gtk_box_append (GTK_BOX (dn_box), self->dn_label);
      GtkWidget *move_btn = gtk_button_new_with_label ("Move…");
      gtk_widget_set_tooltip_text (move_btn, "Move to another OU or container");
      g_signal_connect (move_btn, "clicked", G_CALLBACK (on_move_clicked), self);
      gtk_box_append (GTK_BOX (dn_box), move_btn);
      gtk_box_append (GTK_BOX (form), make_field_row ("DN", dn_box));
    }

    self->sam_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->sam_label), 0);
    gtk_widget_add_css_class (self->sam_label, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Username", self->sam_label));

    self->enabled_check = gtk_check_button_new_with_label ("Account enabled");
    gtk_widget_set_margin_start (self->enabled_check, 148);
    gtk_box_append (GTK_BOX (form), self->enabled_check);

    self->lockout_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->lockout_label), 0);
    gtk_label_set_wrap (GTK_LABEL (self->lockout_label), TRUE);
    self->unlock_btn = gtk_button_new_with_label ("Unlock");
    gtk_widget_set_tooltip_text (self->unlock_btn,
                                 "Clear the lockout so the user can sign in again");
    gtk_widget_set_valign (self->unlock_btn, GTK_ALIGN_CENTER);
    g_signal_connect (self->unlock_btn, "clicked",
                      G_CALLBACK (on_unlock_clicked), self);
    {
      GtkWidget *lock_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_hexpand (self->lockout_label, TRUE);
      gtk_box_append (GTK_BOX (lock_box), self->lockout_label);
      gtk_box_append (GTK_BOX (lock_box), self->unlock_btn);
      gtk_box_append (GTK_BOX (form), make_field_row ("Lockout", lock_box));
    }
    self->unlock_error = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->unlock_error), 0);
    gtk_label_set_wrap (GTK_LABEL (self->unlock_error), TRUE);
    gtk_widget_add_css_class (self->unlock_error, "error");
    gtk_widget_set_margin_start (self->unlock_error, 148);
    gtk_widget_set_visible (self->unlock_error, FALSE);
    gtk_box_append (GTK_BOX (form), self->unlock_error);

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

    /* ── Member Of ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Member Of"));
    self->member_of = sbv_member_of_new ();
    g_signal_connect (self->member_of, "changed",
                      G_CALLBACK (on_member_of_changed), self);
    gtk_box_append (GTK_BOX (form), self->member_of);

    /* ── Windows Profile ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Windows Profile"));

    self->prof_path_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->prof_path_entry),
                                    "\\\\server\\profiles\\username");
    gtk_widget_add_css_class (self->prof_path_entry, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Profile Path", self->prof_path_entry));

    self->prof_script_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->prof_script_entry), "logon.bat");
    gtk_widget_add_css_class (self->prof_script_entry, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Logon Script", self->prof_script_entry));

    self->prof_home_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->prof_home_entry),
                                    "\\\\server\\home\\username");
    gtk_widget_add_css_class (self->prof_home_entry, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Home Folder", self->prof_home_entry));

    self->prof_drive_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->prof_drive_entry), "H:");
    gtk_entry_set_max_length (GTK_ENTRY (self->prof_drive_entry), 3);
    gtk_editable_set_width_chars (GTK_EDITABLE (self->prof_drive_entry), 4);
    gtk_widget_add_css_class (self->prof_drive_entry, "monospace");
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *lbl = gtk_label_new ("Home Drive");
      gtk_label_set_xalign (GTK_LABEL (lbl), 1);
      gtk_widget_set_size_request (lbl, 140, -1);
      gtk_widget_add_css_class (lbl, "dim-label");
      gtk_box_append (GTK_BOX (row), lbl);
      gtk_box_append (GTK_BOX (row), self->prof_drive_entry);
      gtk_box_append (GTK_BOX (form), row);
    }

    {
      GtkWidget *prof_btn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_top (prof_btn_box, 4);
      self->prof_save_btn = gtk_button_new_with_label ("Save Profile");
      gtk_widget_add_css_class (self->prof_save_btn, "suggested-action");
      gtk_widget_set_halign (self->prof_save_btn, GTK_ALIGN_START);
      gtk_widget_set_margin_start (self->prof_save_btn, 148);
      g_signal_connect (self->prof_save_btn, "clicked",
                        G_CALLBACK (on_profile_save_clicked), self);
      gtk_box_append (GTK_BOX (prof_btn_box), self->prof_save_btn);

      self->prof_error = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (self->prof_error), 0);
      gtk_label_set_wrap (GTK_LABEL (self->prof_error), TRUE);
      gtk_widget_add_css_class (self->prof_error, "error");
      gtk_widget_set_margin_start (self->prof_error, 148);
      gtk_widget_set_visible (self->prof_error, FALSE);
      gtk_box_append (GTK_BOX (prof_btn_box), self->prof_error);
      gtk_box_append (GTK_BOX (form), prof_btn_box);
    }

    /* ── Unix Attributes (RFC2307) ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Unix Attributes (RFC2307)"));

    self->rfc_uid_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->rfc_uid_entry), "UID number");
    gtk_widget_add_css_class (self->rfc_uid_entry, "monospace");
    g_signal_connect (self->rfc_uid_entry, "changed",
                      G_CALLBACK (on_uid_entry_changed), self);

    self->rfc_uid_suggest_btn = gtk_button_new_with_label ("Suggest");
    gtk_widget_set_tooltip_text (self->rfc_uid_suggest_btn,
                                  "Fill with the lowest unused UID in the configured range");
    g_signal_connect (self->rfc_uid_suggest_btn, "clicked",
                      G_CALLBACK (on_suggest_uid_clicked), self);
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *lbl = gtk_label_new ("UID Number");
      gtk_label_set_xalign (GTK_LABEL (lbl), 1);
      gtk_widget_set_size_request (lbl, 140, -1);
      gtk_widget_add_css_class (lbl, "dim-label");
      gtk_box_append (GTK_BOX (row), lbl);
      gtk_widget_set_hexpand (self->rfc_uid_entry, TRUE);
      gtk_box_append (GTK_BOX (row), self->rfc_uid_entry);
      gtk_box_append (GTK_BOX (row), self->rfc_uid_suggest_btn);
      gtk_box_append (GTK_BOX (form), row);
    }

    self->rfc_uid_collision = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->rfc_uid_collision), 0);
    gtk_label_set_wrap (GTK_LABEL (self->rfc_uid_collision), TRUE);
    gtk_widget_add_css_class (self->rfc_uid_collision, "warning");
    gtk_widget_set_margin_start (self->rfc_uid_collision, 148);
    gtk_widget_set_visible (self->rfc_uid_collision, FALSE);
    gtk_box_append (GTK_BOX (form), self->rfc_uid_collision);

    self->rfc_gid_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->rfc_gid_entry), "GID number");
    gtk_widget_add_css_class (self->rfc_gid_entry, "monospace");
    g_signal_connect (self->rfc_gid_entry, "changed",
                      G_CALLBACK (on_gid_entry_changed), self);

    self->rfc_gid_suggest_btn = gtk_button_new_with_label ("Suggest");
    gtk_widget_set_tooltip_text (self->rfc_gid_suggest_btn,
                                  "Fill with the lowest unused GID in the configured range");
    g_signal_connect (self->rfc_gid_suggest_btn, "clicked",
                      G_CALLBACK (on_suggest_gid_clicked), self);
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *lbl = gtk_label_new ("GID Number");
      gtk_label_set_xalign (GTK_LABEL (lbl), 1);
      gtk_widget_set_size_request (lbl, 140, -1);
      gtk_widget_add_css_class (lbl, "dim-label");
      gtk_box_append (GTK_BOX (row), lbl);
      gtk_widget_set_hexpand (self->rfc_gid_entry, TRUE);
      gtk_box_append (GTK_BOX (row), self->rfc_gid_entry);
      gtk_box_append (GTK_BOX (row), self->rfc_gid_suggest_btn);
      gtk_box_append (GTK_BOX (form), row);
    }

    self->rfc_gid_collision = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->rfc_gid_collision), 0);
    gtk_label_set_wrap (GTK_LABEL (self->rfc_gid_collision), TRUE);
    gtk_widget_add_css_class (self->rfc_gid_collision, "warning");
    gtk_widget_set_margin_start (self->rfc_gid_collision, 148);
    gtk_widget_set_visible (self->rfc_gid_collision, FALSE);
    gtk_box_append (GTK_BOX (form), self->rfc_gid_collision);

    self->rfc_shell_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->rfc_shell_entry), "/bin/bash");
    gtk_widget_add_css_class (self->rfc_shell_entry, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Login Shell", self->rfc_shell_entry));

    self->rfc_home_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->rfc_home_entry), "/home/username");
    gtk_widget_add_css_class (self->rfc_home_entry, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Home Directory", self->rfc_home_entry));

    self->rfc_gecos_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->rfc_gecos_entry), "Full Name,,,,");
    gtk_box_append (GTK_BOX (form), make_field_row ("GECOS", self->rfc_gecos_entry));

    {
      GtkWidget *rfc_btn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_top (rfc_btn_box, 4);
      self->rfc_save_btn = gtk_button_new_with_label ("Save Unix Attributes");
      gtk_widget_add_css_class (self->rfc_save_btn, "suggested-action");
      gtk_widget_set_halign (self->rfc_save_btn, GTK_ALIGN_START);
      gtk_widget_set_margin_start (self->rfc_save_btn, 148);
      g_signal_connect (self->rfc_save_btn, "clicked",
                        G_CALLBACK (on_rfc_save_clicked), self);
      gtk_box_append (GTK_BOX (rfc_btn_box), self->rfc_save_btn);

      self->rfc_error = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (self->rfc_error), 0);
      gtk_label_set_wrap (GTK_LABEL (self->rfc_error), TRUE);
      gtk_widget_add_css_class (self->rfc_error, "error");
      gtk_widget_set_margin_start (self->rfc_error, 148);
      gtk_widget_set_visible (self->rfc_error, FALSE);
      gtk_box_append (GTK_BOX (rfc_btn_box), self->rfc_error);
      gtk_box_append (GTK_BOX (form), rfc_btn_box);
    }

    /* ── All LDAP Attributes ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("All LDAP Attributes"));

    self->raw_attrs_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (form), self->raw_attrs_box);

    /* ── Danger zone ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Danger Zone"));

    {
      GtkWidget *del_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_top (del_box, 4);

      self->delete_btn = gtk_button_new_with_label ("Delete User…");
      gtk_widget_add_css_class (self->delete_btn, "destructive-action");
      gtk_widget_set_halign (self->delete_btn, GTK_ALIGN_START);
      gtk_widget_set_margin_start (self->delete_btn, 148);
      g_signal_connect (self->delete_btn, "clicked",
                        G_CALLBACK (on_delete_btn_clicked), self);
      gtk_box_append (GTK_BOX (del_box), self->delete_btn);

      self->delete_error = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (self->delete_error), 0);
      gtk_label_set_wrap (GTK_LABEL (self->delete_error), TRUE);
      gtk_widget_add_css_class (self->delete_error, "error");
      gtk_widget_set_margin_start (self->delete_error, 148);
      gtk_widget_set_visible (self->delete_error, FALSE);
      gtk_box_append (GTK_BOX (del_box), self->delete_error);
      gtk_box_append (GTK_BOX (form), del_box);
    }

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
