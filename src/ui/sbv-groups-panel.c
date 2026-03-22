#include "sbv-groups-panel.h"
#include "../backend/sbv-groups-backend.h"
#include "../backend/sbv-users-backend.h"
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
  GtkWidget     *list_box;

  /* Detail pane */
  GtkWidget     *detail_stack;   /* "none" | "detail" */
  GtkWidget     *detail_name;
  GtkWidget     *detail_sam;
  GtkWidget     *detail_desc;
  GtkWidget     *detail_type;
  GtkWidget     *detail_scope;
  GtkWidget     *members_list;
  GtkWidget     *member_count_label;
  GtkWidget     *add_member_btn;
  GtkWidget     *member_error;

  SbvConnection *conn;
  SbvGroup      *selected_group;  /* owned ref */
  char          *filter_text;
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
static void populate_members (SbvGroupsPanel *self, SbvGroup *group);
static void show_add_member_dialog (SbvGroupsPanel *self);

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

  const char *display = sbv_group_get_display_name (group);
  const char *sam     = sbv_group_get_sam (group);
  const char *desc    = sbv_group_get_description (group);
  gint32      gt      = sbv_group_get_group_type (group);

  gtk_label_set_text (GTK_LABEL (self->detail_name), display ? display : (sam ? sam : ""));
  gtk_label_set_text (GTK_LABEL (self->detail_sam),  sam  ? sam  : "");
  gtk_label_set_text (GTK_LABEL (self->detail_desc), desc ? desc : "");
  gtk_label_set_text (GTK_LABEL (self->detail_type),
                      (gt & GT_SECURITY) ? "Security" : "Distribution");
  gtk_label_set_text (GTK_LABEL (self->detail_scope), group_scope_string (gt));

  populate_members (self, group);

  gtk_label_set_text (GTK_LABEL (self->member_error), "");
  gtk_widget_set_visible (self->member_error, FALSE);
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
  } else if (self->selected_group) {
    /* Remove from model */
    const char * const *members = sbv_group_get_members (self->selected_group);
    guint n = sbv_group_get_member_count (self->selected_group);
    char **new_members = g_new0 (char *, n);  /* at most n-1 + NULL */
    guint  j = 0;
    for (guint i = 0; i < n; i++) {
      if (!g_str_equal (members[i], ctx->member_dn))
        new_members[j++] = g_strdup (members[i]);
    }
    new_members[j] = NULL;
    sbv_group_set_members (self->selected_group, new_members);
    populate_members (self, self->selected_group);
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
  } else if (self->selected_group) {
    /* Append to model */
    guint  n          = sbv_group_get_member_count (self->selected_group);
    char **new_members = g_new0 (char *, n + 2);
    const char * const *old = sbv_group_get_members (self->selected_group);
    for (guint i = 0; i < n; i++)
      new_members[i] = g_strdup (old[i]);
    new_members[n]     = g_strdup (ctx->member_dn);
    new_members[n + 1] = NULL;
    sbv_group_set_members (self->selected_group, new_members);
    populate_members (self, self->selected_group);
    gtk_window_destroy (ctx->dialog);
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

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    SbvGroup *group = g_list_model_get_item (G_LIST_MODEL (store), i);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), make_group_row (group));
    g_object_unref (group);
  }

  g_object_unref (store);
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack),
                                     n == 0 ? "empty" : "split");
  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
}

void
sbv_groups_panel_load (SbvGroupsPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "loading");
  sbv_groups_list_async (conn, NULL, on_groups_loaded, self);
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
    gtk_widget_set_tooltip_text (self->search_entry, "Search groups");
    g_signal_connect (self->search_entry, "search-changed",
                      G_CALLBACK (on_search_changed), self);
    gtk_box_append (GTK_BOX (toolbar), self->search_entry);
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
