#include "sbv-computers-panel.h"
#include "sbv-create-computer-dialog.h"
#include "../backend/sbv-computers-backend.h"
#include "../model/sbv-computer.h"

#include <adwaita.h>
#include <string.h>

struct _SbvComputersPanel {
  GtkBox         parent;

  /* ── List pane ── */
  GtkWidget     *outer_stack;    /* "loading" | "empty" | "split" */
  GtkWidget     *search_entry;
  GtkWidget     *add_btn;
  GtkWidget     *list_box;
  GtkWidget     *spinner;

  /* ── Detail pane ── */
  GtkWidget     *detail_stack;   /* "none" | "detail" */

  /* Identity (read-only) */
  GtkWidget     *dn_label;
  GtkWidget     *cn_label;
  GtkWidget     *sam_label;

  /* OS (read-only) */
  GtkWidget     *os_label;
  GtkWidget     *os_version_label;
  GtkWidget     *os_sp_label;

  /* Account (editable) */
  GtkWidget     *description_entry;
  GtkWidget     *dns_hostname_entry;
  GtkWidget     *enabled_check;
  GtkWidget     *save_btn;
  GtkWidget     *save_error;

  /* SPN editor */
  GtkWidget     *spn_warning_label;  /* shown when pwdLastSet == 0 */
  GtkWidget     *spn_list_box;       /* one row per current SPN */
  GtkWidget     *spn_empty_label;    /* shown when working_spns is empty */
  GtkWidget     *spn_new_entry;
  GtkWidget     *spn_add_btn;
  GtkWidget     *spn_save_btn;
  GtkWidget     *spn_save_error;
  GPtrArray     *working_spns;       /* (owned) char* entries; staged edits */

  /* Raw LDAP attributes (dynamic, rebuilt on selection) */
  GtkWidget     *raw_attrs_box;

  /* Paned split — position initialised on first map */
  GtkWidget     *paned;
  gboolean       paned_init;

  /* Delete */
  GtkWidget     *delete_btn;
  GtkWidget     *delete_error;

  /* State */
  SbvConnection *conn;              /* unowned */
  SbvComputer   *selected_computer; /* owned ref */
  char          *filter_text;
  /* When non-NULL the next reload re-selects the row whose DN matches
   * (used by the create flow to highlight the new entry). */
  char          *pending_select_dn;
};

G_DEFINE_TYPE (SbvComputersPanel, sbv_computers_panel, GTK_TYPE_BOX)

/* ── Forward declarations ───────────────────────────────────────────────── */
static void populate_raw_attrs      (GtkWidget *box, GHashTable *attrs, int label_width);
void        sbv_computers_panel_load (SbvComputersPanel *self, SbvConnection *conn);
static void rebuild_spn_rows         (SbvComputersPanel *self);
static void update_spn_editability   (SbvComputersPanel *self);

/* ── Row construction ───────────────────────────────────────────────────── */

static GtkWidget *
make_computer_row (SbvComputer *computer)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-computer",
                           g_object_ref (computer), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top    (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *icon = gtk_image_new_from_icon_name ("computer-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text_box, TRUE);

  /* Prefer cn (host name without trailing $) over sAMAccountName */
  const char *cn  = sbv_computer_get_cn (computer);
  const char *sam = sbv_computer_get_sam (computer);
  const char *dns = sbv_computer_get_dns_hostname (computer);

  GtkWidget *name_label = gtk_label_new (cn ? cn : (sam ? sam : ""));
  gtk_label_set_xalign (GTK_LABEL (name_label), 0);
  gtk_widget_add_css_class (name_label, "heading");
  gtk_box_append (GTK_BOX (text_box), name_label);

  if (dns && *dns) {
    GtkWidget *dns_label = gtk_label_new (dns);
    gtk_label_set_xalign (GTK_LABEL (dns_label), 0);
    gtk_widget_add_css_class (dns_label, "dim-label");
    gtk_box_append (GTK_BOX (text_box), dns_label);
  }

  gtk_box_append (GTK_BOX (box), text_box);

  gboolean   enabled = sbv_computer_get_enabled (computer);
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
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  if (!self->filter_text || !*self->filter_text)
    return TRUE;

  SbvComputer *computer = g_object_get_data (G_OBJECT (row), "sbv-computer");
  if (!computer) return TRUE;

  const char *fields[] = {
    sbv_computer_get_cn (computer),
    sbv_computer_get_sam (computer),
    sbv_computer_get_dns_hostname (computer),
    sbv_computer_get_description (computer),
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
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  g_free (self->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  self->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* ── Load computer into detail form ─────────────────────────────────────── */

static void
load_computer_into_detail (SbvComputersPanel *self, SbvComputer *computer)
{
  g_set_object (&self->selected_computer, computer);

  gtk_label_set_text (GTK_LABEL (self->dn_label),
                      sbv_computer_get_dn (computer) ?: "");
  gtk_label_set_text (GTK_LABEL (self->cn_label),
                      sbv_computer_get_cn (computer) ?: "");
  gtk_label_set_text (GTK_LABEL (self->sam_label),
                      sbv_computer_get_sam (computer) ?: "");

  gtk_label_set_text (GTK_LABEL (self->os_label),
                      sbv_computer_get_os (computer) ?: "\xe2\x80\x94");
  gtk_label_set_text (GTK_LABEL (self->os_version_label),
                      sbv_computer_get_os_version (computer) ?: "\xe2\x80\x94");
  gtk_label_set_text (GTK_LABEL (self->os_sp_label),
                      sbv_computer_get_os_service_pack (computer) ?: "\xe2\x80\x94");

  gtk_editable_set_text (GTK_EDITABLE (self->description_entry),
                         sbv_computer_get_description (computer) ?: "");
  gtk_editable_set_text (GTK_EDITABLE (self->dns_hostname_entry),
                         sbv_computer_get_dns_hostname (computer) ?: "");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->enabled_check),
                                sbv_computer_get_enabled (computer));

  /* SPNs — copy current values into the working list */
  if (self->working_spns)
    g_ptr_array_set_size (self->working_spns, 0);
  else
    self->working_spns = g_ptr_array_new_with_free_func (g_free);

  guint n_spn = sbv_computer_get_spn_count (computer);
  if (n_spn > 0) {
    const char * const *spns = sbv_computer_get_spn (computer);
    for (guint i = 0; spns && spns[i]; i++)
      g_ptr_array_add (self->working_spns, g_strdup (spns[i]));
  }
  rebuild_spn_rows (self);
  update_spn_editability (self);
  gtk_editable_set_text (GTK_EDITABLE (self->spn_new_entry), "");
  gtk_label_set_text (GTK_LABEL (self->spn_save_error), "");
  gtk_widget_set_visible (self->spn_save_error, FALSE);
  gtk_widget_set_sensitive (self->spn_save_btn, FALSE);

  /* Raw LDAP attributes */
  populate_raw_attrs (self->raw_attrs_box,
                      sbv_computer_get_ldap_attrs (computer), 140);

  gtk_label_set_text (GTK_LABEL (self->save_error), "");
  gtk_widget_set_visible (self->save_error, FALSE);
  if (self->delete_error) {
    gtk_label_set_text (GTK_LABEL (self->delete_error), "");
    gtk_widget_set_visible (self->delete_error, FALSE);
  }

  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "detail");
}

/* ── Row activated ──────────────────────────────────────────────────────── */

static void
on_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);

  if (!row) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    return;
  }

  SbvComputer *computer = g_object_get_data (G_OBJECT (row), "sbv-computer");
  if (computer)
    load_computer_into_detail (self, computer);
}

/* ── Save attributes ────────────────────────────────────────────────────── */

typedef struct {
  SbvComputersPanel *panel;
  gboolean           need_enabled_change;
  gboolean           new_enabled;
} SaveCtx;

static void
on_set_enabled_after_save (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SaveCtx           *ctx  = user_data;
  SbvComputersPanel *self = ctx->panel;
  SbvConnection     *conn = SBV_CONNECTION (source);
  GError            *err  = NULL;

  gtk_widget_set_sensitive (self->save_btn, TRUE);

  if (!sbv_computers_set_enabled_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->save_error), err->message);
    gtk_widget_set_visible (self->save_error, TRUE);
    g_error_free (err);
  } else {
    if (self->selected_computer)
      sbv_computer_set_enabled (self->selected_computer, ctx->new_enabled);
    sbv_computers_panel_load (self, conn);
  }
  g_free (ctx);
}

static void
on_update_attrs_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SaveCtx           *ctx  = user_data;
  SbvComputersPanel *self = ctx->panel;
  SbvConnection     *conn = SBV_CONNECTION (source);
  GError            *err  = NULL;

  if (!sbv_computers_update_attrs_finish (conn, result, &err)) {
    gtk_widget_set_sensitive (self->save_btn, TRUE);
    gtk_label_set_text (GTK_LABEL (self->save_error), err->message);
    gtk_widget_set_visible (self->save_error, TRUE);
    g_error_free (err);
    g_free (ctx);
    return;
  }

  if (self->selected_computer) {
    sbv_computer_set_description (self->selected_computer,
      gtk_editable_get_text (GTK_EDITABLE (self->description_entry)));
    sbv_computer_set_dns_hostname (self->selected_computer,
      gtk_editable_get_text (GTK_EDITABLE (self->dns_hostname_entry)));
  }

  if (ctx->need_enabled_change) {
    sbv_computers_set_enabled_async (conn, self->selected_computer, ctx->new_enabled,
                                      NULL, on_set_enabled_after_save, ctx);
  } else {
    gtk_widget_set_sensitive (self->save_btn, TRUE);
    sbv_computers_panel_load (self, conn);
    g_free (ctx);
  }
}

static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  if (!self->selected_computer || !self->conn) return;

  gtk_widget_set_visible (self->save_error, FALSE);
  gtk_widget_set_sensitive (self->save_btn, FALSE);

  const char *desc = gtk_editable_get_text (GTK_EDITABLE (self->description_entry));
  const char *dns  = gtk_editable_get_text (GTK_EDITABLE (self->dns_hostname_entry));

  gboolean new_enabled = gtk_check_button_get_active (
    GTK_CHECK_BUTTON (self->enabled_check));
  gboolean enabled_changed =
    (new_enabled != sbv_computer_get_enabled (self->selected_computer));

  SaveCtx *ctx = g_new0 (SaveCtx, 1);
  ctx->panel               = self;
  ctx->need_enabled_change = enabled_changed;
  ctx->new_enabled         = new_enabled;

  sbv_computers_update_attrs_async (self->conn, self->selected_computer,
                                     desc, dns,
                                     NULL, on_update_attrs_done, ctx);
}

/* ── Create / Delete ────────────────────────────────────────────────────── */

/* Called when the create-computer dialog finishes successfully. Records the
 * new DN so the upcoming reload auto-selects that row. */
static void
on_computer_created (SbvConnection *conn, const char *new_dn, gpointer user_data)
{
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  g_free (self->pending_select_dn);
  self->pending_select_dn = g_strdup (new_dn);
  sbv_computers_panel_load (self, conn);
}

/* Toolbar "+" button: opens the create-computer dialog. */
static void
on_add_computer_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  if (!self->conn) return;

  GtkWidget *parent = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  sbv_create_computer_dialog_show (parent ? GTK_WINDOW (parent) : NULL,
                                    self->conn, on_computer_created, self);
}

/* Async-completion callback for the computer delete operation. */
static void
on_computer_delete_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  SbvConnection     *conn = SBV_CONNECTION (source);
  GError            *err  = NULL;

  gtk_widget_set_sensitive (self->delete_btn, TRUE);

  if (!sbv_computers_delete_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->delete_error), err->message);
    gtk_widget_set_visible (self->delete_error, TRUE);
    g_error_free (err);
    return;
  }

  /* Drop stale selection so the reload doesn't re-pick a deleted entry. */
  g_clear_object (&self->selected_computer);
  sbv_computers_panel_load (self, conn);
}

/* Type-to-confirm dialog state. */
typedef struct {
  SbvComputersPanel *panel;
  GtkWindow         *dialog;
  GtkWidget         *entry;
  GtkWidget         *delete_btn;
  char              *expected_sam;
} ConfirmDelComputerCtx;

/* Frees the per-dialog context. */
static void
confirm_del_computer_ctx_free (ConfirmDelComputerCtx *ctx)
{
  g_free (ctx->expected_sam);
  g_free (ctx);
}

/* Enables the destructive button only when the typed text matches the
 * target computer's sAMAccountName (including the trailing $). */
static void
on_confirm_computer_entry_changed (GtkEditable *editable, gpointer user_data)
{
  ConfirmDelComputerCtx *ctx   = user_data;
  const char            *typed = gtk_editable_get_text (editable);
  gtk_widget_set_sensitive (ctx->delete_btn,
                             g_str_equal (typed, ctx->expected_sam));
}

/* Confirm-button handler: dispatches the delete and closes the dialog. */
static void
on_confirm_computer_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  ConfirmDelComputerCtx *ctx  = user_data;
  SbvComputersPanel     *self = ctx->panel;
  if (!self->conn || !self->selected_computer) {
    gtk_window_destroy (ctx->dialog);
    return;
  }

  gtk_widget_set_visible (self->delete_error, FALSE);
  gtk_widget_set_sensitive (self->delete_btn, FALSE);

  sbv_computers_delete_async (self->conn, self->selected_computer,
                               NULL, on_computer_delete_done, self);
  gtk_window_destroy (ctx->dialog);
}

/* Builds and presents the type-to-confirm modal. */
static void
show_computer_delete_confirm (SbvComputersPanel *self)
{
  if (!self->selected_computer) return;
  const char *sam = sbv_computer_get_sam (self->selected_computer);
  if (!sam || !*sam) return;

  ConfirmDelComputerCtx *ctx = g_new0 (ConfirmDelComputerCtx, 1);
  ctx->panel        = self;
  ctx->expected_sam = g_strdup (sam);

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Delete Computer");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 440, 220);
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
                    G_CALLBACK (on_confirm_computer_delete_clicked), ctx);

  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top    (body, 16);
  gtk_widget_set_margin_bottom (body, 16);
  gtk_widget_set_margin_start  (body, 20);
  gtk_widget_set_margin_end    (body, 20);

  char *warn_text = g_strdup_printf (
    "Permanently delete computer <b>%s</b>?\n"
    "This cannot be undone. The workstation will lose its trust "
    "relationship with the domain.", sam);
  GtkWidget *warn = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (warn), warn_text);
  gtk_label_set_xalign (GTK_LABEL (warn), 0);
  gtk_label_set_wrap   (GTK_LABEL (warn), TRUE);
  g_free (warn_text);
  gtk_box_append (GTK_BOX (body), warn);

  char *prompt_text = g_strdup_printf (
    "Type the account name (<tt>%s</tt>) to confirm:", sam);
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
                    G_CALLBACK (on_confirm_computer_entry_changed), ctx);
  gtk_box_append (GTK_BOX (body), ctx->entry);

  gtk_box_append (GTK_BOX (vbox), body);

  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect_swapped (win, "destroy",
                             G_CALLBACK (confirm_del_computer_ctx_free), ctx);
  gtk_window_present (GTK_WINDOW (win));
  gtk_widget_grab_focus (ctx->entry);
}

/* Detail-pane "Delete" button: routes to the type-to-confirm dialog. */
static void
on_delete_computer_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  show_computer_delete_confirm (SBV_COMPUTERS_PANEL (user_data));
}

/* ── SPN editor ─────────────────────────────────────────────────────────── */

/* Returns TRUE if the selected computer's pwdLastSet is 0, meaning the
 * account has no key material yet (created but never joined). SPNs added
 * to such accounts are non-functional, so we lock the editor. */
static gboolean
selected_is_uninitialized (SbvComputersPanel *self)
{
  if (!self->selected_computer) return FALSE;
  return sbv_computer_get_pwd_last_set (self->selected_computer) == 0;
}

/* Toggles SPN editor controls based on whether the selected account has
 * been initialized. Also flips the warning label. */
static void
update_spn_editability (SbvComputersPanel *self)
{
  gboolean uninit = selected_is_uninitialized (self);
  gtk_widget_set_visible (self->spn_warning_label, uninit);

  gboolean editable = !uninit && self->selected_computer != NULL;
  gtk_widget_set_sensitive (self->spn_new_entry, editable);
  gtk_widget_set_sensitive (self->spn_add_btn,   editable);
  /* Per-row remove buttons live in spn_list_box; mark each row sensitive
   * to match. The save button is enabled separately when the working list
   * actually differs from the loaded values. */
  for (GtkWidget *row = gtk_widget_get_first_child (self->spn_list_box);
       row != NULL;
       row = gtk_widget_get_next_sibling (row))
    gtk_widget_set_sensitive (row, editable);
}

/* Marks the save button sensitive whenever the working SPN list differs
 * from the loaded computer's SPN values (set membership compare — order
 * doesn't matter for AD). */
static void
update_spn_dirty (SbvComputersPanel *self)
{
  if (!self->selected_computer) {
    gtk_widget_set_sensitive (self->spn_save_btn, FALSE);
    return;
  }

  const char * const *current = sbv_computer_get_spn (self->selected_computer);
  guint n_current = sbv_computer_get_spn_count (self->selected_computer);
  guint n_working = self->working_spns ? self->working_spns->len : 0;

  gboolean dirty = (n_current != n_working);
  if (!dirty) {
    /* Same length — check that every working entry is present in current. */
    for (guint i = 0; i < n_working && !dirty; i++) {
      const char *w = g_ptr_array_index (self->working_spns, i);
      gboolean found = FALSE;
      for (guint j = 0; j < n_current; j++) {
        if (g_strcmp0 (w, current[j]) == 0) { found = TRUE; break; }
      }
      if (!found) dirty = TRUE;
    }
  }

  gtk_widget_set_sensitive (self->spn_save_btn,
                             dirty && !selected_is_uninitialized (self));
}

/* "Remove" button on an SPN row: drops that entry from the working list
 * and rebuilds the rows. */
static void
on_spn_remove_clicked (GtkButton *btn, gpointer user_data)
{
  SbvComputersPanel *self  = SBV_COMPUTERS_PANEL (user_data);
  const char        *value = g_object_get_data (G_OBJECT (btn), "spn-value");
  if (!value || !self->working_spns) return;

  for (guint i = 0; i < self->working_spns->len; i++) {
    if (g_strcmp0 (g_ptr_array_index (self->working_spns, i), value) == 0) {
      g_ptr_array_remove_index (self->working_spns, i);
      break;
    }
  }
  rebuild_spn_rows (self);
  update_spn_dirty (self);
}

/* Build a single SPN row: monospace value on the left, flat trash button
 * on the right. */
static GtkWidget *
make_spn_row (SbvComputersPanel *self, const char *value)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (row, 2);
  gtk_widget_set_margin_bottom (row, 2);

  GtkWidget *lbl = gtk_label_new (value);
  gtk_label_set_xalign (GTK_LABEL (lbl), 0);
  gtk_label_set_selectable (GTK_LABEL (lbl), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (lbl, "monospace");
  gtk_widget_set_hexpand (lbl, TRUE);
  gtk_box_append (GTK_BOX (row), lbl);

  GtkWidget *btn = gtk_button_new_from_icon_name ("user-trash-symbolic");
  gtk_widget_add_css_class (btn, "flat");
  gtk_widget_set_tooltip_text (btn, "Remove SPN");
  g_object_set_data_full (G_OBJECT (btn), "spn-value",
                           g_strdup (value), g_free);
  g_signal_connect (btn, "clicked", G_CALLBACK (on_spn_remove_clicked), self);
  gtk_box_append (GTK_BOX (row), btn);

  return row;
}

/* Rebuilds all SPN rows from working_spns. Called on selection change and
 * after every add/remove. Keeps display sorted alphabetically. */
static void
rebuild_spn_rows (SbvComputersPanel *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->spn_list_box)) != NULL)
    gtk_box_remove (GTK_BOX (self->spn_list_box), child);

  guint n = self->working_spns ? self->working_spns->len : 0;
  gtk_widget_set_visible (self->spn_empty_label, n == 0);
  if (n == 0) return;

  /* Sort a copy by case-insensitive collate for stable display. */
  GPtrArray *sorted = g_ptr_array_new ();
  for (guint i = 0; i < n; i++)
    g_ptr_array_add (sorted, g_ptr_array_index (self->working_spns, i));
  g_ptr_array_sort (sorted, (GCompareFunc) g_ascii_strcasecmp);

  for (guint i = 0; i < sorted->len; i++) {
    const char *spn = g_ptr_array_index (sorted, i);
    gtk_box_append (GTK_BOX (self->spn_list_box), make_spn_row (self, spn));
  }
  g_ptr_array_free (sorted, TRUE);
}

/* "Add" button / Enter on the new-SPN entry: validates the input and
 * appends to the working list. Validation is intentionally lenient — we
 * just require a `service/host` shape and reject duplicates. */
static void
on_spn_add_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvComputersPanel *self  = SBV_COMPUTERS_PANEL (user_data);
  const char        *typed = gtk_editable_get_text (GTK_EDITABLE (self->spn_new_entry));
  if (!self->selected_computer || selected_is_uninitialized (self)) return;

  gtk_widget_set_visible (self->spn_save_error, FALSE);

  if (!typed || !*typed) return;

  const char *slash = strchr (typed, '/');
  if (!slash || slash == typed || !*(slash + 1)) {
    gtk_label_set_text (GTK_LABEL (self->spn_save_error),
                        "SPN must be of the form service/host (e.g. HOST/pc.example.com).");
    gtk_widget_set_visible (self->spn_save_error, TRUE);
    return;
  }

  if (!self->working_spns)
    self->working_spns = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < self->working_spns->len; i++) {
    if (g_ascii_strcasecmp (g_ptr_array_index (self->working_spns, i), typed) == 0) {
      gtk_label_set_text (GTK_LABEL (self->spn_save_error),
                          "That SPN is already in the list.");
      gtk_widget_set_visible (self->spn_save_error, TRUE);
      return;
    }
  }

  g_ptr_array_add (self->working_spns, g_strdup (typed));
  gtk_editable_set_text (GTK_EDITABLE (self->spn_new_entry), "");
  rebuild_spn_rows (self);
  update_spn_dirty (self);
}

/* Activate (Enter) on the new-SPN entry mirrors the Add button. */
static void
on_spn_entry_activate (GtkEntry *entry, gpointer user_data)
{
  (void) entry;
  on_spn_add_clicked (NULL, user_data);
}

/* Async-completion: SPN write returned. On success we refresh the list so
 * the model reflects what's now on the DC. */
static void
on_spn_save_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  SbvConnection     *conn = SBV_CONNECTION (source);
  GError            *err  = NULL;

  if (!sbv_computers_set_spn_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->spn_save_error), err->message);
    gtk_widget_set_visible (self->spn_save_error, TRUE);
    g_error_free (err);
    gtk_widget_set_sensitive (self->spn_save_btn, TRUE);
    return;
  }

  sbv_computers_panel_load (self, conn);
}

/* "Save SPNs" button: builds a NULL-terminated GStrv from the working
 * list and dispatches the LDAP modify. */
static void
on_spn_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  if (!self->conn || !self->selected_computer) return;
  if (selected_is_uninitialized (self)) return;

  guint n = self->working_spns ? self->working_spns->len : 0;
  char **spns = g_new0 (char *, n + 1);
  for (guint i = 0; i < n; i++)
    spns[i] = g_strdup (g_ptr_array_index (self->working_spns, i));

  gtk_widget_set_visible (self->spn_save_error, FALSE);
  gtk_widget_set_sensitive (self->spn_save_btn, FALSE);

  sbv_computers_set_spn_async (self->conn, self->selected_computer,
                                (const char * const *) spns,
                                NULL, on_spn_save_done, self);
  g_strfreev (spns);
}

/* ── Load callback ──────────────────────────────────────────────────────── */

static void
on_computers_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  GError            *err  = NULL;

  GListStore *store = sbv_computers_list_finish (SBV_CONNECTION (source),
                                                  result, &err);
  if (!store) {
    g_warning ("Failed to load computers: %s", err->message);
    g_error_free (err);
    gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "empty");
    return;
  }

  /* Decide what to re-select. pending_select_dn (set by the create flow)
   * wins, matched by full DN; otherwise fall back to selected_computer's
   * sam which preserves selection across normal save reloads. */
  char *target_dn  = NULL;
  char *target_sam = NULL;
  if (self->pending_select_dn) {
    target_dn = self->pending_select_dn;        /* takes ownership */
    self->pending_select_dn = NULL;
  } else if (self->selected_computer) {
    target_sam = g_strdup (sbv_computer_get_sam (self->selected_computer));
  }

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  GtkListBoxRow *reselect = NULL;

  for (guint i = 0; i < n; i++) {
    SbvComputer *computer = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget   *row      = make_computer_row (computer);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
    if (!reselect) {
      if (target_dn && g_str_equal (sbv_computer_get_dn (computer) ?: "", target_dn))
        reselect = GTK_LIST_BOX_ROW (row);
      else if (target_sam && g_str_equal (sbv_computer_get_sam (computer) ?: "", target_sam))
        reselect = GTK_LIST_BOX_ROW (row);
    }
    g_object_unref (computer);
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
sbv_computers_panel_load (SbvComputersPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "loading");
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  sbv_computers_list_async (conn, NULL, on_computers_loaded, self);
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

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
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (user_data);
  if (!self->paned_init) {
    int w = gtk_widget_get_width (widget);
    if (w > 0) {
      self->paned_init = TRUE;
      gtk_paned_set_position (GTK_PANED (widget), 280);
    }
  }
}

/* ── GObject init ───────────────────────────────────────────────────────── */

static void
sbv_computers_panel_finalize (GObject *object)
{
  SbvComputersPanel *self = SBV_COMPUTERS_PANEL (object);
  g_free (self->filter_text);
  g_free (self->pending_select_dn);
  g_clear_object (&self->selected_computer);
  if (self->working_spns) g_ptr_array_unref (self->working_spns);
  G_OBJECT_CLASS (sbv_computers_panel_parent_class)->finalize (object);
}

static void sbv_computers_panel_class_init (SbvComputersPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_computers_panel_finalize;
}

static void
sbv_computers_panel_init (SbvComputersPanel *self)
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
    self->spinner = gtk_spinner_new ();
    gtk_box_append (GTK_BOX (box), self->spinner);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), box, "loading");
  }

  /* Empty */
  {
    GtkWidget *empty = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty),
                                    "computer-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Computers Found");
    gtk_stack_add_named (GTK_STACK (self->outer_stack), empty, "empty");
  }

  /* Split view */
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
    gtk_widget_set_tooltip_text (self->search_entry, "Search computers");
    g_signal_connect (self->search_entry, "search-changed",
                      G_CALLBACK (on_search_changed), self);
    gtk_box_append (GTK_BOX (toolbar), self->search_entry);

    self->add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_add_css_class (self->add_btn, "flat");
    gtk_widget_set_tooltip_text (self->add_btn, "Create computer");
    g_signal_connect (self->add_btn, "clicked",
                      G_CALLBACK (on_add_computer_btn_clicked), self);
    gtk_box_append (GTK_BOX (toolbar), self->add_btn);

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
    adw_status_page_set_title (ADW_STATUS_PAGE (none_page), "Select a Computer");
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (none_page),
                                    "computer-symbolic");
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

    self->dn_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->dn_label), 0);
    gtk_label_set_ellipsize (GTK_LABEL (self->dn_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable (GTK_LABEL (self->dn_label), TRUE);
    gtk_widget_add_css_class (self->dn_label, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("DN", self->dn_label));

    self->cn_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->cn_label), 0);
    gtk_box_append (GTK_BOX (form), make_field_row ("Computer Name", self->cn_label));

    self->sam_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->sam_label), 0);
    gtk_widget_add_css_class (self->sam_label, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Account Name", self->sam_label));

    /* ── Operating System ── */
    gtk_box_append (GTK_BOX (form), make_section_label ("Operating System"));

    self->os_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->os_label), 0);
    gtk_box_append (GTK_BOX (form), make_field_row ("OS", self->os_label));

    self->os_version_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->os_version_label), 0);
    gtk_widget_add_css_class (self->os_version_label, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Version", self->os_version_label));

    self->os_sp_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->os_sp_label), 0);
    gtk_box_append (GTK_BOX (form), make_field_row ("Service Pack", self->os_sp_label));

    /* ── Account ── */
    gtk_box_append (GTK_BOX (form), make_section_label ("Account"));

    self->dns_hostname_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->dns_hostname_entry),
                                     "host.example.com");
    gtk_widget_add_css_class (self->dns_hostname_entry, "monospace");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("DNS Hostname", self->dns_hostname_entry));

    self->description_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->description_entry),
                                     "Description");
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Description", self->description_entry));

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

    /* ── Service Principal Names (editable) ── */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Service Principal Names"));

    /* Warning banner — shown only when pwdLastSet == 0. The directory
     * will accept the modify, but the SPN is non-functional until the
     * computer joins the domain and acquires key material, so we
     * disable the editor outright. */
    self->spn_warning_label = gtk_label_new (
      "Computer must be initialized (joined domain) before SPNs can be registered.");
    gtk_label_set_xalign (GTK_LABEL (self->spn_warning_label), 0);
    gtk_label_set_wrap (GTK_LABEL (self->spn_warning_label), TRUE);
    gtk_widget_add_css_class (self->spn_warning_label, "warning");
    gtk_widget_set_margin_start (self->spn_warning_label, 148);
    gtk_widget_set_visible (self->spn_warning_label, FALSE);
    gtk_box_append (GTK_BOX (form), self->spn_warning_label);

    GtkWidget *spn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start (spn_box, 148);

    self->spn_list_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (spn_box), self->spn_list_box);

    self->spn_empty_label = gtk_label_new ("\xe2\x80\x94");
    gtk_label_set_xalign (GTK_LABEL (self->spn_empty_label), 0);
    gtk_widget_add_css_class (self->spn_empty_label, "dim-label");
    gtk_box_append (GTK_BOX (spn_box), self->spn_empty_label);

    GtkWidget *add_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top (add_row, 4);
    self->spn_new_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->spn_new_entry),
                                     "service/host.example.com");
    gtk_widget_add_css_class (self->spn_new_entry, "monospace");
    gtk_widget_set_hexpand (self->spn_new_entry, TRUE);
    g_signal_connect (self->spn_new_entry, "activate",
                      G_CALLBACK (on_spn_entry_activate), self);
    gtk_box_append (GTK_BOX (add_row), self->spn_new_entry);

    self->spn_add_btn = gtk_button_new_with_label ("Add");
    g_signal_connect (self->spn_add_btn, "clicked",
                      G_CALLBACK (on_spn_add_clicked), self);
    gtk_box_append (GTK_BOX (add_row), self->spn_add_btn);
    gtk_box_append (GTK_BOX (spn_box), add_row);

    self->spn_save_btn = gtk_button_new_with_label ("Save SPNs");
    gtk_widget_add_css_class (self->spn_save_btn, "suggested-action");
    gtk_widget_set_halign (self->spn_save_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top (self->spn_save_btn, 4);
    gtk_widget_set_sensitive (self->spn_save_btn, FALSE);
    g_signal_connect (self->spn_save_btn, "clicked",
                      G_CALLBACK (on_spn_save_clicked), self);
    gtk_box_append (GTK_BOX (spn_box), self->spn_save_btn);

    self->spn_save_error = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->spn_save_error), 0);
    gtk_label_set_wrap (GTK_LABEL (self->spn_save_error), TRUE);
    gtk_widget_add_css_class (self->spn_save_error, "error");
    gtk_widget_set_visible (self->spn_save_error, FALSE);
    gtk_box_append (GTK_BOX (spn_box), self->spn_save_error);

    gtk_box_append (GTK_BOX (form), spn_box);

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

      self->delete_btn = gtk_button_new_with_label ("Delete Computer…");
      gtk_widget_add_css_class (self->delete_btn, "destructive-action");
      gtk_widget_set_halign (self->delete_btn, GTK_ALIGN_START);
      gtk_widget_set_margin_start (self->delete_btn, 148);
      g_signal_connect (self->delete_btn, "clicked",
                        G_CALLBACK (on_delete_computer_btn_clicked), self);
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
sbv_computers_panel_new (void)
{
  return g_object_new (SBV_TYPE_COMPUTERS_PANEL, NULL);
}
