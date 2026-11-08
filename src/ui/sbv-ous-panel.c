#include "sbv-ous-panel.h"
#include "sbv-ou-row.h"
#include "sbv-create-ou-dialog.h"
#include "sbv-move-dialog.h"
#include "../backend/sbv-ou-backend.h"
#include "../backend/sbv-dn.h"
#include "../model/sbv-ou.h"

#include <adwaita.h>
#include <string.h>

struct _SbvOusPanel {
  GtkBox         parent;

  /* ── List pane ── */
  GtkWidget     *outer_stack;    /* "loading" | "empty" | "split" */
  GtkWidget     *search_entry;
  GtkWidget     *add_btn;
  GtkWidget     *list_box;
  GtkWidget     *spinner;

  /* ── Detail pane ── */
  GtkWidget     *detail_stack;   /* "none" | "detail" */
  GtkWidget     *heading_label;
  GtkWidget     *dn_label;
  GtkWidget     *name_entry;
  GtkWidget     *desc_entry;
  GtkWidget     *locked_hint;
  GtkWidget     *save_btn;
  GtkWidget     *save_error;
  GtkWidget     *parent_label;
  GtkWidget     *move_btn;
  GtkWidget     *raw_attrs_box;
  GtkWidget     *danger_box;
  GtkWidget     *delete_btn;
  GtkWidget     *delete_error;

  GtkWidget     *paned;
  gboolean       paned_init;

  /* ── State ── */
  SbvConnection *conn;              /* unowned */
  SbvOu         *selected_ou;       /* owned ref */
  char          *filter_text;
  char          *pending_select_dn; /* reselect target for the next reload */
};

G_DEFINE_TYPE (SbvOusPanel, sbv_ous_panel, GTK_TYPE_BOX)

enum { SIGNAL_DIRECTORY_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* Returns the toplevel window that hosts the panel, for dialog parenting. */
static GtkWindow *
panel_window (SbvOusPanel *self)
{
  GtkWidget *w = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  return w ? GTK_WINDOW (w) : NULL;
}

/* Reloads the list and re-selects `dn` (borrowed; NULL clears selection)
 * once it arrives. */
static void
reload_selecting (SbvOusPanel *self, const char *dn)
{
  g_free (self->pending_select_dn);
  self->pending_select_dn = g_strdup (dn);
  if (!dn) g_clear_object (&self->selected_ou);
  sbv_ous_panel_load (self, self->conn);
}

/* Shows or hides an inline error label. NULL hides it. */
static void
set_error (GtkWidget *label, const char *message)
{
  gtk_label_set_text (GTK_LABEL (label), message ?: "");
  gtk_widget_set_visible (label, message != NULL);
}

/* ── Filter ─────────────────────────────────────────────────────────────── */

/* Matches the search text against the name, description and path. */
static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  if (!self->filter_text) return TRUE;
  SbvOu *ou = sbv_ou_row_get_ou (row);
  if (!ou) return TRUE;

  char *joined = g_strjoinv (" ", (char **) sbv_ou_get_path (ou));
  const char *fields[] = { sbv_ou_get_name (ou), sbv_ou_get_description (ou), joined };
  gboolean hit = FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS (fields) && !hit; i++) {
    if (!fields[i]) continue;
    char *lower = g_utf8_strdown (fields[i], -1);
    hit = strstr (lower, self->filter_text) != NULL;
    g_free (lower);
  }
  g_free (joined);
  return hit;
}

/* Re-runs the list filter when the search entry changes. */
static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  g_free (self->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  self->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* ── Raw attributes ─────────────────────────────────────────────────────── */

/* One row in the "All LDAP Attributes" dump. Mirrors the other panels. */
static GtkWidget *
make_raw_attr_row (const char *attr, const char *value)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top    (box, 2);
  gtk_widget_set_margin_bottom (box, 2);
  GtkWidget *lbl = gtk_label_new (attr);
  gtk_label_set_xalign (GTK_LABEL (lbl), 1.0);
  gtk_widget_set_size_request (lbl, 200, -1);
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

/* Rebuilds the "All LDAP Attributes" dump from the selected entry. */
static void
populate_raw_attrs (GtkWidget *box, GHashTable *attrs)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (box)) != NULL)
    gtk_box_remove (GTK_BOX (box), child);
  if (!attrs) return;
  GList *keys = g_list_sort (g_hash_table_get_keys (attrs), (GCompareFunc) g_strcmp0);
  for (GList *l = keys; l; l = l->next) {
    char **vals   = g_hash_table_lookup (attrs, l->data);
    char  *joined = (vals && vals[0]) ? g_strjoinv ("; ", vals) : g_strdup ("-");
    gtk_box_append (GTK_BOX (box), make_raw_attr_row (l->data, joined));
    g_free (joined);
  }
  g_list_free (keys);
}

/* ── Detail ─────────────────────────────────────────────────────────────── */

/* Fills the detail pane from `ou` and enables only the actions allowed for
 * its kind. Built-in containers and critical OUs are read-only. */
static void
load_ou_into_detail (SbvOusPanel *self, SbvOu *ou)
{
  g_set_object (&self->selected_ou, ou);

  SbvOuKind kind     = sbv_ou_get_kind (ou);
  gboolean  editable = sbv_ou_is_editable (ou);

  gtk_label_set_text (GTK_LABEL (self->heading_label),
    kind == SBV_OU_KIND_OU        ? "Organisational Unit" :
    kind == SBV_OU_KIND_CONTAINER ? "Container" : "Domain");

  gtk_label_set_text (GTK_LABEL (self->dn_label), sbv_ou_get_dn (ou) ?: "");
  gtk_editable_set_text (GTK_EDITABLE (self->name_entry), sbv_ou_get_name (ou) ?: "");
  gtk_editable_set_text (GTK_EDITABLE (self->desc_entry),
                         sbv_ou_get_description (ou) ?: "");

  gtk_widget_set_sensitive (self->name_entry, editable);
  gtk_widget_set_sensitive (self->desc_entry, kind != SBV_OU_KIND_DOMAIN);
  gtk_widget_set_sensitive (self->save_btn,   kind != SBV_OU_KIND_DOMAIN);

  const char *hint = NULL;
  if (kind == SBV_OU_KIND_DOMAIN)
    hint = "The domain root can't be renamed, moved or deleted.";
  else if (!editable)
    hint = "Built-in system object. Only the description can be changed.";
  gtk_label_set_text (GTK_LABEL (self->locked_hint), hint ?: "");
  gtk_widget_set_visible (self->locked_hint, hint != NULL);

  char *parent = NULL;
  sbv_dn_split (sbv_ou_get_dn (ou), NULL, &parent);
  gtk_label_set_text (GTK_LABEL (self->parent_label),
                      kind == SBV_OU_KIND_DOMAIN ? "-" : (parent ?: ""));
  g_free (parent);
  gtk_widget_set_sensitive (self->move_btn, editable);

  gtk_widget_set_visible (self->danger_box, editable);

  populate_raw_attrs (self->raw_attrs_box, sbv_ou_get_ldap_attrs (ou));

  set_error (self->save_error,   NULL);
  set_error (self->delete_error, NULL);
  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "detail");
}

/* Row selection → detail pane; clearing selection shows the placeholder. */
static void
on_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  SbvOu *ou = sbv_ou_row_get_ou (row);
  if (!ou) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    return;
  }
  load_ou_into_detail (self, ou);
}

/* ── Save (rename + description) ────────────────────────────────────────── */

/* Async-completion for save. A rename changes descendant DNs, so other
 * panels are told to reload. */
static void
on_update_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  GError      *err  = NULL;

  gtk_widget_set_sensitive (self->save_btn, TRUE);

  char *new_dn = sbv_ou_update_finish (SBV_CONNECTION (source), result, &err);
  if (!new_dn) {
    set_error (self->save_error, err->message);
    g_error_free (err);
    return;
  }

  gboolean renamed = self->selected_ou &&
    !sbv_dn_equal (new_dn, sbv_ou_get_dn (self->selected_ou));
  reload_selecting (self, new_dn);
  if (renamed)
    g_signal_emit (self, signals[SIGNAL_DIRECTORY_CHANGED], 0);
  g_free (new_dn);
}

/* "Save Changes": sends the new name (OUs only) and description. */
static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  if (!self->selected_ou || !self->conn) return;

  const char *name = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));
  const char *desc = gtk_editable_get_text (GTK_EDITABLE (self->desc_entry));

  if (sbv_ou_is_editable (self->selected_ou) && !*name) {
    set_error (self->save_error, "The name can't be empty.");
    return;
  }

  set_error (self->save_error, NULL);
  gtk_widget_set_sensitive (self->save_btn, FALSE);
  sbv_ou_update_async (self->conn, self->selected_ou,
                       sbv_ou_is_editable (self->selected_ou) ? name : NULL,
                       desc, NULL, on_update_done, self);
}

/* ── Create ─────────────────────────────────────────────────────────────── */

/* Create-dialog completion: reload and select the new OU. */
static void
on_ou_created (SbvConnection *conn, const char *new_dn, gpointer user_data)
{
  (void) conn;
  reload_selecting (SBV_OUS_PANEL (user_data), new_dn);
}

/* "+" button: create an OU inside the selected container, or at the
 * domain root when nothing is selected. */
static void
on_add_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  if (!self->conn) return;
  const char *parent = self->selected_ou ? sbv_ou_get_dn (self->selected_ou) : NULL;
  sbv_create_ou_dialog_show (panel_window (self), self->conn, parent,
                             on_ou_created, self);
}

/* ── Move ───────────────────────────────────────────────────────────────── */

/* Move-dialog completion: reload at the new DN and tell other panels. */
static void
on_ou_moved (SbvConnection *conn, const char *new_dn, gpointer user_data)
{
  (void) conn;
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  reload_selecting (self, new_dn);
  g_signal_emit (self, signals[SIGNAL_DIRECTORY_CHANGED], 0);
}

/* "Move…" button: pick a new parent for the selected OU. */
static void
on_move_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  if (!self->selected_ou || !self->conn) return;
  sbv_move_dialog_show (panel_window (self), self->conn,
                        sbv_ou_get_dn (self->selected_ou),
                        sbv_ou_get_name (self->selected_ou),
                        on_ou_moved, self);
}

/* ── Delete ─────────────────────────────────────────────────────────────── */

/* Async-completion for delete: reload with no selection on success. */
static void
on_delete_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  GError      *err  = NULL;

  gtk_widget_set_sensitive (self->delete_btn, TRUE);

  if (!sbv_ou_delete_finish (SBV_CONNECTION (source), result, &err)) {
    set_error (self->delete_error, err->message);
    g_error_free (err);
    return;
  }
  reload_selecting (self, NULL);
  g_signal_emit (self, signals[SIGNAL_DIRECTORY_CHANGED], 0);
}

/* Type-to-confirm modal state. */
typedef struct {
  SbvOusPanel *panel;
  GtkWindow   *dialog;
  GtkWidget   *delete_btn;
  char        *expected;
} ConfirmDelCtx;

/* Frees the modal context. */
static void
confirm_del_ctx_free (ConfirmDelCtx *ctx)
{
  g_free (ctx->expected);
  g_free (ctx);
}

/* Enables the destructive button only on an exact name match. */
static void
on_confirm_entry_changed (GtkEditable *editable, gpointer user_data)
{
  ConfirmDelCtx *ctx = user_data;
  gtk_widget_set_sensitive (ctx->delete_btn,
                            g_str_equal (gtk_editable_get_text (editable), ctx->expected));
}

/* Fires the delete and closes the modal. */
static void
on_confirm_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  ConfirmDelCtx *ctx  = user_data;
  SbvOusPanel   *self = ctx->panel;
  if (self->conn && self->selected_ou) {
    set_error (self->delete_error, NULL);
    gtk_widget_set_sensitive (self->delete_btn, FALSE);
    sbv_ou_delete_async (self->conn, self->selected_ou, NULL, on_delete_done, self);
  }
  gtk_window_destroy (ctx->dialog);
}

/* "Delete OU…": builds and presents the type-to-confirm modal. */
static void
on_delete_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  if (!self->selected_ou) return;
  const char *name = sbv_ou_get_name (self->selected_ou);
  if (!name || !*name) return;

  ConfirmDelCtx *ctx = g_new0 (ConfirmDelCtx, 1);
  ctx->panel    = self;
  ctx->expected = g_strdup (name);

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Delete OU");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 420, 220);
  GtkWindow *parent = panel_window (self);
  if (parent) gtk_window_set_transient_for (GTK_WINDOW (win), parent);
  ctx->dialog = GTK_WINDOW (win);

  GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (gtk_window_destroy), win);

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

  char *esc  = g_markup_escape_text (name, -1);
  char *text = g_strdup_printf (
    "Permanently delete OU <b>%s</b>?\nThe OU must be empty. This cannot be undone.", esc);
  GtkWidget *warn = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (warn), text);
  gtk_label_set_xalign (GTK_LABEL (warn), 0);
  gtk_label_set_wrap (GTK_LABEL (warn), TRUE);
  g_free (text);
  gtk_box_append (GTK_BOX (body), warn);

  text = g_strdup_printf ("Type the OU name (<tt>%s</tt>) to confirm:", esc);
  GtkWidget *prompt = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (prompt), text);
  gtk_label_set_xalign (GTK_LABEL (prompt), 0);
  gtk_widget_add_css_class (prompt, "dim-label");
  gtk_widget_set_margin_top (prompt, 4);
  g_free (text);
  g_free (esc);
  gtk_box_append (GTK_BOX (body), prompt);

  GtkWidget *entry = gtk_entry_new ();
  gtk_widget_add_css_class (entry, "monospace");
  g_signal_connect (entry, "changed", G_CALLBACK (on_confirm_entry_changed), ctx);
  gtk_box_append (GTK_BOX (body), entry);

  gtk_box_append (GTK_BOX (vbox), body);
  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect_swapped (win, "destroy", G_CALLBACK (confirm_del_ctx_free), ctx);
  gtk_window_present (GTK_WINDOW (win));
  gtk_widget_grab_focus (entry);
}

/* ── Load ───────────────────────────────────────────────────────────────── */

/* Async-completion: rebuild the rows and re-select the pending DN, or the
 * previously selected one. */
static void
on_ous_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  GError      *err  = NULL;

  gtk_spinner_stop (GTK_SPINNER (self->spinner));
  GListStore *store = sbv_ou_list_finish (SBV_CONNECTION (source), result, &err);
  if (!store) {
    g_warning ("Failed to load OUs: %s", err->message);
    g_error_free (err);
    gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "empty");
    return;
  }

  char *target = self->pending_select_dn;
  self->pending_select_dn = NULL;
  if (!target && self->selected_ou)
    target = g_strdup (sbv_ou_get_dn (self->selected_ou));

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint          n        = g_list_model_get_n_items (G_LIST_MODEL (store));
  GtkListBoxRow *reselect = NULL;
  for (guint i = 0; i < n; i++) {
    SbvOu     *ou  = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget *row = sbv_ou_row_new (ou);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
    if (!reselect && target && sbv_dn_equal (sbv_ou_get_dn (ou), target))
      reselect = GTK_LIST_BOX_ROW (row);
    g_object_unref (ou);
  }
  g_object_unref (store);
  g_free (target);

  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack),
                                    n == 0 ? "empty" : "split");
  if (reselect) {
    gtk_list_box_select_row (GTK_LIST_BOX (self->list_box), reselect);
  } else {
    g_clear_object (&self->selected_ou);
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
  }
}

void
sbv_ous_panel_load (SbvOusPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  if (!conn) return;
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "loading");
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  sbv_ou_list_async (conn, NULL, on_ous_loaded, self);
}

/* ── Widget construction ────────────────────────────────────────────────── */

/* Section heading inside the detail form. */
static GtkWidget *
make_section_label (const char *title)
{
  GtkWidget *lbl = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (lbl), 0);
  gtk_widget_add_css_class (lbl, "heading");
  gtk_widget_set_margin_top (lbl, 12);
  return lbl;
}

/* Labelled form row with the standard 140px label column. */
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

/* Left-aligned, selectable monospace label for DN values. */
static GtkWidget *
make_dn_value_label (void)
{
  GtkWidget *lbl = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (lbl), 0);
  gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_selectable (GTK_LABEL (lbl), TRUE);
  gtk_widget_add_css_class (lbl, "monospace");
  return lbl;
}

/* Inline error label indented under the form's value column. */
static GtkWidget *
make_error_label (void)
{
  GtkWidget *lbl = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (lbl), 0);
  gtk_label_set_wrap (GTK_LABEL (lbl), TRUE);
  gtk_widget_add_css_class (lbl, "error");
  gtk_widget_set_margin_start (lbl, 148);
  gtk_widget_set_visible (lbl, FALSE);
  return lbl;
}

/* Sets the paned split the first time the panel is mapped. */
static void
on_paned_map (GtkWidget *widget, gpointer user_data)
{
  SbvOusPanel *self = SBV_OUS_PANEL (user_data);
  if (!self->paned_init && gtk_widget_get_width (widget) > 0) {
    self->paned_init = TRUE;
    gtk_paned_set_position (GTK_PANED (widget), 320);
  }
}

/* Builds the detail form: identity, location, raw attributes, danger zone. */
static GtkWidget *
build_detail_form (SbvOusPanel *self)
{
  GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top    (form, 16);
  gtk_widget_set_margin_bottom (form, 24);
  gtk_widget_set_margin_start  (form, 20);
  gtk_widget_set_margin_end    (form, 20);

  self->heading_label = make_section_label ("Organisational Unit");
  gtk_box_append (GTK_BOX (form), self->heading_label);

  self->dn_label = make_dn_value_label ();
  gtk_box_append (GTK_BOX (form), make_field_row ("DN", self->dn_label));

  self->name_entry = gtk_entry_new ();
  gtk_box_append (GTK_BOX (form), make_field_row ("Name", self->name_entry));

  self->desc_entry = gtk_entry_new ();
  gtk_box_append (GTK_BOX (form), make_field_row ("Description", self->desc_entry));

  self->locked_hint = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->locked_hint), 0);
  gtk_label_set_wrap (GTK_LABEL (self->locked_hint), TRUE);
  gtk_widget_add_css_class (self->locked_hint, "dim-label");
  gtk_widget_add_css_class (self->locked_hint, "caption");
  gtk_widget_set_margin_start (self->locked_hint, 148);
  gtk_box_append (GTK_BOX (form), self->locked_hint);

  self->save_btn = gtk_button_new_with_label ("Save Changes");
  gtk_widget_add_css_class (self->save_btn, "suggested-action");
  gtk_widget_set_halign (self->save_btn, GTK_ALIGN_START);
  gtk_widget_set_margin_start (self->save_btn, 148);
  gtk_widget_set_margin_top (self->save_btn, 8);
  g_signal_connect (self->save_btn, "clicked", G_CALLBACK (on_save_clicked), self);
  gtk_box_append (GTK_BOX (form), self->save_btn);

  self->save_error = make_error_label ();
  gtk_box_append (GTK_BOX (form), self->save_error);

  gtk_box_append (GTK_BOX (form), make_section_label ("Location"));
  {
    GtkWidget *loc = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    self->parent_label = make_dn_value_label ();
    gtk_widget_set_hexpand (self->parent_label, TRUE);
    gtk_box_append (GTK_BOX (loc), self->parent_label);
    self->move_btn = gtk_button_new_with_label ("Move…");
    g_signal_connect (self->move_btn, "clicked", G_CALLBACK (on_move_clicked), self);
    gtk_box_append (GTK_BOX (loc), self->move_btn);
    gtk_box_append (GTK_BOX (form), make_field_row ("Parent", loc));
  }

  gtk_box_append (GTK_BOX (form), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (form), make_section_label ("All LDAP Attributes"));
  self->raw_attrs_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (form), self->raw_attrs_box);

  self->danger_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append (GTK_BOX (self->danger_box),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (self->danger_box), make_section_label ("Danger Zone"));
  self->delete_btn = gtk_button_new_with_label ("Delete OU…");
  gtk_widget_add_css_class (self->delete_btn, "destructive-action");
  gtk_widget_set_halign (self->delete_btn, GTK_ALIGN_START);
  gtk_widget_set_margin_start (self->delete_btn, 148);
  gtk_widget_set_margin_top (self->delete_btn, 4);
  g_signal_connect (self->delete_btn, "clicked", G_CALLBACK (on_delete_btn_clicked), self);
  gtk_box_append (GTK_BOX (self->danger_box), self->delete_btn);
  self->delete_error = make_error_label ();
  gtk_box_append (GTK_BOX (self->danger_box), self->delete_error);
  gtk_box_append (GTK_BOX (form), self->danger_box);

  return form;
}

/* Releases owned state. */
static void
sbv_ous_panel_finalize (GObject *object)
{
  SbvOusPanel *self = SBV_OUS_PANEL (object);
  g_free (self->filter_text);
  g_free (self->pending_select_dn);
  g_clear_object (&self->selected_ou);
  G_OBJECT_CLASS (sbv_ous_panel_parent_class)->finalize (object);
}

/* Registers finalize and the "directory-changed" signal. */
static void
sbv_ous_panel_class_init (SbvOusPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_ous_panel_finalize;
  signals[SIGNAL_DIRECTORY_CHANGED] =
    g_signal_new ("directory-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/* Builds the widget tree: loading/empty/split pages, with the OU list on
 * the left and the detail form on the right. */
static void
sbv_ous_panel_init (SbvOusPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  self->outer_stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->outer_stack, TRUE);

  {
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    self->spinner = gtk_spinner_new ();
    gtk_box_append (GTK_BOX (box), self->spinner);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), box, "loading");
  }
  {
    GtkWidget *empty = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "folder-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (empty), "Couldn't Load OUs");
    adw_status_page_set_description (ADW_STATUS_PAGE (empty),
      "The domain root couldn't be read. Check the connection's base DN.");
    gtk_stack_add_named (GTK_STACK (self->outer_stack), empty, "empty");
  }

  self->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  g_signal_connect (self->paned, "map", G_CALLBACK (on_paned_map), self);

  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top    (toolbar, 6);
  gtk_widget_set_margin_bottom (toolbar, 6);
  gtk_widget_set_margin_start  (toolbar, 8);
  gtk_widget_set_margin_end    (toolbar, 8);

  self->search_entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (self->search_entry, TRUE);
  gtk_widget_set_tooltip_text (self->search_entry, "Search OUs");
  g_signal_connect (self->search_entry, "search-changed",
                    G_CALLBACK (on_search_changed), self);
  gtk_box_append (GTK_BOX (toolbar), self->search_entry);

  self->add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (self->add_btn, "flat");
  gtk_widget_set_tooltip_text (self->add_btn, "Create OU in the selected container");
  g_signal_connect (self->add_btn, "clicked", G_CALLBACK (on_add_btn_clicked), self);
  gtk_box_append (GTK_BOX (toolbar), self->add_btn);

  gtk_box_append (GTK_BOX (left), toolbar);
  gtk_box_append (GTK_BOX (left), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *list_scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (list_scroll, TRUE);
  gtk_widget_set_size_request (list_scroll, 260, -1);
  self->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list_box), GTK_SELECTION_SINGLE);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (self->list_box), filter_func, self, NULL);
  gtk_widget_add_css_class (self->list_box, "boxed-list");
  g_signal_connect (self->list_box, "row-selected", G_CALLBACK (on_row_selected), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (list_scroll), self->list_box);
  gtk_box_append (GTK_BOX (left), list_scroll);
  gtk_paned_set_start_child (GTK_PANED (self->paned), left);

  self->detail_stack = gtk_stack_new ();
  gtk_widget_set_hexpand (self->detail_stack, TRUE);
  GtkWidget *none = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (none), "Select an OU");
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (none), "folder-symbolic");
  gtk_stack_add_named (GTK_STACK (self->detail_stack), none, "none");

  GtkWidget *detail_scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (detail_scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (detail_scroll),
                                 build_detail_form (self));
  gtk_stack_add_named (GTK_STACK (self->detail_stack), detail_scroll, "detail");
  gtk_paned_set_end_child (GTK_PANED (self->paned), self->detail_stack);

  gtk_stack_add_named (GTK_STACK (self->outer_stack), self->paned, "split");
  gtk_box_append (GTK_BOX (self), self->outer_stack);
}

GtkWidget *
sbv_ous_panel_new (void)
{
  return g_object_new (SBV_TYPE_OUS_PANEL, NULL);
}
