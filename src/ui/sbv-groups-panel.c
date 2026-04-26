#include "sbv-groups-panel.h"
#include "sbv-create-group-dialog.h"
#include "../backend/sbv-groups-backend.h"
#include "../backend/sbv-users-backend.h"
#include "../backend/sbv-collisions.h"
#include "../backend/sbv-idmap-hints.h"
#include "../model/sbv-group.h"
#include "../model/sbv-user.h"

#include <adwaita.h>
#include <string.h>

/* groupType bit flags (AD) */
#define GT_SECURITY  ((gint32)0x80000000)
#define GT_BUILTIN   0x00000001
#define GT_GLOBAL    0x00000002
#define GT_DOMAIN_LOCAL 0x00000004
#define GT_UNIVERSAL 0x00000008

struct _SbvGroupsPanel {
  GtkBox         parent;

  GtkWidget     *outer_stack;    /* "loading" | "empty" | "split" */
  GtkWidget     *search_entry;
  GtkWidget     *add_btn;
  GtkWidget     *list_box;

  /* Detail pane */
  GtkWidget     *detail_stack;   /* "none" | "detail" */
  GtkWidget     *detail_dn;
  GtkWidget     *detail_name;
  GtkWidget     *detail_sam;
  GtkWidget     *detail_desc;
  GtkWidget     *detail_type;
  GtkWidget     *detail_scope;

  /* RFC2307 / POSIX */
  GtkWidget     *rfc_gid_entry;
  GtkWidget     *rfc_member_uid_label; /* memberUid — read-only */
  GtkWidget     *rfc_save_btn;
  GtkWidget     *rfc_error;
  GtkWidget     *rfc_gid_collision;    /* warning under gid entry */
  guint          gid_check_timer;       /* g_timeout_add id for debounce */
  GtkWidget     *rfc_gid_suggest_btn;
  /* Cached idmap hints for the active connection. Loaded lazily. */
  SbvIdmapHints *idmap_hints;

  /* Raw LDAP attributes (dynamic, rebuilt on selection) */
  GtkWidget     *raw_attrs_box;

  /* Paned split — position initialised on first map */
  GtkWidget     *paned;
  gboolean       paned_init;

  GtkWidget     *members_list;
  GtkWidget     *member_count_label;
  GtkWidget     *add_member_btn;
  GtkWidget     *member_error;

  /* Delete */
  GtkWidget     *delete_btn;
  GtkWidget     *delete_error;

  SbvConnection *conn;
  SbvGroup      *selected_group;       /* owned ref */
  char          *filter_text;
  /* When non-NULL the next reload re-selects the row whose sAMAccountName
   * matches this string (and then clears it). Used by the create flow to
   * highlight the just-created group; for a normal reload (after save) we
   * fall back to selected_group's current sam. */
  char          *pending_select_sam;
};

G_DEFINE_TYPE (SbvGroupsPanel, sbv_groups_panel, GTK_TYPE_BOX)

/* ── Helpers ────────────────────────────────────────────────────────────── */

static char *
dn_to_friendly (const char *dn)
{
  if (!dn) return NULL;
  if (g_ascii_strncasecmp (dn, "CN=", 3) == 0) {
    const char *start = dn + 3;
    const char *comma = strchr (start, ',');
    return comma ? g_strndup (start, comma - start) : g_strdup (start);
  }
  return g_strdup (dn);
}

static const char *
group_scope_string (gint32 gt)
{
  if (gt & GT_BUILTIN)      return "Built-in Local";
  if (gt & GT_DOMAIN_LOCAL) return "Domain Local";
  if (gt & GT_GLOBAL)       return "Global";
  if (gt & GT_UNIVERSAL)    return "Universal";
  return "Unknown";
}

/* ── Forward declarations ───────────────────────────────────────────────── */
static void populate_members       (SbvGroupsPanel *self, SbvGroup *group);
static void show_add_member_dialog (SbvGroupsPanel *self);
static void populate_raw_attrs     (GtkWidget *box, GHashTable *attrs, int label_width);
void        sbv_groups_panel_load  (SbvGroupsPanel *self, SbvConnection *conn);

/* ── Row construction ───────────────────────────────────────────────────── */

static GtkWidget *
make_group_row (SbvGroup *group)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-group",
                           g_object_ref (group), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top    (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *icon = gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text_box, TRUE);

  const char *display = sbv_group_get_display_name (group);
  const char *sam     = sbv_group_get_sam (group);

  GtkWidget *name_lbl = gtk_label_new (display ? display : sam);
  gtk_label_set_xalign (GTK_LABEL (name_lbl), 0);
  gtk_widget_add_css_class (name_lbl, "heading");
  gtk_box_append (GTK_BOX (text_box), name_lbl);

  if (sam) {
    GtkWidget *sam_lbl = gtk_label_new (sam);
    gtk_label_set_xalign (GTK_LABEL (sam_lbl), 0);
    gtk_widget_add_css_class (sam_lbl, "dim-label");
    gtk_box_append (GTK_BOX (text_box), sam_lbl);
  }
  gtk_box_append (GTK_BOX (box), text_box);

  guint  count    = sbv_group_get_member_count (group);
  char  *count_s  = g_strdup_printf ("%u", count);
  GtkWidget *cnt  = gtk_label_new (count_s);
  g_free (count_s);
  gtk_widget_add_css_class (cnt, "dim-label");
  gtk_box_append (GTK_BOX (box), cnt);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* ── Filter ─────────────────────────────────────────────────────────────── */

static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  if (!self->filter_text || !*self->filter_text)
    return TRUE;

  SbvGroup *group = g_object_get_data (G_OBJECT (row), "sbv-group");
  if (!group) return TRUE;

  const char *fields[] = {
    sbv_group_get_display_name (group),
    sbv_group_get_sam (group),
    NULL
  };
  for (int i = 0; fields[i]; i++) {
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
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  g_free (self->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  self->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* ── Selection ──────────────────────────────────────────────────────────── */

static void
on_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);

  if (!row) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    return;
  }

  SbvGroup *group = g_object_get_data (G_OBJECT (row), "sbv-group");
  if (!group) return;

  g_set_object (&self->selected_group, group);

  const char *dn      = sbv_group_get_dn (group);
  const char *display = sbv_group_get_display_name (group);
  const char *sam     = sbv_group_get_sam (group);
  const char *desc    = sbv_group_get_description (group);
  gint32      gt      = sbv_group_get_group_type (group);

  gtk_label_set_text (GTK_LABEL (self->detail_dn),   dn      ? dn      : "");
  gtk_label_set_text (GTK_LABEL (self->detail_name), display ? display : (sam ? sam : ""));
  gtk_label_set_text (GTK_LABEL (self->detail_sam),  sam  ? sam  : "");
  gtk_label_set_text (GTK_LABEL (self->detail_desc), desc ? desc : "");
  gtk_label_set_text (GTK_LABEL (self->detail_type),
                      (gt & GT_SECURITY) ? "Security" : "Distribution");
  gtk_label_set_text (GTK_LABEL (self->detail_scope), group_scope_string (gt));

  /* RFC2307 / POSIX */
  gint gid = sbv_group_get_gid_number (group);
  char *gid_str = (gid >= 0) ? g_strdup_printf ("%d", gid) : g_strdup ("");
  gtk_editable_set_text (GTK_EDITABLE (self->rfc_gid_entry), gid_str);
  g_free (gid_str);
  gtk_widget_set_visible (self->rfc_error, FALSE);

  /* Cancel any in-flight collision debounce; the changed-handler will
   * rearm with the freshly-loaded value. */
  if (self->gid_check_timer) {
    g_source_remove (self->gid_check_timer);
    self->gid_check_timer = 0;
  }
  if (self->rfc_gid_collision) {
    gtk_label_set_text (GTK_LABEL (self->rfc_gid_collision), "");
    gtk_widget_set_visible (self->rfc_gid_collision, FALSE);
  }

  guint n_uid = sbv_group_get_member_uid_count (group);
  if (n_uid > 0) {
    const char * const *uids = sbv_group_get_member_uid (group);
    char *joined = g_strjoinv (", ", (char **) uids);
    gtk_label_set_text (GTK_LABEL (self->rfc_member_uid_label), joined);
    g_free (joined);
  } else {
    gtk_label_set_text (GTK_LABEL (self->rfc_member_uid_label), "\xe2\x80\x94");
  }

  /* Raw LDAP attributes */
  populate_raw_attrs (self->raw_attrs_box, sbv_group_get_ldap_attrs (group), 90);

  populate_members (self, group);

  gtk_label_set_text (GTK_LABEL (self->member_error), "");
  gtk_widget_set_visible (self->member_error, FALSE);
  if (self->delete_error) {
    gtk_label_set_text (GTK_LABEL (self->delete_error), "");
    gtk_widget_set_visible (self->delete_error, FALSE);
  }
  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "detail");
}

/* ── Member list population ─────────────────────────────────────────────── */

typedef struct {
  SbvGroupsPanel *panel;
  char           *member_dn;
} RemoveCtx;

static void
on_remove_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RemoveCtx      *ctx   = user_data;
  SbvGroupsPanel *self  = ctx->panel;
  SbvConnection  *conn  = SBV_CONNECTION (source);
  GError         *err   = NULL;

  if (!sbv_groups_remove_member_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->member_error), err->message);
    gtk_widget_set_visible (self->member_error, TRUE);
    g_error_free (err);
  } else {
    sbv_groups_panel_load (self, conn);
  }

  g_free (ctx->member_dn);
  g_free (ctx);
}

static void
on_remove_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  RemoveCtx *ctx = user_data;
  if (!ctx->panel->conn || !ctx->panel->selected_group) return;

  gtk_widget_set_visible (ctx->panel->member_error, FALSE);
  sbv_groups_remove_member_async (ctx->panel->conn, ctx->panel->selected_group,
                                   ctx->member_dn, NULL, on_remove_done,
                                   /* ctx is reused in callback */ ctx);
}

static void
populate_members (SbvGroupsPanel *self, SbvGroup *group)
{
  /* Clear old rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->members_list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->members_list), child);

  const char * const *members = sbv_group_get_members (group);
  guint n = sbv_group_get_member_count (group);

  char *cnt_str = g_strdup_printf ("Members (%u)", n);
  gtk_label_set_text (GTK_LABEL (self->member_count_label), cnt_str);
  g_free (cnt_str);

  if (!members) return;

  for (guint i = 0; members[i]; i++) {
    GtkWidget *row = gtk_list_box_row_new ();
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top    (box, 6);
    gtk_widget_set_margin_bottom (box, 6);
    gtk_widget_set_margin_start  (box, 12);
    gtk_widget_set_margin_end    (box, 8);

    char      *friendly = dn_to_friendly (members[i]);
    GtkWidget *lbl      = gtk_label_new (friendly ? friendly : members[i]);
    g_free (friendly);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand (lbl, TRUE);
    gtk_widget_set_tooltip_text (lbl, members[i]);
    gtk_box_append (GTK_BOX (box), lbl);

    GtkWidget *rm_btn = gtk_button_new_from_icon_name ("list-remove-symbolic");
    gtk_widget_add_css_class (rm_btn, "flat");
    gtk_widget_add_css_class (rm_btn, "destructive-action");
    gtk_widget_set_tooltip_text (rm_btn, "Remove from group");

    /* ctx freed in on_remove_done */
    RemoveCtx *ctx    = g_new0 (RemoveCtx, 1);
    ctx->panel        = self;
    ctx->member_dn    = g_strdup (members[i]);
    g_signal_connect (rm_btn, "clicked", G_CALLBACK (on_remove_clicked), ctx);

    gtk_box_append (GTK_BOX (box), rm_btn);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_append (GTK_LIST_BOX (self->members_list), row);
  }
}

/* ── Add member dialog ──────────────────────────────────────────────────── */

typedef struct {
  SbvGroupsPanel *panel;
  GtkWindow      *dialog;
  GtkWidget      *list_box;
  GtkWidget      *spinner;
  GtkWidget      *add_btn;
  GtkWidget      *search_entry;
  char           *filter_text;
  SbvUser        *selected;   /* unowned — points into row data */
} AddMemberCtx;

static void
add_ctx_free (AddMemberCtx *ctx)
{
  g_free (ctx->filter_text);
  g_free (ctx);
}

static gboolean
add_dialog_filter (GtkListBoxRow *row, gpointer user_data)
{
  AddMemberCtx *ctx = user_data;
  if (!ctx->filter_text || !*ctx->filter_text) return TRUE;

  SbvUser *user = g_object_get_data (G_OBJECT (row), "sbv-user");
  if (!user) return TRUE;

  const char *fields[] = {
    sbv_user_get_display_name (user),
    sbv_user_get_cn (user),
    sbv_user_get_sam (user),
    NULL
  };
  for (int i = 0; fields[i]; i++) {
    if (!fields[i]) continue;
    char *lower = g_utf8_strdown (fields[i], -1);
    gboolean m = strstr (lower, ctx->filter_text) != NULL;
    g_free (lower);
    if (m) return TRUE;
  }
  return FALSE;
}

static void
on_add_dialog_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  AddMemberCtx *ctx = user_data;
  g_free (ctx->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  ctx->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (ctx->list_box));
}

static void
on_add_dialog_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  AddMemberCtx *ctx = user_data;
  ctx->selected = row ? g_object_get_data (G_OBJECT (row), "sbv-user") : NULL;
  gtk_widget_set_sensitive (ctx->add_btn, ctx->selected != NULL);
}

typedef struct {
  SbvGroupsPanel *panel;
  GtkWindow      *dialog;
  char           *member_dn;   /* DN of user being added */
  char           *member_name; /* friendly name for UI */
} AddDoneCtx;

static void
on_add_member_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AddDoneCtx     *ctx  = user_data;
  SbvGroupsPanel *self = ctx->panel;
  SbvConnection  *conn = SBV_CONNECTION (source);
  GError         *err  = NULL;

  if (!sbv_groups_add_member_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->member_error), err->message);
    gtk_widget_set_visible (self->member_error, TRUE);
    g_error_free (err);
  } else {
    gtk_window_destroy (ctx->dialog);
    sbv_groups_panel_load (self, conn);
  }

  g_free (ctx->member_dn);
  g_free (ctx->member_name);
  g_free (ctx);
}

static void
on_add_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  AddMemberCtx *ctx = user_data;
  if (!ctx->selected || !ctx->panel->conn || !ctx->panel->selected_group) return;

  const char *dn = sbv_user_get_dn (ctx->selected);
  if (!dn) return;

  /* Friendly name for error reporting */
  const char *disp = sbv_user_get_display_name (ctx->selected);
  if (!disp || !*disp) disp = sbv_user_get_cn (ctx->selected);

  AddDoneCtx *done_ctx   = g_new0 (AddDoneCtx, 1);
  done_ctx->panel        = ctx->panel;
  done_ctx->dialog       = ctx->dialog;
  done_ctx->member_dn    = g_strdup (dn);
  done_ctx->member_name  = g_strdup (disp ? disp : dn);

  gtk_widget_set_sensitive (ctx->add_btn, FALSE);

  sbv_groups_add_member_async (ctx->panel->conn, ctx->panel->selected_group,
                                dn, NULL, on_add_member_done, done_ctx);
}

static void
on_add_dialog_users_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  AddMemberCtx *ctx = user_data;
  GError       *err = NULL;

  GListStore *store = sbv_users_list_finish (SBV_CONNECTION (source), result, &err);
  gtk_widget_set_visible (ctx->spinner, FALSE);

  if (!store) {
    g_warning ("Add-member: failed to load users: %s", err->message);
    g_error_free (err);
    return;
  }

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    SbvUser   *user = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget *row  = gtk_list_box_row_new ();
    g_object_set_data_full (G_OBJECT (row), "sbv-user",
                             g_object_ref (user), g_object_unref);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top    (box, 6);
    gtk_widget_set_margin_bottom (box, 6);
    gtk_widget_set_margin_start  (box, 12);
    gtk_widget_set_margin_end    (box, 12);

    const char *display = sbv_user_get_display_name (user);
    if (!display || !*display) display = sbv_user_get_cn (user);
    const char *sam = sbv_user_get_sam (user);

    GtkWidget *name_lbl = gtk_label_new (display ? display : sam);
    gtk_label_set_xalign (GTK_LABEL (name_lbl), 0);
    gtk_widget_set_hexpand (name_lbl, TRUE);
    gtk_box_append (GTK_BOX (box), name_lbl);

    if (sam) {
      GtkWidget *sam_lbl = gtk_label_new (sam);
      gtk_widget_add_css_class (sam_lbl, "dim-label");
      gtk_box_append (GTK_BOX (box), sam_lbl);
    }

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_append (GTK_LIST_BOX (ctx->list_box), row);
    g_object_unref (user);
  }

  g_object_unref (store);
}

static void
on_add_dialog_destroy (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  add_ctx_free (user_data);
}

static void
show_add_member_dialog (SbvGroupsPanel *self)
{
  if (!self->conn) return;

  AddMemberCtx *ctx = g_new0 (AddMemberCtx, 1);
  ctx->panel = self;

  /* Build dialog window */
  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Add Member");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 440, 500);
  GtkWidget *top_win = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  if (top_win)
    gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (top_win));

  ctx->dialog = GTK_WINDOW (win);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);
  g_signal_connect_swapped (cancel_btn, "clicked",
                             G_CALLBACK (gtk_window_destroy), win);

  ctx->add_btn = gtk_button_new_with_label ("Add");
  gtk_widget_add_css_class (ctx->add_btn, "suggested-action");
  gtk_widget_set_sensitive (ctx->add_btn, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->add_btn);
  g_signal_connect (ctx->add_btn, "clicked", G_CALLBACK (on_add_btn_clicked), ctx);

  gtk_box_append (GTK_BOX (vbox), header);

  /* Search */
  GtkWidget *search = gtk_search_entry_new ();
  gtk_widget_set_margin_top    (search, 8);
  gtk_widget_set_margin_bottom (search, 4);
  gtk_widget_set_margin_start  (search, 8);
  gtk_widget_set_margin_end    (search, 8);
  ctx->search_entry = search;
  g_signal_connect (search, "search-changed",
                    G_CALLBACK (on_add_dialog_search_changed), ctx);
  gtk_box_append (GTK_BOX (vbox), search);

  /* Spinner while loading */
  ctx->spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (ctx->spinner));
  gtk_widget_set_margin_top (ctx->spinner, 16);
  gtk_widget_set_halign (ctx->spinner, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (vbox), ctx->spinner);

  /* User list */
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  ctx->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (ctx->list_box),
                                    GTK_SELECTION_SINGLE);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (ctx->list_box),
                                 add_dialog_filter, ctx, NULL);
  gtk_widget_add_css_class (ctx->list_box, "boxed-list");
  g_signal_connect (ctx->list_box, "row-selected",
                    G_CALLBACK (on_add_dialog_row_selected), ctx);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), ctx->list_box);
  gtk_box_append (GTK_BOX (vbox), scroll);

  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect (win, "destroy", G_CALLBACK (on_add_dialog_destroy), ctx);
  gtk_window_present (GTK_WINDOW (win));

  /* Load users async */
  sbv_users_list_async (self->conn, NULL, on_add_dialog_users_loaded, ctx);
}

static void
on_add_member_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  show_add_member_dialog (SBV_GROUPS_PANEL (user_data));
}

/* ── GID auto-assignment (Suggest button) ───────────────────────────────── */

/* Per-suggest context. */
typedef struct {
  SbvGroupsPanel *panel;
  GtkWidget      *button;
} GroupSuggestCtx;

/* Async-completion: fill rfc_gid_entry with the suggested value. */
static void
on_group_suggest_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GroupSuggestCtx *ctx  = user_data;
  SbvGroupsPanel  *self = ctx->panel;
  SbvConnection   *conn = SBV_CONNECTION (source);
  GError          *err  = NULL;

  gint64 v = sbv_collisions_next_free_gid_finish (conn, result, &err);

  gtk_widget_set_sensitive (ctx->button, TRUE);

  if (err) {
    g_warning ("Suggest GID failed: %s", err->message);
    g_error_free (err);
  } else if (v < 0) {
    g_warning ("Suggest GID: configured range exhausted");
  } else {
    char buf[32];
    g_snprintf (buf, sizeof buf, "%" G_GINT64_FORMAT, v);
    gtk_editable_set_text (GTK_EDITABLE (self->rfc_gid_entry), buf);
  }

  g_free (ctx);
}

/* Issues the next-free-gid scan using the cached hints. */
static void
group_suggest_with_hints (SbvGroupsPanel *self)
{
  gint64 lo = self->idmap_hints->gid_min;
  gint64 hi = self->idmap_hints->gid_max;
  gint64 hint = self->idmap_hints->next_gid_hint;
  if (hint > lo && hint <= hi) lo = hint;

  GroupSuggestCtx *ctx = g_new0 (GroupSuggestCtx, 1);
  ctx->panel  = self;
  ctx->button = self->rfc_gid_suggest_btn;

  sbv_collisions_next_free_gid_async (self->conn, lo, hi,
                                       NULL, on_group_suggest_done, ctx);
}

/* Lazy-load context for the hints probe. */
typedef struct {
  SbvGroupsPanel *panel;
} GroupHintsThenSuggestCtx;

/* Completion of the hints probe — caches and resumes the suggest call. */
static void
on_group_hints_then_suggest (GObject *source, GAsyncResult *result,
                             gpointer user_data)
{
  GroupHintsThenSuggestCtx *ctx  = user_data;
  SbvGroupsPanel           *self = ctx->panel;
  SbvConnection            *conn = SBV_CONNECTION (source);
  GError                   *err  = NULL;

  SbvIdmapHints *hints = sbv_idmap_hints_query_finish (conn, result, &err);
  if (!hints) {
    g_warning ("Idmap hints probe failed: %s",
                err ? err->message : "(no detail)");
    if (err) g_error_free (err);
    SbvProfile *profile = sbv_connection_get_profile (conn);
    hints = sbv_idmap_hints_from_profile (profile);
  }

  g_clear_pointer (&self->idmap_hints, sbv_idmap_hints_free);
  self->idmap_hints = hints;

  group_suggest_with_hints (self);
  g_free (ctx);
}

/* "Suggest" button next to the GID entry. */
static void
on_group_suggest_gid_clicked (GtkButton *btn, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  if (!self->conn) return;
  gtk_widget_set_sensitive (GTK_WIDGET (btn), FALSE);

  if (self->idmap_hints) {
    group_suggest_with_hints (self);
    return;
  }

  GroupHintsThenSuggestCtx *ctx = g_new0 (GroupHintsThenSuggestCtx, 1);
  ctx->panel = self;

  SbvProfile *profile = sbv_connection_get_profile (self->conn);
  sbv_idmap_hints_query_async (self->conn, profile, NULL,
                                on_group_hints_then_suggest, ctx);
}

/* ── GID collision detection ────────────────────────────────────────────── */

#define GID_COLLISION_DEBOUNCE_MS 350

/* Per-check context. */
typedef struct {
  SbvGroupsPanel *panel;
  gint64          value;
} GidCollisionCtx;

/* Async-completion callback for the GID collision check. Drops the result
 * if the entry has since changed. */
static void
on_gid_collision_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GidCollisionCtx *ctx  = user_data;
  SbvGroupsPanel  *self = ctx->panel;
  SbvConnection   *conn = SBV_CONNECTION (source);
  GError          *err  = NULL;

  GPtrArray *hits = sbv_collisions_gid_check_finish (conn, result, &err);

  const char *now_text = gtk_editable_get_text (
    GTK_EDITABLE (self->rfc_gid_entry));
  gint64 now_val = *now_text ? g_ascii_strtoll (now_text, NULL, 10) : -1;

  if (now_val == ctx->value && self->rfc_gid_collision) {
    if (!hits || hits->len == 0) {
      gtk_label_set_text (GTK_LABEL (self->rfc_gid_collision), "");
      gtk_widget_set_visible (self->rfc_gid_collision, FALSE);
    } else {
      GString *s = g_string_new ("GID in use by ");
      guint shown = MIN (hits->len, 2u);
      for (guint i = 0; i < shown; i++) {
        SbvCollisionHit *h = g_ptr_array_index (hits, i);
        if (i > 0) g_string_append (s, ", ");
        g_string_append (s, h->sam ? h->sam : h->dn);
      }
      if (hits->len > shown)
        g_string_append_printf (s, " (+%u more)", hits->len - shown);
      gtk_label_set_text (GTK_LABEL (self->rfc_gid_collision), s->str);
      gtk_widget_set_visible (self->rfc_gid_collision, TRUE);
      g_string_free (s, TRUE);
    }
  }

  if (hits) sbv_collision_hits_free (hits);
  if (err)  g_error_free (err);
  g_free (ctx);
}

/* Fires the GID collision check for whatever's currently in rfc_gid_entry. */
static gboolean
fire_group_gid_check (gpointer user_data)
{
  SbvGroupsPanel *self = user_data;
  self->gid_check_timer = 0;

  if (!self->conn) return G_SOURCE_REMOVE;

  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->rfc_gid_entry));
  if (!*text) {
    gtk_label_set_text (GTK_LABEL (self->rfc_gid_collision), "");
    gtk_widget_set_visible (self->rfc_gid_collision, FALSE);
    return G_SOURCE_REMOVE;
  }

  gint64 gid = g_ascii_strtoll (text, NULL, 10);

  GidCollisionCtx *ctx = g_new0 (GidCollisionCtx, 1);
  ctx->panel = self;
  ctx->value = gid;

  const char *exclude = self->selected_group
    ? sbv_group_get_dn (self->selected_group) : NULL;

  sbv_collisions_gid_check_async (self->conn, gid, exclude,
                                   NULL, on_gid_collision_done, ctx);
  return G_SOURCE_REMOVE;
}

/* "changed" handler on rfc_gid_entry: debounce a collision check. */
static void
on_group_gid_entry_changed (GtkEditable *e, gpointer user_data)
{
  (void) e;
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  if (self->gid_check_timer)
    g_source_remove (self->gid_check_timer);
  self->gid_check_timer =
    g_timeout_add (GID_COLLISION_DEBOUNCE_MS, fire_group_gid_check, self);
}

/* ── Create / Delete ────────────────────────────────────────────────────── */

/* Called when the create-group dialog finishes successfully. Records the
 * sAMAccountName of the new group (extracted from the first RDN of the
 * new DN — for groups, CN equals sAMAccountName by AD convention) so the
 * subsequent reload can auto-select it. */
static void
on_group_created (SbvConnection *conn, const char *new_dn, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);

  if (new_dn && g_ascii_strncasecmp (new_dn, "CN=", 3) == 0) {
    const char *start = new_dn + 3;
    const char *comma = strchr (start, ',');
    g_free (self->pending_select_sam);
    self->pending_select_sam =
      comma ? g_strndup (start, comma - start) : g_strdup (start);
  }

  sbv_groups_panel_load (self, conn);
}

/* Toolbar "+" button: opens the create-group dialog. */
static void
on_add_group_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  if (!self->conn) return;

  GtkWidget *parent = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  sbv_create_group_dialog_show (parent ? GTK_WINDOW (parent) : NULL,
                                 self->conn, on_group_created, self);
}

/* Async-completion callback for the group delete operation. */
static void
on_group_delete_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  SbvConnection  *conn = SBV_CONNECTION (source);
  GError         *err  = NULL;

  gtk_widget_set_sensitive (self->delete_btn, TRUE);

  if (!sbv_groups_delete_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->delete_error), err->message);
    gtk_widget_set_visible (self->delete_error, TRUE);
    g_error_free (err);
    return;
  }

  /* Drop the now-stale selection so the reload doesn't try to re-pick a
   * deleted entry. */
  g_clear_object (&self->selected_group);
  sbv_groups_panel_load (self, conn);
}

/* Type-to-confirm dialog state. */
typedef struct {
  SbvGroupsPanel *panel;
  GtkWindow      *dialog;
  GtkWidget      *entry;
  GtkWidget      *delete_btn;
  char           *expected_sam;
} ConfirmDelGroupCtx;

/* Frees the per-dialog context. */
static void
confirm_del_group_ctx_free (ConfirmDelGroupCtx *ctx)
{
  g_free (ctx->expected_sam);
  g_free (ctx);
}

/* Enables the destructive button only when the typed text matches the
 * target group's sAMAccountName. */
static void
on_confirm_group_entry_changed (GtkEditable *editable, gpointer user_data)
{
  ConfirmDelGroupCtx *ctx   = user_data;
  const char         *typed = gtk_editable_get_text (editable);
  gtk_widget_set_sensitive (ctx->delete_btn,
                             g_str_equal (typed, ctx->expected_sam));
}

/* Confirm-button handler: dispatches the delete and closes the dialog. */
static void
on_confirm_group_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  ConfirmDelGroupCtx *ctx  = user_data;
  SbvGroupsPanel     *self = ctx->panel;
  if (!self->conn || !self->selected_group) {
    gtk_window_destroy (ctx->dialog);
    return;
  }

  gtk_widget_set_visible (self->delete_error, FALSE);
  gtk_widget_set_sensitive (self->delete_btn, FALSE);

  sbv_groups_delete_async (self->conn, self->selected_group,
                            NULL, on_group_delete_done, self);
  gtk_window_destroy (ctx->dialog);
}

/* Builds and presents the type-to-confirm modal for deleting a group. */
static void
show_group_delete_confirm (SbvGroupsPanel *self)
{
  if (!self->selected_group) return;
  const char *sam = sbv_group_get_sam (self->selected_group);
  if (!sam || !*sam) return;

  ConfirmDelGroupCtx *ctx = g_new0 (ConfirmDelGroupCtx, 1);
  ctx->panel        = self;
  ctx->expected_sam = g_strdup (sam);

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Delete Group");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 420, 220);
  GtkWidget *parent = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  if (parent)
    gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (parent));
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
                    G_CALLBACK (on_confirm_group_delete_clicked), ctx);

  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top    (body, 16);
  gtk_widget_set_margin_bottom (body, 16);
  gtk_widget_set_margin_start  (body, 20);
  gtk_widget_set_margin_end    (body, 20);

  char *warn_text = g_strdup_printf (
    "Permanently delete group <b>%s</b>?\n"
    "This cannot be undone. Members are not deleted.", sam);
  GtkWidget *warn = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (warn), warn_text);
  gtk_label_set_xalign (GTK_LABEL (warn), 0);
  gtk_label_set_wrap   (GTK_LABEL (warn), TRUE);
  g_free (warn_text);
  gtk_box_append (GTK_BOX (body), warn);

  char *prompt_text = g_strdup_printf (
    "Type the group name (<tt>%s</tt>) to confirm:", sam);
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
                    G_CALLBACK (on_confirm_group_entry_changed), ctx);
  gtk_box_append (GTK_BOX (body), ctx->entry);

  gtk_box_append (GTK_BOX (vbox), body);

  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect_swapped (win, "destroy",
                             G_CALLBACK (confirm_del_group_ctx_free), ctx);
  gtk_window_present (GTK_WINDOW (win));
  gtk_widget_grab_focus (ctx->entry);
}

/* Detail-pane "Delete" button: routes to the type-to-confirm dialog. */
static void
on_delete_group_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  show_group_delete_confirm (SBV_GROUPS_PANEL (user_data));
}

/* ── Load ───────────────────────────────────────────────────────────────── */

static void
on_groups_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  GError         *err  = NULL;

  GListStore *store = sbv_groups_list_finish (SBV_CONNECTION (source), result, &err);
  if (!store) {
    g_warning ("Failed to load groups: %s", err->message);
    g_error_free (err);
    gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "empty");
    return;
  }

  /* Decide which row to re-select after the rebuild. pending_select_sam
   * (set by the create flow) takes precedence; otherwise fall back to the
   * sam of whatever was selected before the reload (preserves selection
   * across "Save" actions). The delete flow clears selected_group before
   * triggering the reload, so this resolves to NULL there — exactly what
   * we want. */
  char *target_sam = NULL;
  if (self->pending_select_sam) {
    target_sam = self->pending_select_sam;        /* takes ownership */
    self->pending_select_sam = NULL;
  } else if (self->selected_group) {
    target_sam = g_strdup (sbv_group_get_sam (self->selected_group));
  }

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint          n        = g_list_model_get_n_items (G_LIST_MODEL (store));
  GtkListBoxRow *reselect = NULL;

  for (guint i = 0; i < n; i++) {
    SbvGroup  *group = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget *row   = make_group_row (group);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
    if (target_sam && g_str_equal (sbv_group_get_sam (group) ?: "", target_sam))
      reselect = GTK_LIST_BOX_ROW (row);
    g_object_unref (group);
  }

  g_object_unref (store);
  g_free (target_sam);

  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack),
                                     n == 0 ? "empty" : "split");

  if (reselect)
    gtk_list_box_select_row (GTK_LIST_BOX (self->list_box), reselect);
  else
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
}

void
sbv_groups_panel_load (SbvGroupsPanel *self, SbvConnection *conn)
{
  /* Drop cached hints when the connection changes — they're tied to the
   * profile's range and the DC's published values. */
  if (self->conn != conn)
    g_clear_pointer (&self->idmap_hints, sbv_idmap_hints_free);

  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "loading");
  sbv_groups_list_async (conn, NULL, on_groups_loaded, self);
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
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  if (!self->paned_init) {
    int w = gtk_widget_get_width (widget);
    if (w > 0) {
      self->paned_init = TRUE;
      gtk_paned_set_position (GTK_PANED (widget), 280);
    }
  }
}

/* ── RFC2307 save ───────────────────────────────────────────────────────── */

static void
on_rfc_save_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  SbvConnection  *conn = SBV_CONNECTION (source);
  GError         *err  = NULL;

  gtk_widget_set_sensitive (self->rfc_save_btn, TRUE);

  if (!sbv_groups_set_unix_attrs_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->rfc_error), err->message);
    gtk_widget_set_visible (self->rfc_error, TRUE);
    g_error_free (err);
    return;
  }

  gtk_widget_set_visible (self->rfc_error, FALSE);
  sbv_groups_panel_load (self, conn);
}

static void
on_rfc_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (user_data);
  if (!self->selected_group || !self->conn) return;

  gtk_widget_set_visible (self->rfc_error, FALSE);
  gtk_widget_set_sensitive (self->rfc_save_btn, FALSE);

  const char *gid_s = gtk_editable_get_text (GTK_EDITABLE (self->rfc_gid_entry));
  gint gid = *gid_s ? (gint) strtol (gid_s, NULL, 10) : -1;

  sbv_groups_set_unix_attrs_async (self->conn, self->selected_group, gid,
                                    NULL, on_rfc_save_done, self);
}

/* ── Helper: detail info row ────────────────────────────────────────────── */

static GtkWidget *
make_info_row (const char *label_text, GtkWidget **out_label)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (box, 3);
  gtk_widget_set_margin_bottom (box, 3);

  GtkWidget *lbl = gtk_label_new (label_text);
  gtk_label_set_xalign (GTK_LABEL (lbl), 1);
  gtk_widget_set_size_request (lbl, 90, -1);
  gtk_widget_add_css_class (lbl, "dim-label");
  gtk_box_append (GTK_BOX (box), lbl);

  GtkWidget *val = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (val), 0);
  gtk_label_set_ellipsize (GTK_LABEL (val), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (val, TRUE);
  gtk_box_append (GTK_BOX (box), val);

  if (out_label) *out_label = val;
  return box;
}

/* ── GObject init ───────────────────────────────────────────────────────── */

static void
sbv_groups_panel_finalize (GObject *object)
{
  SbvGroupsPanel *self = SBV_GROUPS_PANEL (object);
  g_free (self->filter_text);
  g_free (self->pending_select_sam);
  if (self->gid_check_timer) g_source_remove (self->gid_check_timer);
  g_clear_pointer (&self->idmap_hints, sbv_idmap_hints_free);
  g_clear_object (&self->selected_group);
  G_OBJECT_CLASS (sbv_groups_panel_parent_class)->finalize (object);
}

static void sbv_groups_panel_class_init (SbvGroupsPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_groups_panel_finalize;
}

static void
sbv_groups_panel_init (SbvGroupsPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->outer_stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->outer_stack, TRUE);

  /* Loading */
  {
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    GtkWidget *sp = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (sp));
    gtk_box_append (GTK_BOX (box), sp);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), box, "loading");
  }

  /* Empty */
  {
    GtkWidget *empty = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty),
                                    "avatar-default-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Groups Found");
    gtk_stack_add_named (GTK_STACK (self->outer_stack), empty, "empty");
  }

  /* Split */
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
    gtk_widget_set_tooltip_text (self->search_entry, "Search groups");
    g_signal_connect (self->search_entry, "search-changed",
                      G_CALLBACK (on_search_changed), self);
    gtk_box_append (GTK_BOX (toolbar), self->search_entry);

    self->add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_add_css_class (self->add_btn, "flat");
    gtk_widget_set_tooltip_text (self->add_btn, "Create group");
    g_signal_connect (self->add_btn, "clicked",
                      G_CALLBACK (on_add_group_btn_clicked), self);
    gtk_box_append (GTK_BOX (toolbar), self->add_btn);

    gtk_box_append (GTK_BOX (left_box), toolbar);
    gtk_box_append (GTK_BOX (left_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *left_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (left_scroll, TRUE);
    gtk_widget_set_size_request (left_scroll, 260, -1);

    self->list_box = gtk_list_box_new ();
    gtk_list_box_set_filter_func (GTK_LIST_BOX (self->list_box),
                                   filter_func, self, NULL);
    gtk_widget_add_css_class (self->list_box, "boxed-list");
    g_signal_connect (self->list_box, "row-selected",
                      G_CALLBACK (on_row_selected), self);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (left_scroll),
                                    self->list_box);
    gtk_box_append (GTK_BOX (left_box), left_scroll);
    gtk_paned_set_start_child (GTK_PANED (paned), left_box);

    /* ── Right: detail stack ── */
    self->detail_stack = gtk_stack_new ();
    gtk_widget_set_hexpand (self->detail_stack, TRUE);

    /* "none" page */
    GtkWidget *none_page = adw_status_page_new ();
    adw_status_page_set_title (ADW_STATUS_PAGE (none_page), "Select a Group");
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (none_page),
                                    "avatar-default-symbolic");
    gtk_stack_add_named (GTK_STACK (self->detail_stack), none_page, "none");

    /* "detail" page */
    GtkWidget *detail_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (detail_scroll, TRUE);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *detail_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top    (detail_box, 16);
    gtk_widget_set_margin_bottom (detail_box, 16);
    gtk_widget_set_margin_start  (detail_box, 16);
    gtk_widget_set_margin_end    (detail_box, 16);

    /* ── Group details (read-only) ── */
    GtkWidget *info_lbl = gtk_label_new ("Group Details");
    gtk_label_set_xalign (GTK_LABEL (info_lbl), 0);
    gtk_widget_add_css_class (info_lbl, "heading");
    gtk_box_append (GTK_BOX (detail_box), info_lbl);

    {
      GtkWidget *dn_row = make_info_row ("DN", &self->detail_dn);
      /* Make the DN value selectable and monospace for easy copying */
      gtk_label_set_selectable (GTK_LABEL (self->detail_dn), TRUE);
      gtk_label_set_ellipsize  (GTK_LABEL (self->detail_dn), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_add_css_class (self->detail_dn, "monospace");
      gtk_box_append (GTK_BOX (detail_box), dn_row);
    }
    gtk_box_append (GTK_BOX (detail_box),
                    make_info_row ("Name",        &self->detail_name));
    gtk_box_append (GTK_BOX (detail_box),
                    make_info_row ("Username",    &self->detail_sam));
    gtk_box_append (GTK_BOX (detail_box),
                    make_info_row ("Description", &self->detail_desc));
    gtk_box_append (GTK_BOX (detail_box),
                    make_info_row ("Type",        &self->detail_type));
    gtk_box_append (GTK_BOX (detail_box),
                    make_info_row ("Scope",       &self->detail_scope));

    /* ── RFC2307 / POSIX ── */
    gtk_box_append (GTK_BOX (detail_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    {
      GtkWidget *rfc_lbl = gtk_label_new ("Unix Attributes (RFC2307)");
      gtk_label_set_xalign (GTK_LABEL (rfc_lbl), 0);
      gtk_widget_add_css_class (rfc_lbl, "heading");
      gtk_box_append (GTK_BOX (detail_box), rfc_lbl);
    }

    {
      self->rfc_gid_entry = gtk_entry_new ();
      gtk_entry_set_placeholder_text (GTK_ENTRY (self->rfc_gid_entry), "GID number");
      gtk_widget_add_css_class (self->rfc_gid_entry, "monospace");
      g_signal_connect (self->rfc_gid_entry, "changed",
                        G_CALLBACK (on_group_gid_entry_changed), self);

      self->rfc_gid_suggest_btn = gtk_button_new_with_label ("Suggest");
      gtk_widget_set_tooltip_text (self->rfc_gid_suggest_btn,
                                    "Fill with the lowest unused GID in the configured range");
      g_signal_connect (self->rfc_gid_suggest_btn, "clicked",
                        G_CALLBACK (on_group_suggest_gid_clicked), self);

      GtkWidget *lbl = gtk_label_new ("GID Number");
      gtk_label_set_xalign (GTK_LABEL (lbl), 1.0);
      gtk_widget_set_size_request (lbl, 90, -1);
      gtk_widget_add_css_class (lbl, "dim-label");
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_margin_top    (row, 3);
      gtk_widget_set_margin_bottom (row, 3);
      gtk_widget_set_hexpand (self->rfc_gid_entry, TRUE);
      gtk_box_append (GTK_BOX (row), lbl);
      gtk_box_append (GTK_BOX (row), self->rfc_gid_entry);
      gtk_box_append (GTK_BOX (row), self->rfc_gid_suggest_btn);
      gtk_box_append (GTK_BOX (detail_box), row);

      self->rfc_gid_collision = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (self->rfc_gid_collision), 0);
      gtk_label_set_wrap (GTK_LABEL (self->rfc_gid_collision), TRUE);
      gtk_widget_add_css_class (self->rfc_gid_collision, "warning");
      gtk_widget_set_margin_start (self->rfc_gid_collision, 98);
      gtk_widget_set_visible (self->rfc_gid_collision, FALSE);
      gtk_box_append (GTK_BOX (detail_box), self->rfc_gid_collision);
    }

    {
      GtkWidget *row = make_info_row ("Member UIDs", &self->rfc_member_uid_label);
      gtk_label_set_ellipsize (GTK_LABEL (self->rfc_member_uid_label), PANGO_ELLIPSIZE_END);
      gtk_label_set_text (GTK_LABEL (self->rfc_member_uid_label), "\xe2\x80\x94");
      gtk_box_append (GTK_BOX (detail_box), row);
    }

    {
      GtkWidget *rfc_btn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_top (rfc_btn_box, 4);
      self->rfc_save_btn = gtk_button_new_with_label ("Save Unix Attributes");
      gtk_widget_add_css_class (self->rfc_save_btn, "suggested-action");
      gtk_widget_set_halign (self->rfc_save_btn, GTK_ALIGN_START);
      gtk_widget_set_margin_start (self->rfc_save_btn, 98);
      g_signal_connect (self->rfc_save_btn, "clicked",
                        G_CALLBACK (on_rfc_save_clicked), self);
      gtk_box_append (GTK_BOX (rfc_btn_box), self->rfc_save_btn);

      self->rfc_error = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (self->rfc_error), 0);
      gtk_label_set_wrap (GTK_LABEL (self->rfc_error), TRUE);
      gtk_widget_add_css_class (self->rfc_error, "error");
      gtk_widget_set_margin_start (self->rfc_error, 98);
      gtk_widget_set_visible (self->rfc_error, FALSE);
      gtk_box_append (GTK_BOX (rfc_btn_box), self->rfc_error);
      gtk_box_append (GTK_BOX (detail_box), rfc_btn_box);
    }

    /* ── All LDAP Attributes ── */
    gtk_box_append (GTK_BOX (detail_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    {
      GtkWidget *raw_lbl = gtk_label_new ("All LDAP Attributes");
      gtk_label_set_xalign (GTK_LABEL (raw_lbl), 0);
      gtk_widget_add_css_class (raw_lbl, "heading");
      gtk_box_append (GTK_BOX (detail_box), raw_lbl);
    }

    self->raw_attrs_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (detail_box), self->raw_attrs_box);

    gtk_box_append (GTK_BOX (detail_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* ── Members header row ── */
    GtkWidget *mem_header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top (mem_header, 4);

    self->member_count_label = gtk_label_new ("Members");
    gtk_label_set_xalign (GTK_LABEL (self->member_count_label), 0);
    gtk_widget_add_css_class (self->member_count_label, "heading");
    gtk_widget_set_hexpand (self->member_count_label, TRUE);
    gtk_box_append (GTK_BOX (mem_header), self->member_count_label);

    self->add_member_btn = gtk_button_new_with_label ("Add Member");
    gtk_widget_add_css_class (self->add_member_btn, "suggested-action");
    g_signal_connect (self->add_member_btn, "clicked",
                      G_CALLBACK (on_add_member_btn_clicked), self);
    gtk_box_append (GTK_BOX (mem_header), self->add_member_btn);

    gtk_box_append (GTK_BOX (detail_box), mem_header);

    /* Error label */
    self->member_error = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->member_error), 0);
    gtk_label_set_wrap (GTK_LABEL (self->member_error), TRUE);
    gtk_widget_add_css_class (self->member_error, "error");
    gtk_widget_set_visible (self->member_error, FALSE);
    gtk_box_append (GTK_BOX (detail_box), self->member_error);

    /* Members list */
    GtkWidget *members_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (members_scroll, TRUE);
    self->members_list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->members_list),
                                      GTK_SELECTION_NONE);
    gtk_widget_add_css_class (self->members_list, "boxed-list");
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (members_scroll),
                                    self->members_list);
    gtk_box_append (GTK_BOX (detail_box), members_scroll);

    /* ── Danger zone ── */
    gtk_box_append (GTK_BOX (detail_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    {
      GtkWidget *dz_lbl = gtk_label_new ("Danger Zone");
      gtk_label_set_xalign (GTK_LABEL (dz_lbl), 0);
      gtk_widget_add_css_class (dz_lbl, "heading");
      gtk_widget_set_margin_top (dz_lbl, 8);
      gtk_box_append (GTK_BOX (detail_box), dz_lbl);

      GtkWidget *del_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_top (del_box, 4);

      self->delete_btn = gtk_button_new_with_label ("Delete Group…");
      gtk_widget_add_css_class (self->delete_btn, "destructive-action");
      gtk_widget_set_halign (self->delete_btn, GTK_ALIGN_START);
      g_signal_connect (self->delete_btn, "clicked",
                        G_CALLBACK (on_delete_group_btn_clicked), self);
      gtk_box_append (GTK_BOX (del_box), self->delete_btn);

      self->delete_error = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (self->delete_error), 0);
      gtk_label_set_wrap (GTK_LABEL (self->delete_error), TRUE);
      gtk_widget_add_css_class (self->delete_error, "error");
      gtk_widget_set_visible (self->delete_error, FALSE);
      gtk_box_append (GTK_BOX (del_box), self->delete_error);
      gtk_box_append (GTK_BOX (detail_box), del_box);
    }

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (detail_scroll), detail_box);
    gtk_stack_add_named (GTK_STACK (self->detail_stack), detail_scroll, "detail");

    gtk_paned_set_end_child (GTK_PANED (paned), self->detail_stack);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), paned, "split");
  }

  gtk_box_append (GTK_BOX (self), self->outer_stack);
}

GtkWidget *
sbv_groups_panel_new (void)
{
  return g_object_new (SBV_TYPE_GROUPS_PANEL, NULL);
}
