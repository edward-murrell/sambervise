#include "sbv-member-of.h"
#include "../backend/sbv-groups-backend.h"
#include "../backend/sbv-dn.h"
#include "../model/sbv-group.h"
#include "../model/sbv-search.h"

#include <adwaita.h>

struct _SbvMemberOf {
  GtkBox         parent;

  GtkWidget     *primary_label;
  GtkWidget     *list_box;
  GtkWidget     *error_label;
  GtkWidget     *add_btn;

  SbvConnection *conn;       /* owned ref */
  char          *dn;         /* the user or computer */
  char         **member_of;  /* memberOf DNs */
  gint           primary_rid; /* primary group RID; -1 = unknown */
};

G_DEFINE_TYPE (SbvMemberOf, sbv_member_of, GTK_TYPE_BOX)

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* Shows an error under the list, or hides it when `msg` is NULL. */
static void
show_error (SbvMemberOf *self, const char *msg)
{
  gtk_label_set_text (GTK_LABEL (self->error_label), msg ? msg : "");
  gtk_widget_set_visible (self->error_label, msg != NULL);
}

/* TRUE when the object is already a member of the group at `group_dn`. */
static gboolean
is_member (SbvMemberOf *self, const char *group_dn)
{
  for (char **p = self->member_of; p && *p; p++)
    if (sbv_dn_equal (*p, group_dn)) return TRUE;
  return FALSE;
}

/* Builds a temporary SbvGroup carrying only a DN, for the groups backend
 * calls. Caller unrefs. */
static SbvGroup *
group_for_dn (const char *dn)
{
  SbvGroup *g = sbv_group_new ();
  sbv_group_set_dn (g, dn);
  return g;
}

/* ── Remove ─────────────────────────────────────────────────────────────── */

/* Remove completion: report the error, or tell the panel to reload. */
static void
on_remove_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvMemberOf *self = SBV_MEMBER_OF (user_data);
  GError      *err  = NULL;

  gtk_widget_set_sensitive (self->list_box, TRUE);
  if (!sbv_groups_remove_member_finish (SBV_CONNECTION (source), result, &err)) {
    show_error (self, err->message);
    g_error_free (err);
  } else {
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  }
  g_object_unref (self);
}

/* Remove button: takes the object out of the row's group. */
static void
on_remove_clicked (GtkButton *btn, gpointer user_data)
{
  SbvMemberOf *self     = SBV_MEMBER_OF (user_data);
  const char  *group_dn = g_object_get_data (G_OBJECT (btn), "group-dn");
  if (!self->conn || !self->dn || !group_dn) return;

  show_error (self, NULL);
  gtk_widget_set_sensitive (self->list_box, FALSE);
  SbvGroup *group = group_for_dn (group_dn);
  sbv_groups_remove_member_async (self->conn, group, self->dn, NULL,
                                  on_remove_done, g_object_ref (self));
  g_object_unref (group);
}

/* ── Group picker dialog ────────────────────────────────────────────────── */

/* Shared state for one picker. Reference counted with GRcBox: the dialog
 * holds one reference and each pending LDAP call holds another, so the
 * state outlives whichever finishes last. With an `owner` the picker adds
 * the owner's object itself. Without one it hands the group to
 * `pick_cb`. */
typedef struct {
  SbvMemberOf      *owner;  /* owned ref, or NULL in pick mode */
  SbvConnection    *conn;   /* owned ref */
  SbvGroupPickedFn  pick_cb;
  gpointer          pick_ud;
  GDestroyNotify    pick_destroy;
  GtkWindow   *dialog;      /* NULL once the dialog is gone */
  GtkWidget   *list_box;
  GtkWidget   *spinner;
  GtkWidget   *add_btn;
  GtkWidget   *error_label;
  char        *filter_text;
} PickCtx;

/* Frees the picker state's contents when the last reference drops. */
static void
pick_ctx_clear (gpointer data)
{
  PickCtx *ctx = data;
  g_clear_object (&ctx->owner);
  g_clear_object (&ctx->conn);
  if (ctx->pick_destroy) ctx->pick_destroy (ctx->pick_ud);
  g_free (ctx->filter_text);
}

/* Drops one reference to the picker state. */
static void
pick_ctx_release (PickCtx *ctx)
{
  g_rc_box_release_full (ctx, pick_ctx_clear);
}

/* Shows a group when the search text matches its name, sAMAccountName
 * or description. */
static gboolean
pick_filter (GtkListBoxRow *row, gpointer user_data)
{
  PickCtx  *ctx   = user_data;
  SbvGroup *group = g_object_get_data (G_OBJECT (row), "sbv-group");
  if (!group) return TRUE;

  const char *fields[] = {
    sbv_group_get_display_name (group),
    sbv_group_get_sam (group),
    sbv_group_get_description (group),
  };
  return sbv_search_matches (ctx->filter_text, fields, G_N_ELEMENTS (fields));
}

/* Re-runs the picker's filter when its search entry changes. */
static void
on_pick_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  PickCtx *ctx = user_data;
  g_free (ctx->filter_text);
  ctx->filter_text = sbv_search_fold (gtk_editable_get_text (GTK_EDITABLE (entry)));
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (ctx->list_box));
}

/* Enables Add only while a group is selected. Connected with
 * g_signal_connect_object on the button, so GLib drops it once the button
 * is gone. The list box can outlive the button while the dialog closes. */
static void
on_pick_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  gtk_widget_set_sensitive (GTK_WIDGET (user_data), row != NULL);
}

/* Fills the picker with every group the object isn't already in,
 * including its primary group. */
static void
on_pick_groups_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  PickCtx *ctx = user_data;
  GError  *err = NULL;

  GListStore *store = sbv_groups_list_finish (SBV_CONNECTION (source), result, &err);
  if (!ctx->dialog) {
    g_clear_error (&err);
    g_clear_object (&store);
    pick_ctx_release (ctx);
    return;
  }

  gtk_widget_set_visible (ctx->spinner, FALSE);
  if (!store) {
    gtk_label_set_text (GTK_LABEL (ctx->error_label), err->message);
    gtk_widget_set_visible (ctx->error_label, TRUE);
    g_error_free (err);
    pick_ctx_release (ctx);
    return;
  }

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    SbvGroup *group = g_list_model_get_item (G_LIST_MODEL (store), i);
    gint64 rid = sbv_group_get_rid (group);
    if (ctx->owner
        && (is_member (ctx->owner, sbv_group_get_dn (group))
            || (rid >= 0 && rid == ctx->owner->primary_rid))) {
      g_object_unref (group);
      continue;
    }

    GtkWidget *row = gtk_list_box_row_new ();
    g_object_set_data_full (G_OBJECT (row), "sbv-group",
                            g_object_ref (group), g_object_unref);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top    (box, 6);
    gtk_widget_set_margin_bottom (box, 6);
    gtk_widget_set_margin_start  (box, 12);
    gtk_widget_set_margin_end    (box, 12);

    const char *sam  = sbv_group_get_sam (group);
    const char *name = sbv_group_get_display_name (group);
    if (!name || !*name) name = sam;

    GtkWidget *name_lbl = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (name_lbl), 0);
    gtk_widget_set_hexpand (name_lbl, TRUE);
    gtk_box_append (GTK_BOX (box), name_lbl);

    if (sam && g_strcmp0 (sam, name) != 0) {
      GtkWidget *sam_lbl = gtk_label_new (sam);
      gtk_widget_add_css_class (sam_lbl, "dim-label");
      gtk_box_append (GTK_BOX (box), sam_lbl);
    }

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_append (GTK_LIST_BOX (ctx->list_box), row);
    g_object_unref (group);
  }
  g_object_unref (store);
  pick_ctx_release (ctx);
}

/* Add completion: close the picker and tell the panel to reload, or show
 * the error in the picker. */
static void
on_pick_add_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  PickCtx *ctx = user_data;
  GError  *err = NULL;

  if (!sbv_groups_add_member_finish (SBV_CONNECTION (source), result, &err)) {
    if (ctx->dialog) {
      gtk_label_set_text (GTK_LABEL (ctx->error_label), err->message);
      gtk_widget_set_visible (ctx->error_label, TRUE);
      gtk_widget_set_sensitive (ctx->add_btn, TRUE);
    } else {
      show_error (ctx->owner, err->message);
    }
    g_error_free (err);
  } else {
    if (ctx->dialog) gtk_window_destroy (ctx->dialog);
    g_signal_emit (ctx->owner, signals[SIGNAL_CHANGED], 0);
  }
  pick_ctx_release (ctx);
}

/* Add button: adds the object to the selected group. */
static void
on_pick_add_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  PickCtx       *ctx   = user_data;
  SbvMemberOf   *owner = ctx->owner;
  GtkListBoxRow *row   = gtk_list_box_get_selected_row (GTK_LIST_BOX (ctx->list_box));
  SbvGroup      *group = row ? g_object_get_data (G_OBJECT (row), "sbv-group") : NULL;
  if (!group) return;

  /* Pick mode: close, then hand the group to the caller. */
  if (!owner) {
    g_object_ref (group);
    PickCtx *keep = g_rc_box_acquire (ctx);
    gtk_window_destroy (ctx->dialog);
    if (keep->pick_cb) keep->pick_cb (group, keep->pick_ud);
    pick_ctx_release (keep);
    g_object_unref (group);
    return;
  }
  if (!owner->conn || !owner->dn) return;

  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_widget_set_sensitive (ctx->add_btn, FALSE);
  sbv_groups_add_member_async (owner->conn, group, owner->dn, NULL,
                               on_pick_add_done, g_rc_box_acquire (ctx));
}

/* Dialog "destroy": marks the dialog gone and drops its reference. */
static void
on_pick_dialog_destroy (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  PickCtx *ctx = user_data;
  ctx->dialog = NULL;
  pick_ctx_release (ctx);
}

/* Builds and presents the modal group picker for `ctx`, then starts
 * loading the groups. Takes over the caller's reference to `ctx`. */
static void
present_picker (PickCtx *ctx, GtkWindow *parent)
{
  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Add to Group");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 440, 500);
  if (parent) {
    gtk_window_set_transient_for (GTK_WINDOW (win), parent);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (win), TRUE);
  }
  ctx->dialog = GTK_WINDOW (win);

  GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel = gtk_button_new_with_label ("Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (gtk_window_destroy), win);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);

  ctx->add_btn = gtk_button_new_with_label ("Add");
  gtk_widget_add_css_class (ctx->add_btn, "suggested-action");
  gtk_widget_set_sensitive (ctx->add_btn, FALSE);
  g_signal_connect (ctx->add_btn, "clicked", G_CALLBACK (on_pick_add_clicked), ctx);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->add_btn);
  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *search = gtk_search_entry_new ();
  gtk_widget_set_margin_top    (search, 8);
  gtk_widget_set_margin_bottom (search, 4);
  gtk_widget_set_margin_start  (search, 8);
  gtk_widget_set_margin_end    (search, 8);
  g_signal_connect (search, "search-changed", G_CALLBACK (on_pick_search_changed), ctx);
  gtk_box_append (GTK_BOX (vbox), search);

  ctx->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (ctx->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (ctx->error_label), 0);
  gtk_widget_add_css_class (ctx->error_label, "error");
  gtk_widget_set_margin_start (ctx->error_label, 8);
  gtk_widget_set_margin_end   (ctx->error_label, 8);
  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_box_append (GTK_BOX (vbox), ctx->error_label);

  ctx->spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (ctx->spinner));
  gtk_widget_set_margin_top (ctx->spinner, 16);
  gtk_widget_set_halign (ctx->spinner, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (vbox), ctx->spinner);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  ctx->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (ctx->list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (ctx->list_box), pick_filter, ctx, NULL);
  gtk_widget_add_css_class (ctx->list_box, "boxed-list");
  g_signal_connect_object (ctx->list_box, "row-selected",
                           G_CALLBACK (on_pick_row_selected), ctx->add_btn, 0);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), ctx->list_box);
  gtk_box_append (GTK_BOX (vbox), scroll);

  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect (win, "destroy", G_CALLBACK (on_pick_dialog_destroy), ctx);
  gtk_window_present (GTK_WINDOW (win));

  sbv_groups_list_async (ctx->conn, NULL, on_pick_groups_loaded,
                         g_rc_box_acquire (ctx));
}

/* Opens the picker that adds this section's object to a group. */
static void
show_picker (SbvMemberOf *self)
{
  PickCtx *ctx = g_rc_box_new0 (PickCtx);
  ctx->owner   = g_object_ref (self);
  ctx->conn    = g_object_ref (self->conn);
  GtkWidget *top = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  present_picker (ctx, top ? GTK_WINDOW (top) : NULL);
}

void
sbv_member_of_pick_group (GtkWindow        *parent,
                          SbvConnection    *conn,
                          SbvGroupPickedFn  callback,
                          gpointer          user_data,
                          GDestroyNotify    user_data_free)
{
  if (!conn) {
    if (user_data_free) user_data_free (user_data);
    return;
  }
  PickCtx *ctx      = g_rc_box_new0 (PickCtx);
  ctx->conn         = g_object_ref (conn);
  ctx->pick_cb      = callback;
  ctx->pick_ud      = user_data;
  ctx->pick_destroy = user_data_free;
  present_picker (ctx, parent);
}

/* "Add to Group…" button. */
static void
on_add_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvMemberOf *self = SBV_MEMBER_OF (user_data);
  if (!self->conn || !self->dn) return;
  show_error (self, NULL);
  show_picker (self);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Rebuilds the primary group line and the membership list. */
void
sbv_member_of_set (SbvMemberOf *self, SbvConnection *conn, const char *dn,
                   const char * const *member_of, gint primary_group_id)
{
  g_return_if_fail (SBV_IS_MEMBER_OF (self));

  g_set_object (&self->conn, conn);
  g_free (self->dn);
  self->dn = g_strdup (dn);
  g_strfreev (self->member_of);
  self->member_of = g_strdupv ((char **) member_of);
  self->primary_rid = primary_group_id;
  show_error (self, NULL);
  gtk_widget_set_sensitive (self->list_box, TRUE);

  if (primary_group_id < 0) {
    gtk_widget_set_visible (self->primary_label, FALSE);
  } else {
    const char *name = sbv_group_well_known_name (primary_group_id);
    char *text = name ? g_strdup_printf ("Primary group: %s", name)
                      : g_strdup_printf ("Primary group: RID %d", primary_group_id);
    gtk_label_set_text (GTK_LABEL (self->primary_label), text);
    gtk_widget_set_visible (self->primary_label, TRUE);
    g_free (text);
  }

  /* Remove rows by index so the placeholder stays. */
  GtkListBoxRow *old_row;
  while ((old_row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->list_box), 0)))
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), GTK_WIDGET (old_row));

  /* Sort by display name so the list reads in a stable order. */
  guint   n     = self->member_of ? g_strv_length (self->member_of) : 0;
  char  **names = g_new0 (char *, n + 1);
  guint  *order = g_new (guint, n);
  for (guint i = 0; i < n; i++) {
    names[i] = sbv_dn_rdn_value (self->member_of[i]);
    if (!names[i]) names[i] = g_strdup (self->member_of[i]);
    order[i] = i;
  }
  for (guint i = 1; i < n; i++) {
    guint v = order[i], j = i;
    while (j > 0 && g_utf8_collate (names[order[j - 1]], names[v]) > 0) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }

  for (guint k = 0; k < n; k++) {
    guint      i   = order[k];
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top    (row, 4);
    gtk_widget_set_margin_bottom (row, 4);
    gtk_widget_set_margin_start  (row, 12);
    gtk_widget_set_margin_end    (row, 6);

    GtkWidget *lbl = gtk_label_new (names[i]);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (lbl, TRUE);
    gtk_widget_set_tooltip_text (lbl, self->member_of[i]);
    gtk_box_append (GTK_BOX (row), lbl);

    GtkWidget *rm = gtk_button_new_from_icon_name ("list-remove-symbolic");
    gtk_widget_add_css_class (rm, "flat");
    gtk_widget_set_tooltip_text (rm, "Remove from this group");
    g_object_set_data_full (G_OBJECT (rm), "group-dn",
                            g_strdup (self->member_of[i]), g_free);
    g_signal_connect (rm, "clicked", G_CALLBACK (on_remove_clicked), self);
    gtk_box_append (GTK_BOX (row), rm);

    gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
  }
  g_strfreev (names);
  g_free (order);
}

/* ── GObject ────────────────────────────────────────────────────────────── */

/* Releases the connection and copied strings. */
static void
sbv_member_of_finalize (GObject *object)
{
  SbvMemberOf *self = SBV_MEMBER_OF (object);
  g_clear_object (&self->conn);
  g_free (self->dn);
  g_strfreev (self->member_of);
  G_OBJECT_CLASS (sbv_member_of_parent_class)->finalize (object);
}

/* Registers the "changed" signal. */
static void
sbv_member_of_class_init (SbvMemberOfClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_member_of_finalize;
  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/* Builds the primary group line, the list, the error label and the Add
 * button. */
static void
sbv_member_of_init (SbvMemberOf *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 6);
  self->primary_rid = -1;

  self->primary_label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->primary_label), 0);
  gtk_widget_add_css_class (self->primary_label, "dim-label");
  gtk_widget_set_tooltip_text (self->primary_label,
      "The primary group is set on the account and isn't listed in memberOf.");
  gtk_box_append (GTK_BOX (self), self->primary_label);

  self->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list_box), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (self->list_box, "boxed-list");
  GtkWidget *empty = gtk_label_new ("No other groups");
  gtk_widget_add_css_class (empty, "dim-label");
  gtk_widget_set_margin_top    (empty, 8);
  gtk_widget_set_margin_bottom (empty, 8);
  gtk_list_box_set_placeholder (GTK_LIST_BOX (self->list_box), empty);
  gtk_box_append (GTK_BOX (self), self->list_box);

  self->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->error_label), 0);
  gtk_widget_add_css_class (self->error_label, "error");
  gtk_widget_set_visible (self->error_label, FALSE);
  gtk_box_append (GTK_BOX (self), self->error_label);

  self->add_btn = gtk_button_new_with_label ("Add to Group…");
  gtk_widget_set_halign (self->add_btn, GTK_ALIGN_START);
  g_signal_connect (self->add_btn, "clicked", G_CALLBACK (on_add_clicked), self);
  gtk_box_append (GTK_BOX (self), self->add_btn);
}

/* Creates an empty section. Call sbv_member_of_set to fill it. */
GtkWidget *
sbv_member_of_new (void)
{
  return g_object_new (SBV_TYPE_MEMBER_OF, NULL);
}
