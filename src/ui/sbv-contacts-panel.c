#include "sbv-contacts-panel.h"
#include "sbv-create-contact-dialog.h"
#include "../backend/sbv-contacts-backend.h"
#include "../model/sbv-contact.h"

#include <adwaita.h>
#include <string.h>

struct _SbvContactsPanel {
  GtkBox         parent;

  /* ── List pane ── */
  GtkWidget     *outer_stack;    /* "loading" | "empty" | "split" */
  GtkWidget     *search_entry;
  GtkWidget     *add_btn;
  GtkWidget     *list_box;
  GtkWidget     *spinner;

  /* ── Detail pane ── */
  GtkWidget     *detail_stack;   /* "none" | "detail" */

  /* Identity (editable cn / displayName / given / sn / description) */
  GtkWidget     *dn_label;
  GtkWidget     *cn_entry;
  GtkWidget     *display_entry;
  GtkWidget     *given_entry;
  GtkWidget     *sn_entry;
  GtkWidget     *description_entry;

  /* Mail */
  GtkWidget     *mail_entry;
  GtkWidget     *proxy_entry;    /* one per-line address text */

  /* Phone */
  GtkWidget     *tel_entry;
  GtkWidget     *mobile_entry;
  GtkWidget     *fax_entry;

  /* Address */
  GtkWidget     *office_entry;
  GtkWidget     *street_entry;
  GtkWidget     *locality_entry;
  GtkWidget     *state_entry;
  GtkWidget     *postal_entry;
  GtkWidget     *country_entry;

  /* Save */
  GtkWidget     *save_btn;
  GtkWidget     *save_error;

  /* Raw LDAP attributes (rebuilt on selection) */
  GtkWidget     *raw_attrs_box;

  /* Delete */
  GtkWidget     *delete_btn;
  GtkWidget     *delete_error;

  /* Paned split */
  GtkWidget     *paned;
  gboolean       paned_init;

  /* State */
  SbvConnection *conn;             /* unowned */
  SbvContact    *selected_contact; /* owned ref */
  char          *filter_text;
  /* Set by the create flow so the upcoming reload selects the new row. */
  char          *pending_select_dn;
};

G_DEFINE_TYPE (SbvContactsPanel, sbv_contacts_panel, GTK_TYPE_BOX)

/* ── Forward declarations ───────────────────────────────────────────────── */
static void populate_raw_attrs       (GtkWidget *box, GHashTable *attrs, int label_width);
void        sbv_contacts_panel_load  (SbvContactsPanel *self, SbvConnection *conn);

/* ── Row construction ───────────────────────────────────────────────────── */

/* Builds one sidebar list row showing the contact's preferred name and mail. */
static GtkWidget *
make_contact_row (SbvContact *contact)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-contact",
                           g_object_ref (contact), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top    (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *icon = gtk_image_new_from_icon_name ("contact-new-symbolic");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text_box, TRUE);

  const char *display = sbv_contact_get_display_name (contact);
  if (!display || !*display) display = sbv_contact_get_cn (contact);

  GtkWidget *name_label = gtk_label_new (display ? display : "");
  gtk_label_set_xalign (GTK_LABEL (name_label), 0);
  gtk_widget_add_css_class (name_label, "heading");
  gtk_box_append (GTK_BOX (text_box), name_label);

  const char *mail = sbv_contact_get_mail (contact);
  if (mail && *mail) {
    GtkWidget *mail_label = gtk_label_new (mail);
    gtk_label_set_xalign (GTK_LABEL (mail_label), 0);
    gtk_widget_add_css_class (mail_label, "dim-label");
    gtk_box_append (GTK_BOX (text_box), mail_label);
  }

  gtk_box_append (GTK_BOX (box), text_box);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* ── Filter ─────────────────────────────────────────────────────────────── */

/* Returns TRUE if the row should be visible given the current search text.
 * Skips NULL fields explicitly so a missing displayName doesn't hide rows. */
static gboolean
filter_func (GtkListBoxRow *row, gpointer user_data)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  if (!self->filter_text || !*self->filter_text)
    return TRUE;

  SbvContact *contact = g_object_get_data (G_OBJECT (row), "sbv-contact");
  if (!contact) return TRUE;

  const char *fields[] = {
    sbv_contact_get_display_name (contact),
    sbv_contact_get_cn (contact),
    sbv_contact_get_mail (contact),
    sbv_contact_get_telephone_number (contact),
  };

  for (size_t i = 0; i < G_N_ELEMENTS (fields); i++) {
    if (!fields[i]) continue;
    char *lower = g_utf8_strdown (fields[i], -1);
    gboolean match = strstr (lower, self->filter_text) != NULL;
    g_free (lower);
    if (match) return TRUE;
  }
  return FALSE;
}

/* Re-runs the list filter when the search entry changes. */
static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  g_free (self->filter_text);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  self->filter_text = *text ? g_utf8_strdown (text, -1) : NULL;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->list_box));
}

/* ── Detail loading ─────────────────────────────────────────────────────── */

/* Joins a GStrv with "\n" for editing in a multi-line text view. Returns
 * an empty string when the vector is NULL or empty. Caller frees. */
static char *
strv_to_lines (const char *const *v)
{
  if (!v || !v[0]) return g_strdup ("");
  return g_strjoinv ("\n", (char **) v);
}

/* Splits the textview body into a GStrv by newlines, discarding empty lines.
 * Returns NULL on a fully-empty body (caller passes that to the backend as
 * "leave alone"). Otherwise returns an allocated NULL-terminated vector. */
static char **
lines_to_strv (const char *text)
{
  if (!text) return NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  const char *p = text;
  while (*p) {
    const char *nl = strchr (p, '\n');
    gsize len = nl ? (gsize) (nl - p) : strlen (p);
    if (len > 0) {
      char *line = g_strndup (p, len);
      g_strstrip (line);
      if (*line) g_ptr_array_add (out, line);
      else       g_free (line);
    }
    if (!nl) break;
    p = nl + 1;
  }
  g_ptr_array_add (out, NULL);
  /* If only the terminator was added, treat as empty (clear). */
  return (char **) g_ptr_array_free (out, FALSE);
}

/* Pulls the buffered text out of a multi-line GtkTextView. Caller frees. */
static char *
textview_get_text (GtkWidget *tv)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (tv));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buf, &start, &end);
  return gtk_text_buffer_get_text (buf, &start, &end, FALSE);
}

/* Replaces the textview body. Pass NULL for empty. */
static void
textview_set_text (GtkWidget *tv, const char *text)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (tv));
  gtk_text_buffer_set_text (buf, text ? text : "", -1);
}

/* Populates every detail-form widget from the selected contact and switches
 * the detail stack to the "detail" page. */
static void
load_contact_into_detail (SbvContactsPanel *self, SbvContact *contact)
{
  g_set_object (&self->selected_contact, contact);

  gtk_label_set_text (GTK_LABEL (self->dn_label),
                      sbv_contact_get_dn (contact) ?: "");

#define SET_ENTRY(w, getter) \
  gtk_editable_set_text (GTK_EDITABLE (self->w), \
                         getter (contact) ? getter (contact) : "");

  SET_ENTRY (cn_entry,           sbv_contact_get_cn)
  SET_ENTRY (display_entry,      sbv_contact_get_display_name)
  SET_ENTRY (given_entry,        sbv_contact_get_given_name)
  SET_ENTRY (sn_entry,           sbv_contact_get_sn)
  SET_ENTRY (description_entry,  sbv_contact_get_description)
  SET_ENTRY (mail_entry,         sbv_contact_get_mail)
  SET_ENTRY (tel_entry,          sbv_contact_get_telephone_number)
  SET_ENTRY (mobile_entry,       sbv_contact_get_mobile)
  SET_ENTRY (fax_entry,          sbv_contact_get_fax)
  SET_ENTRY (office_entry,       sbv_contact_get_office)
  SET_ENTRY (street_entry,       sbv_contact_get_street_address)
  SET_ENTRY (locality_entry,     sbv_contact_get_locality)
  SET_ENTRY (state_entry,        sbv_contact_get_state)
  SET_ENTRY (postal_entry,       sbv_contact_get_postal_code)
  SET_ENTRY (country_entry,      sbv_contact_get_country)

#undef SET_ENTRY

  char *proxy = strv_to_lines (sbv_contact_get_proxy_addresses (contact));
  textview_set_text (self->proxy_entry, proxy);
  g_free (proxy);

  populate_raw_attrs (self->raw_attrs_box, sbv_contact_get_ldap_attrs (contact), 200);

  gtk_label_set_text   (GTK_LABEL (self->save_error),   "");
  gtk_label_set_text   (GTK_LABEL (self->delete_error), "");
  gtk_widget_set_visible (self->save_error,   FALSE);
  gtk_widget_set_visible (self->delete_error, FALSE);

  gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "detail");
}

/* Row selection → detail page swap; clearing selection returns to "none". */
static void
on_row_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);

  if (!row) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->detail_stack), "none");
    return;
  }

  SbvContact *contact = g_object_get_data (G_OBJECT (row), "sbv-contact");
  if (contact)
    load_contact_into_detail (self, contact);
}

/* ── Save ───────────────────────────────────────────────────────────────── */

/* Async-completion: on success reload to pick up DC-side normalisation; on
 * error surface the message inline. */
static void
on_update_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  SbvConnection    *conn = SBV_CONNECTION (source);
  GError           *err  = NULL;

  gtk_widget_set_sensitive (self->save_btn, TRUE);

  if (!sbv_contacts_update_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->save_error), err->message);
    gtk_widget_set_visible (self->save_error, TRUE);
    g_error_free (err);
    return;
  }
  sbv_contacts_panel_load (self, conn);
}

/* "Save Changes" click handler — builds the modify call from the form. */
static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  if (!self->selected_contact || !self->conn) return;

  gtk_widget_set_visible (self->save_error, FALSE);
  gtk_widget_set_sensitive (self->save_btn, FALSE);

  /* Pass current cn only if it differs from the loaded value — saves an
   * ldap_rename round-trip when the user only edited other fields. */
  const char *form_cn   = gtk_editable_get_text (GTK_EDITABLE (self->cn_entry));
  const char *loaded_cn = sbv_contact_get_cn (self->selected_contact);
  const char *cn_arg    = g_strcmp0 (form_cn, loaded_cn ?: "") != 0 ? form_cn : NULL;

  char  *proxy_text = textview_get_text (self->proxy_entry);
  char **proxy_vec  = lines_to_strv (proxy_text);
  g_free (proxy_text);

  sbv_contacts_update_async (self->conn, self->selected_contact,
    cn_arg,
    gtk_editable_get_text (GTK_EDITABLE (self->display_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->given_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->sn_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->description_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->mail_entry)),
    (const char *const *) proxy_vec,
    gtk_editable_get_text (GTK_EDITABLE (self->tel_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->mobile_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->fax_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->office_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->street_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->locality_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->state_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->postal_entry)),
    gtk_editable_get_text (GTK_EDITABLE (self->country_entry)),
    NULL, on_update_done, self);

  g_strfreev (proxy_vec);
}

/* ── Create / Delete ────────────────────────────────────────────────────── */

/* Records the new DN so the upcoming reload auto-selects that row. */
static void
on_contact_created (SbvConnection *conn, const char *new_dn, gpointer user_data)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  g_free (self->pending_select_dn);
  self->pending_select_dn = g_strdup (new_dn);
  sbv_contacts_panel_load (self, conn);
}

/* Toolbar "+" button: opens the create-contact dialog. */
static void
on_add_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  if (!self->conn) return;

  GtkWidget *parent = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  sbv_create_contact_dialog_show (parent ? GTK_WINDOW (parent) : NULL,
                                    self->conn, on_contact_created, self);
}

/* Async-completion for delete: reload the list on success, surface error
 * inline on failure. */
static void
on_delete_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  SbvConnection    *conn = SBV_CONNECTION (source);
  GError           *err  = NULL;

  gtk_widget_set_sensitive (self->delete_btn, TRUE);

  if (!sbv_contacts_delete_finish (conn, result, &err)) {
    gtk_label_set_text (GTK_LABEL (self->delete_error), err->message);
    gtk_widget_set_visible (self->delete_error, TRUE);
    g_error_free (err);
    return;
  }
  g_clear_object (&self->selected_contact);
  sbv_contacts_panel_load (self, conn);
}

/* Type-to-confirm modal state. */
typedef struct {
  SbvContactsPanel *panel;
  GtkWindow        *dialog;
  GtkWidget        *entry;
  GtkWidget        *delete_btn;
  char             *expected;
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
  ConfirmDelCtx *ctx   = user_data;
  const char    *typed = gtk_editable_get_text (editable);
  gtk_widget_set_sensitive (ctx->delete_btn,
                             g_str_equal (typed, ctx->expected));
}

/* Fires the delete and dismisses the modal. */
static void
on_confirm_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  ConfirmDelCtx    *ctx  = user_data;
  SbvContactsPanel *self = ctx->panel;
  if (!self->conn || !self->selected_contact) {
    gtk_window_destroy (ctx->dialog);
    return;
  }

  gtk_widget_set_visible (self->delete_error, FALSE);
  gtk_widget_set_sensitive (self->delete_btn, FALSE);

  sbv_contacts_delete_async (self->conn, self->selected_contact,
                              NULL, on_delete_done, self);
  gtk_window_destroy (ctx->dialog);
}

/* Builds and presents the type-to-confirm delete modal. */
static void
show_delete_confirm (SbvContactsPanel *self)
{
  if (!self->selected_contact) return;
  const char *cn = sbv_contact_get_cn (self->selected_contact);
  if (!cn || !*cn) return;

  ConfirmDelCtx *ctx = g_new0 (ConfirmDelCtx, 1);
  ctx->panel    = self;
  ctx->expected = g_strdup (cn);

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Delete Contact");
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
    "Permanently delete contact <b>%s</b>?\nThis cannot be undone.", cn);
  GtkWidget *warn = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (warn), warn_text);
  gtk_label_set_xalign (GTK_LABEL (warn), 0);
  gtk_label_set_wrap   (GTK_LABEL (warn), TRUE);
  g_free (warn_text);
  gtk_box_append (GTK_BOX (body), warn);

  char *prompt_text = g_strdup_printf (
    "Type the contact name (<tt>%s</tt>) to confirm:", cn);
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

/* "Delete Contact…" button → confirmation modal. */
static void
on_delete_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  show_delete_confirm (SBV_CONTACTS_PANEL (user_data));
}

/* ── Load callback ──────────────────────────────────────────────────────── */

/* Async-completion: re-build the list rows and attempt to re-select the
 * previously-active contact (by DN if pending_select_dn was set, otherwise
 * by cn from the prior selection). */
static void
on_contacts_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
  GError           *err  = NULL;

  GListStore *store = sbv_contacts_list_finish (SBV_CONNECTION (source),
                                                  result, &err);
  if (!store) {
    g_warning ("Failed to load contacts: %s", err->message);
    g_error_free (err);
    gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "empty");
    return;
  }

  char *target_dn = NULL;
  char *target_cn = NULL;
  if (self->pending_select_dn) {
    target_dn = self->pending_select_dn;
    self->pending_select_dn = NULL;
  } else if (self->selected_contact) {
    target_cn = g_strdup (sbv_contact_get_cn (self->selected_contact));
  }

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  GtkListBoxRow *reselect = NULL;

  for (guint i = 0; i < n; i++) {
    SbvContact *contact = g_list_model_get_item (G_LIST_MODEL (store), i);
    GtkWidget  *row     = make_contact_row (contact);
    gtk_list_box_append (GTK_LIST_BOX (self->list_box), row);
    if (!reselect) {
      if (target_dn && g_str_equal (sbv_contact_get_dn (contact) ?: "", target_dn))
        reselect = GTK_LIST_BOX_ROW (row);
      else if (target_cn && g_str_equal (sbv_contact_get_cn (contact) ?: "", target_cn))
        reselect = GTK_LIST_BOX_ROW (row);
    }
    g_object_unref (contact);
  }

  g_object_unref (store);
  g_free (target_dn);
  g_free (target_cn);

  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack),
                                     n == 0 ? "empty" : "split");

  if (reselect)
    gtk_list_box_select_row (GTK_LIST_BOX (self->list_box), reselect);
}

/* Kicks off an async list load; switches the outer stack to "loading". */
void
sbv_contacts_panel_load (SbvContactsPanel *self, SbvConnection *conn)
{
  self->conn = conn;
  gtk_stack_set_visible_child_name (GTK_STACK (self->outer_stack), "loading");
  gtk_spinner_start (GTK_SPINNER (self->spinner));
  sbv_contacts_list_async (conn, NULL, on_contacts_loaded, self);
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

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

/* One row in the "All LDAP Attributes" dump. Mirrors the helper used in the
 * other panels — kept local to avoid a UI-helper cross-file dependency. */
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

/* Rebuilds the "All LDAP Attributes" dump from a fresh hashtable. */
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

/* Initialises the paned split position the first time the panel is mapped. */
static void
on_paned_map (GtkWidget *widget, gpointer user_data)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (user_data);
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
sbv_contacts_panel_finalize (GObject *object)
{
  SbvContactsPanel *self = SBV_CONTACTS_PANEL (object);
  g_free (self->filter_text);
  g_free (self->pending_select_dn);
  g_clear_object (&self->selected_contact);
  G_OBJECT_CLASS (sbv_contacts_panel_parent_class)->finalize (object);
}

static void
sbv_contacts_panel_class_init (SbvContactsPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_contacts_panel_finalize;
}

/* Builds the panel's widget tree: an outer stack with loading/empty/split
 * pages; split holds the search-list on the left and a detail scroll on
 * the right. */
static void
sbv_contacts_panel_init (SbvContactsPanel *self)
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
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "contact-new-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Contacts Found");
    adw_status_page_set_description (ADW_STATUS_PAGE (empty),
      "Create a mail-enabled contact to get started.");
    GtkWidget *create_btn = gtk_button_new_with_label ("Create Contact…");
    gtk_widget_add_css_class (create_btn, "suggested-action");
    gtk_widget_add_css_class (create_btn, "pill");
    gtk_widget_set_halign (create_btn, GTK_ALIGN_CENTER);
    g_signal_connect (create_btn, "clicked",
                      G_CALLBACK (on_add_btn_clicked), self);
    adw_status_page_set_child (ADW_STATUS_PAGE (empty), create_btn);
    gtk_stack_add_named (GTK_STACK (self->outer_stack), empty, "empty");
  }

  {
    self->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *paned = self->paned;
    g_signal_connect (paned, "map", G_CALLBACK (on_paned_map), self);

    /* Left: search + list */
    GtkWidget *left_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top    (toolbar, 6);
    gtk_widget_set_margin_bottom (toolbar, 6);
    gtk_widget_set_margin_start  (toolbar, 8);
    gtk_widget_set_margin_end    (toolbar, 8);

    self->search_entry = gtk_search_entry_new ();
    gtk_widget_set_hexpand (self->search_entry, TRUE);
    gtk_widget_set_tooltip_text (self->search_entry, "Search contacts");
    g_signal_connect (self->search_entry, "search-changed",
                      G_CALLBACK (on_search_changed), self);
    gtk_box_append (GTK_BOX (toolbar), self->search_entry);

    self->add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_add_css_class (self->add_btn, "flat");
    gtk_widget_set_tooltip_text (self->add_btn, "Create contact");
    g_signal_connect (self->add_btn, "clicked",
                      G_CALLBACK (on_add_btn_clicked), self);
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

    /* Right: detail stack */
    self->detail_stack = gtk_stack_new ();
    gtk_widget_set_hexpand (self->detail_stack, TRUE);

    GtkWidget *none_page = adw_status_page_new ();
    adw_status_page_set_title (ADW_STATUS_PAGE (none_page), "Select a Contact");
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (none_page),
                                    "contact-new-symbolic");
    gtk_stack_add_named (GTK_STACK (self->detail_stack), none_page, "none");

    GtkWidget *detail_scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (detail_scroll, TRUE);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top    (form, 16);
    gtk_widget_set_margin_bottom (form, 24);
    gtk_widget_set_margin_start  (form, 20);
    gtk_widget_set_margin_end    (form, 20);

    /* Identity */
    gtk_box_append (GTK_BOX (form), make_section_label ("Identity"));

    self->dn_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->dn_label), 0);
    gtk_label_set_ellipsize (GTK_LABEL (self->dn_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable (GTK_LABEL (self->dn_label), TRUE);
    gtk_widget_add_css_class (self->dn_label, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("DN", self->dn_label));

    self->cn_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Full Name (cn)", self->cn_entry));

    self->display_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Display Name", self->display_entry));

    self->given_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("First Name", self->given_entry));

    self->sn_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Last Name", self->sn_entry));

    self->description_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Description", self->description_entry));

    /* Mail */
    gtk_box_append (GTK_BOX (form), make_section_label ("Email"));

    self->mail_entry = gtk_entry_new ();
    gtk_widget_add_css_class (self->mail_entry, "monospace");
    gtk_box_append (GTK_BOX (form), make_field_row ("Primary Email", self->mail_entry));

    self->proxy_entry = gtk_text_view_new ();
    gtk_widget_add_css_class (self->proxy_entry, "monospace");
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->proxy_entry), TRUE);
    gtk_widget_set_size_request (self->proxy_entry, -1, 80);
    GtkWidget *proxy_frame = gtk_frame_new (NULL);
    gtk_frame_set_child (GTK_FRAME (proxy_frame), self->proxy_entry);
    gtk_box_append (GTK_BOX (form),
                    make_field_row ("Proxy Addresses", proxy_frame));

    GtkWidget *proxy_hint = gtk_label_new (
      "One value per line. Use AD prefixes: SMTP: (primary), smtp: (secondary).");
    gtk_label_set_xalign (GTK_LABEL (proxy_hint), 0);
    gtk_label_set_wrap   (GTK_LABEL (proxy_hint), TRUE);
    gtk_widget_add_css_class (proxy_hint, "dim-label");
    gtk_widget_add_css_class (proxy_hint, "caption");
    gtk_widget_set_margin_start (proxy_hint, 148);
    gtk_box_append (GTK_BOX (form), proxy_hint);

    /* Phone */
    gtk_box_append (GTK_BOX (form), make_section_label ("Phone"));

    self->tel_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Telephone", self->tel_entry));

    self->mobile_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Mobile", self->mobile_entry));

    self->fax_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Fax", self->fax_entry));

    /* Address */
    gtk_box_append (GTK_BOX (form), make_section_label ("Address"));

    self->office_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Office", self->office_entry));

    self->street_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Street", self->street_entry));

    self->locality_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("City (l)", self->locality_entry));

    self->state_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("State (st)", self->state_entry));

    self->postal_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Postal Code", self->postal_entry));

    self->country_entry = gtk_entry_new ();
    gtk_box_append (GTK_BOX (form), make_field_row ("Country (co)", self->country_entry));

    /* Save */
    {
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
    }

    /* Raw attrs */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("All LDAP Attributes"));
    self->raw_attrs_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (form), self->raw_attrs_box);

    /* Danger zone */
    gtk_box_append (GTK_BOX (form),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (form), make_section_label ("Danger Zone"));
    {
      GtkWidget *del_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_top (del_box, 4);

      self->delete_btn = gtk_button_new_with_label ("Delete Contact…");
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

/* Allocates a fresh, unloaded panel. */
GtkWidget *
sbv_contacts_panel_new (void)
{
  return g_object_new (SBV_TYPE_CONTACTS_PANEL, NULL);
}
