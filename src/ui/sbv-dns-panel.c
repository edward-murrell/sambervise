#include "sbv-dns-panel.h"
#include "sbv-window.h"
#include "../backend/sbv-dnszone-backend.h"
#include "../model/sbv-search.h"

#include <adwaita.h>
#include <string.h>

struct _SbvDnsPanel {
  GtkBox         parent;

  SbvConnection *conn;
  GPtrArray     *zones;          /* SbvDnsZone, or NULL */
  char          *zone_dn;        /* selected zone */
  char          *zone_name;
  char          *reselect_zone;  /* zone name to select after a reload */
  guint          zones_gen;
  guint          records_gen;
  char          *filter_text;
  char          *sample_a;       /* an A address in the zone, for hints */
  char          *sample_aaaa;    /* an AAAA address in the zone */

  GtkWidget *zone_list;
  GtkWidget *zone_status;
  GtkWidget *records_stack;      /* "none" | "loading" | "list" | "error" */
  GtkWidget *records_list;
  GtkWidget *records_error;
  GtkWidget *zone_title;
  GtkWidget *search_entry;
  GtkWidget *add_btn;
};

G_DEFINE_TYPE (SbvDnsPanel, sbv_dns_panel, GTK_TYPE_BOX)

static void load_records (SbvDnsPanel *self);

/* Reply context: the panel plus the generation it was sent for. */
typedef struct {
  SbvDnsPanel *self;   /* owned ref */
  guint        gen;
} ReplyCtx;

/* Makes a reply context. */
static ReplyCtx *
reply_ctx_new (SbvDnsPanel *self, guint gen)
{
  ReplyCtx *ctx = g_new0 (ReplyCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->gen  = gen;
  return ctx;
}

/* Frees a reply context. */
static void
reply_ctx_free (ReplyCtx *ctx)
{
  g_object_unref (ctx->self);
  g_free (ctx);
}

/* Shows a toast on the main window. */
static void
toast (SbvDnsPanel *self, const char *text)
{
  GtkWidget *win = gtk_widget_get_ancestor (GTK_WIDGET (self), SBV_TYPE_WINDOW);
  if (win) sbv_window_show_toast (SBV_WINDOW (win), text);
}

/* Returns the panel's top-level window, or NULL. */
static GtkWindow *
parent_window (SbvDnsPanel *self)
{
  GtkWidget *win = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);
  return win ? GTK_WINDOW (win) : NULL;
}

/* Removes every row from a list box. */
static void
clear_list (GtkWidget *list)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (list), child);
}

/* ── Record dialog ──────────────────────────────────────────────────────── */

static const guint16 EDIT_TYPES[] = {
  SBV_DNS_TYPE_A, SBV_DNS_TYPE_AAAA, SBV_DNS_TYPE_CNAME, SBV_DNS_TYPE_PTR,
};

/* State for one Add or Edit dialog. One ref for the window and one per
 * call in flight. `alive` clears when the window goes. */
typedef struct {
  SbvDnsPanel  *panel;       /* owned ref */
  GtkWindow    *window;
  char         *zone_dn;
  char         *zone_name;
  char         *node_dn;     /* edit only */
  SbvDnsRecord *old;         /* edit only; a copy */

  GtkWidget *name_entry;
  GtkWidget *type_drop;
  GtkWidget *value_entry;
  GtkWidget *ttl_spin;
  GtkWidget *error_label;
  GtkWidget *save_btn;
  GtkWidget *spinner;

  int        refs;
  gboolean   alive;
} RecordCtx;

/* Drops one reference and frees the context on the last one. */
static void
record_ctx_unref (RecordCtx *ctx)
{
  if (--ctx->refs > 0) return;
  g_object_unref (ctx->panel);
  g_free (ctx->zone_dn);
  g_free (ctx->zone_name);
  g_free (ctx->node_dn);
  sbv_dns_record_free (ctx->old);
  g_free (ctx);
}

/* Window "destroy": mark the widgets gone. */
static void
on_record_window_destroy (GtkWidget *w, gpointer user_data)
{
  (void) w;
  RecordCtx *ctx = user_data;
  ctx->alive = FALSE;
  record_ctx_unref (ctx);
}

/* Shows an error in the dialog. */
static void
record_error (RecordCtx *ctx, const char *msg)
{
  gtk_label_set_text (GTK_LABEL (ctx->error_label), msg);
  gtk_widget_set_visible (ctx->error_label, TRUE);
}

/* Turns what the user typed into a name relative to the zone. "" and the
 * zone's own name become "@", and a trailing ".<zone>" is dropped.
 * Caller frees. */
static char *
relative_name (const char *typed, const char *zone)
{
  char *s = g_strstrip (g_strdup (typed ? typed : ""));
  gsize n = strlen (s);
  if (n && s[n - 1] == '.') s[--n] = '\0';
  if (n == 0 || g_ascii_strcasecmp (s, zone) == 0) {
    g_free (s);
    return g_strdup ("@");
  }
  char *suffix = g_strconcat (".", zone, NULL);
  gsize sl     = strlen (suffix);
  if (n > sl && g_ascii_strcasecmp (s + n - sl, suffix) == 0)
    s[n - sl] = '\0';
  g_free (suffix);
  return s;
}

/* Save completion: close and reload, or show the DC's reason. */
static void
on_record_saved (GObject *source, GAsyncResult *result, gpointer user_data)
{
  RecordCtx *ctx = user_data;
  GError    *err = NULL;
  gboolean   ok  = sbv_dns_record_change_finish (SBV_CONNECTION (source), result, &err);

  if (ok) {
    toast (ctx->panel, ctx->old ? "Record updated" : "Record added");
    load_records (ctx->panel);
    if (ctx->alive) gtk_window_destroy (ctx->window);
  } else if (ctx->alive) {
    gtk_widget_set_visible (ctx->spinner, FALSE);
    gtk_widget_set_sensitive (ctx->save_btn, TRUE);
    record_error (ctx, err->message);
  }
  g_clear_error (&err);
  record_ctx_unref (ctx);
}

/* Save button: check the input, then add or update. */
static void
on_record_save (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  RecordCtx *ctx   = user_data;
  guint16    type  = ctx->old ? ctx->old->type
                              : EDIT_TYPES[gtk_drop_down_get_selected (GTK_DROP_DOWN (ctx->type_drop))];
  const char *value = gtk_editable_get_text (GTK_EDITABLE (ctx->value_entry));
  guint32     ttl   = (guint32) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (ctx->ttl_spin));
  SbvConnection *conn = ctx->panel->conn;
  if (!conn) return;

  /* Check the value now so the user gets the message straight away. */
  GError *err   = NULL;
  GBytes *probe = sbv_dns_record_build (type, value, ttl, 0, &err);
  if (!probe) {
    record_error (ctx, err->message);
    g_error_free (err);
    return;
  }
  g_bytes_unref (probe);

  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_widget_set_sensitive (ctx->save_btn, FALSE);
  gtk_widget_set_visible (ctx->spinner, TRUE);
  ctx->refs++;

  if (ctx->old) {
    sbv_dns_record_update_async (conn, ctx->zone_dn, ctx->node_dn, ctx->old,
                                 value, ttl, NULL, on_record_saved, ctx);
    return;
  }

  char *name = relative_name (gtk_editable_get_text (GTK_EDITABLE (ctx->name_entry)),
                              ctx->zone_name);
  if (!sbv_dns_name_valid (name, TRUE)) {
    ctx->refs--;
    gtk_widget_set_sensitive (ctx->save_btn, TRUE);
    gtk_widget_set_visible (ctx->spinner, FALSE);
    record_error (ctx, "Enter a name such as \"host\", or leave it empty for the zone itself.");
    g_free (name);
    return;
  }
  sbv_dns_record_add_async (conn, ctx->zone_dn, name, type, value, ttl,
                            NULL, on_record_saved, ctx);
  g_free (name);
}

/* Updates the value hint to suit the chosen type. */
static void
on_type_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void) obj;
  (void) pspec;
  RecordCtx *ctx  = user_data;
  guint16    type = EDIT_TYPES[gtk_drop_down_get_selected (GTK_DROP_DOWN (ctx->type_drop))];
  char *hint = sbv_dns_value_hint (type,
      type == SBV_DNS_TYPE_A    ? ctx->panel->sample_a
    : type == SBV_DNS_TYPE_AAAA ? ctx->panel->sample_aaaa : NULL);
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->value_entry), hint);
  g_free (hint);
}

/* Builds a labelled form row. */
static GtkWidget *
form_row (const char *label, GtkWidget *widget)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lbl = gtk_label_new (label);
  gtk_label_set_xalign (GTK_LABEL (lbl), 1);
  gtk_widget_set_size_request (lbl, 90, -1);
  gtk_widget_add_css_class (lbl, "dim-label");
  gtk_box_append (GTK_BOX (box), lbl);
  gtk_widget_set_hexpand (widget, TRUE);
  gtk_box_append (GTK_BOX (box), widget);
  return box;
}

/* Opens the Add dialog, or the Edit dialog when `entry` is set. */
static void
show_record_dialog (SbvDnsPanel *self, SbvDnsEntry *entry)
{
  if (!self->conn || !self->zone_dn) return;

  RecordCtx *ctx = g_new0 (RecordCtx, 1);
  ctx->panel     = g_object_ref (self);
  ctx->zone_dn   = g_strdup (self->zone_dn);
  ctx->zone_name = g_strdup (self->zone_name);
  ctx->refs      = 1;
  ctx->alive     = TRUE;
  if (entry) {
    gsize         len = 0;
    const guint8 *d   = g_bytes_get_data (entry->record->raw, &len);
    ctx->node_dn = g_strdup (entry->dn);
    ctx->old     = sbv_dns_record_parse (d, len);
  }

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), entry ? "Edit Record" : "Add Record");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 460, -1);
  GtkWindow *parent = parent_window (self);
  if (parent) gtk_window_set_transient_for (GTK_WINDOW (win), parent);
  ctx->window = GTK_WINDOW (win);

  GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label ("Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (gtk_window_destroy), win);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  ctx->save_btn = gtk_button_new_with_label (entry ? "Save" : "Add");
  gtk_widget_add_css_class (ctx->save_btn, "suggested-action");
  g_signal_connect (ctx->save_btn, "clicked", G_CALLBACK (on_record_save), ctx);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->save_btn);
  ctx->spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (ctx->spinner));
  gtk_widget_set_visible (ctx->spinner, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->spinner);
  gtk_box_append (GTK_BOX (vbox), header);

  GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top    (form, 16);
  gtk_widget_set_margin_bottom (form, 16);
  gtk_widget_set_margin_start  (form, 16);
  gtk_widget_set_margin_end    (form, 16);

  ctx->name_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->name_entry), "Empty for the zone itself");
  GtkWidget *name_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (ctx->name_entry, TRUE);
  gtk_box_append (GTK_BOX (name_box), ctx->name_entry);
  char      *suffix_text = g_strdup_printf (".%s", self->zone_name);
  GtkWidget *suffix      = gtk_label_new (suffix_text);
  gtk_label_set_ellipsize (GTK_LABEL (suffix), PANGO_ELLIPSIZE_START);
  gtk_widget_add_css_class (suffix, "dim-label");
  gtk_box_append (GTK_BOX (name_box), suffix);
  g_free (suffix_text);
  gtk_box_append (GTK_BOX (form), form_row ("Name", name_box));

  const char *type_names[] = { "A", "AAAA", "CNAME", "PTR", NULL };
  ctx->type_drop = gtk_drop_down_new_from_strings (type_names);
  gtk_box_append (GTK_BOX (form), form_row ("Type", ctx->type_drop));

  ctx->value_entry = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (ctx->value_entry), TRUE);
  gtk_box_append (GTK_BOX (form), form_row ("Value", ctx->value_entry));

  ctx->ttl_spin = gtk_spin_button_new_with_range (0, 2147483647, 60);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (ctx->ttl_spin), SBV_DNS_DEFAULT_TTL);
  GtkWidget *ttl_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (ttl_box), ctx->ttl_spin);
  GtkWidget *secs = gtk_label_new ("seconds");
  gtk_widget_add_css_class (secs, "dim-label");
  gtk_box_append (GTK_BOX (ttl_box), secs);
  gtk_box_append (GTK_BOX (form), form_row ("TTL", ttl_box));

  ctx->error_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (ctx->error_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (ctx->error_label), 0);
  gtk_widget_add_css_class (ctx->error_label, "error");
  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_box_append (GTK_BOX (form), ctx->error_label);

  if (entry) {
    /* Name and type are fixed. To change those, delete and re-add. */
    gtk_editable_set_text (GTK_EDITABLE (ctx->name_entry),
                           strcmp (entry->name, "@") == 0 ? "" : entry->name);
    gtk_widget_set_sensitive (ctx->name_entry, FALSE);
    for (guint i = 0; i < G_N_ELEMENTS (EDIT_TYPES); i++)
      if (EDIT_TYPES[i] == entry->record->type)
        gtk_drop_down_set_selected (GTK_DROP_DOWN (ctx->type_drop), i);
    gtk_widget_set_sensitive (ctx->type_drop, FALSE);
    gtk_editable_set_text (GTK_EDITABLE (ctx->value_entry), entry->record->value);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (ctx->ttl_spin), entry->record->ttl);
  }
  g_signal_connect (ctx->type_drop, "notify::selected", G_CALLBACK (on_type_changed), ctx);
  on_type_changed (NULL, NULL, ctx);

  gtk_box_append (GTK_BOX (vbox), form);
  gtk_window_set_child (GTK_WINDOW (win), vbox);
  gtk_window_set_default_widget (GTK_WINDOW (win), ctx->save_btn);
  g_signal_connect (win, "destroy", G_CALLBACK (on_record_window_destroy), ctx);
  gtk_window_present (GTK_WINDOW (win));
  gtk_widget_grab_focus (entry ? ctx->value_entry : ctx->name_entry);
}

/* ── Delete ─────────────────────────────────────────────────────────────── */

/* Delete completion: reload, or report. */
static void
on_record_deleted (GObject *source, GAsyncResult *result, gpointer user_data)
{
  SbvDnsPanel *self = SBV_DNS_PANEL (user_data);
  GError      *err  = NULL;

  if (sbv_dns_record_change_finish (SBV_CONNECTION (source), result, &err)) {
    toast (self, "Record deleted");
  } else {
    GtkWidget *dlg = adw_message_dialog_new (parent_window (self),
                                             "Couldn't delete the record", err->message);
    adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "close", "_Close");
    gtk_window_present (GTK_WINDOW (dlg));
    g_error_free (err);
  }
  load_records (self);
  g_object_unref (self);
}

/* Confirmation reply: delete the record on "delete". */
static void
on_delete_response (AdwMessageDialog *dlg, const char *response, gpointer user_data)
{
  (void) dlg;
  SbvDnsEntry *entry = user_data;
  SbvDnsPanel *self  = g_object_get_data (G_OBJECT (dlg), "sbv-panel");
  if (g_strcmp0 (response, "delete") == 0 && self->conn && self->zone_dn)
    sbv_dns_record_delete_async (self->conn, self->zone_dn, entry->dn,
                                 entry->record, NULL, on_record_deleted,
                                 g_object_ref (self));
}

/* Row's delete button: ask first. */
static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
  SbvDnsPanel *self  = SBV_DNS_PANEL (user_data);
  SbvDnsEntry *entry = g_object_get_data (G_OBJECT (btn), "sbv-entry");
  if (!entry) return;

  char *type = sbv_dns_type_name (entry->record->type);
  char *body = g_strdup_printf ("%s %s %s", entry->name, type, entry->record->value);
  GtkWidget *dlg = adw_message_dialog_new (parent_window (self), "Delete this record?", body);
  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "cancel", "_Cancel");
  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dlg), "delete", "_Delete");
  adw_message_dialog_set_response_appearance (ADW_MESSAGE_DIALOG (dlg), "delete",
                                              ADW_RESPONSE_DESTRUCTIVE);
  adw_message_dialog_set_default_response (ADW_MESSAGE_DIALOG (dlg), "cancel");
  adw_message_dialog_set_close_response (ADW_MESSAGE_DIALOG (dlg), "cancel");
  g_object_set_data_full (G_OBJECT (dlg), "sbv-panel", g_object_ref (self), g_object_unref);

  /* The row may be rebuilt while the dialog is open, so keep a copy. */
  SbvDnsEntry  *copy = g_new0 (SbvDnsEntry, 1);
  gsize         len  = 0;
  const guint8 *d    = g_bytes_get_data (entry->record->raw, &len);
  copy->name   = g_strdup (entry->name);
  copy->dn     = g_strdup (entry->dn);
  copy->record = sbv_dns_record_parse (d, len);
  g_object_set_data_full (G_OBJECT (dlg), "sbv-entry", copy,
                          (GDestroyNotify) sbv_dns_entry_free);
  g_signal_connect (dlg, "response", G_CALLBACK (on_delete_response), copy);
  gtk_window_present (GTK_WINDOW (dlg));
  g_free (type);
  g_free (body);
}

/* Row's edit button. */
static void
on_edit_clicked (GtkButton *btn, gpointer user_data)
{
  SbvDnsEntry *entry = g_object_get_data (G_OBJECT (btn), "sbv-entry");
  if (entry) show_record_dialog (SBV_DNS_PANEL (user_data), entry);
}

/* "Add Record…" button. */
static void
on_add_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  show_record_dialog (SBV_DNS_PANEL (user_data), NULL);
}

/* ── Records list ───────────────────────────────────────────────────────── */

/* Search over name, type and value. */
static gboolean
record_filter (GtkListBoxRow *row, gpointer user_data)
{
  SbvDnsPanel *self  = user_data;
  SbvDnsEntry *entry = g_object_get_data (G_OBJECT (row), "sbv-entry");
  if (!entry) return TRUE;
  char       *type     = sbv_dns_type_name (entry->record->type);
  const char *fields[] = { entry->name, type, entry->record->value };
  gboolean    hit      = sbv_search_matches (self->filter_text, fields, G_N_ELEMENTS (fields));
  g_free (type);
  return hit;
}

/* Re-runs the record filter. */
static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
  SbvDnsPanel *self = SBV_DNS_PANEL (user_data);
  g_free (self->filter_text);
  self->filter_text = sbv_search_fold (gtk_editable_get_text (GTK_EDITABLE (entry)));
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->records_list));
}

/* Builds one record row: name, type, value, TTL and, for editable types,
 * edit and delete buttons. The row owns `entry`. */
static GtkWidget *
record_row (SbvDnsPanel *self, SbvDnsEntry *entry)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  g_object_set_data_full (G_OBJECT (row), "sbv-entry", entry,
                          (GDestroyNotify) sbv_dns_entry_free);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top    (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 8);

  GtkWidget *name = gtk_label_new (entry->name);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_width_chars (GTK_LABEL (name), 18);
  gtk_label_set_max_width_chars (GTK_LABEL (name), 18);
  gtk_widget_add_css_class (name, "heading");
  gtk_box_append (GTK_BOX (box), name);

  char      *type_text = sbv_dns_type_name (entry->record->type);
  GtkWidget *type      = gtk_label_new (type_text);
  gtk_label_set_width_chars (GTK_LABEL (type), 6);
  gtk_label_set_xalign (GTK_LABEL (type), 0);
  gtk_widget_add_css_class (type, "dim-label");
  gtk_box_append (GTK_BOX (box), type);
  g_free (type_text);

  GtkWidget *value = gtk_label_new (entry->record->value);
  gtk_label_set_xalign (GTK_LABEL (value), 0);
  gtk_label_set_ellipsize (GTK_LABEL (value), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable (GTK_LABEL (value), TRUE);
  gtk_widget_set_hexpand (value, TRUE);
  gtk_widget_set_tooltip_text (value, entry->record->value);
  gtk_box_append (GTK_BOX (box), value);

  char      *ttl_text = g_strdup_printf ("TTL %u", entry->record->ttl);
  GtkWidget *ttl      = gtk_label_new (ttl_text);
  gtk_widget_add_css_class (ttl, "dim-label");
  gtk_box_append (GTK_BOX (box), ttl);
  g_free (ttl_text);

  if (sbv_dns_type_editable (entry->record->type)) {
    GtkWidget *edit = gtk_button_new_from_icon_name ("document-edit-symbolic");
    gtk_widget_add_css_class (edit, "flat");
    gtk_widget_set_tooltip_text (edit, "Edit");
    g_object_set_data (G_OBJECT (edit), "sbv-entry", entry);
    g_signal_connect (edit, "clicked", G_CALLBACK (on_edit_clicked), self);
    gtk_box_append (GTK_BOX (box), edit);

    GtkWidget *del = gtk_button_new_from_icon_name ("user-trash-symbolic");
    gtk_widget_add_css_class (del, "flat");
    gtk_widget_set_tooltip_text (del, "Delete");
    g_object_set_data (G_OBJECT (del), "sbv-entry", entry);
    g_signal_connect (del, "clicked", G_CALLBACK (on_delete_clicked), self);
    gtk_box_append (GTK_BOX (box), del);
  } else {
    /* Keep the columns lined up with editable rows. */
    GtkWidget *pad = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request (pad, 76, -1);
    gtk_widget_set_tooltip_text (box, "Sambervise shows this record type read-only");
    gtk_box_append (GTK_BOX (box), pad);
  }

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* Records completion: fill the list. */
static void
on_records_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ReplyCtx    *ctx  = user_data;
  SbvDnsPanel *self = ctx->self;
  GError      *err  = NULL;

  GPtrArray *entries = sbv_dns_records_list_finish (SBV_CONNECTION (source), result, &err);
  if (ctx->gen != self->records_gen) {
    if (entries) g_ptr_array_unref (entries);
    g_clear_error (&err);
    reply_ctx_free (ctx);
    return;
  }

  if (!entries) {
    gtk_label_set_text (GTK_LABEL (self->records_error), err->message);
    gtk_stack_set_visible_child_name (GTK_STACK (self->records_stack), "error");
    g_error_free (err);
    reply_ctx_free (ctx);
    return;
  }

  clear_list (self->records_list);
  g_clear_pointer (&self->sample_a, g_free);
  g_clear_pointer (&self->sample_aaaa, g_free);
  for (guint i = 0; i < entries->len; i++) {
    SbvDnsEntry *e = g_ptr_array_index (entries, i);
    if (!self->sample_a && e->record->type == SBV_DNS_TYPE_A)
      self->sample_a = g_strdup (e->record->value);
    if (!self->sample_aaaa && e->record->type == SBV_DNS_TYPE_AAAA)
      self->sample_aaaa = g_strdup (e->record->value);
  }
  /* Rows take ownership of the entries. */
  g_ptr_array_set_free_func (entries, NULL);
  for (guint i = 0; i < entries->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (self->records_list),
                         record_row (self, g_ptr_array_index (entries, i)));
  g_ptr_array_unref (entries);
  gtk_stack_set_visible_child_name (GTK_STACK (self->records_stack), "list");
  reply_ctx_free (ctx);
}

/* Reloads the selected zone's records. */
static void
load_records (SbvDnsPanel *self)
{
  self->records_gen++;
  if (!self->conn || !self->zone_dn) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->records_stack), "none");
    return;
  }
  gtk_stack_set_visible_child_name (GTK_STACK (self->records_stack), "loading");
  sbv_dns_records_list_async (self->conn, self->zone_dn, NULL, on_records_loaded,
                              reply_ctx_new (self, self->records_gen));
}

/* ── Zones list ─────────────────────────────────────────────────────────── */

/* A zone was picked: show its records. */
static void
on_zone_selected (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
  (void) lb;
  SbvDnsPanel *self = SBV_DNS_PANEL (user_data);
  SbvDnsZone  *zone = row ? g_object_get_data (G_OBJECT (row), "sbv-zone") : NULL;

  g_free (self->zone_dn);
  g_free (self->zone_name);
  self->zone_dn   = zone ? g_strdup (zone->dn)   : NULL;
  g_clear_pointer (&self->sample_a, g_free);
  g_clear_pointer (&self->sample_aaaa, g_free);
  self->zone_name = zone ? g_strdup (zone->name) : NULL;

  gtk_label_set_text (GTK_LABEL (self->zone_title), zone ? zone->name : "");
  gtk_widget_set_sensitive (self->add_btn, zone != NULL);
  load_records (self);
}

/* Zones completion: fill the list and reselect the previous zone. */
static void
on_zones_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ReplyCtx    *ctx  = user_data;
  SbvDnsPanel *self = ctx->self;
  GError      *err  = NULL;

  GPtrArray *zones = sbv_dns_zones_list_finish (SBV_CONNECTION (source), result, &err);
  if (ctx->gen != self->zones_gen) {
    if (zones) g_ptr_array_unref (zones);
    g_clear_error (&err);
    reply_ctx_free (ctx);
    return;
  }

  if (!zones) {
    gtk_label_set_text (GTK_LABEL (self->zone_status), err->message);
    gtk_widget_set_visible (self->zone_status, TRUE);
    g_error_free (err);
    reply_ctx_free (ctx);
    return;
  }

  g_clear_pointer (&self->zones, g_ptr_array_unref);
  self->zones = zones;
  gtk_widget_set_visible (self->zone_status, zones->len == 0);
  if (zones->len == 0)
    gtk_label_set_text (GTK_LABEL (self->zone_status), "No DNS zones found");

  GtkListBoxRow *select = NULL;
  for (guint i = 0; i < zones->len; i++) {
    SbvDnsZone *z   = g_ptr_array_index (zones, i);
    GtkWidget  *row = gtk_list_box_row_new ();
    g_object_set_data (G_OBJECT (row), "sbv-zone", z);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top    (box, 6);
    gtk_widget_set_margin_bottom (box, 6);
    gtk_widget_set_margin_start  (box, 12);
    gtk_widget_set_margin_end    (box, 12);
    GtkWidget *name = gtk_label_new (z->name);
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append (GTK_BOX (box), name);
    GtkWidget *part = gtk_label_new (z->partition);
    gtk_label_set_xalign (GTK_LABEL (part), 0);
    gtk_widget_add_css_class (part, "dim-label");
    gtk_widget_add_css_class (part, "caption");
    gtk_box_append (GTK_BOX (box), part);
    gtk_widget_set_tooltip_text (row, z->dn);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_append (GTK_LIST_BOX (self->zone_list), row);

    if (self->reselect_zone && g_ascii_strcasecmp (self->reselect_zone, z->name) == 0)
      select = GTK_LIST_BOX_ROW (row);
  }
  if (!select && zones->len > 0)
    select = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->zone_list), 0);
  if (select) gtk_list_box_select_row (GTK_LIST_BOX (self->zone_list), select);
  reply_ctx_free (ctx);
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

/* Builds the zone list on the left. */
static GtkWidget *
build_zones (SbvDnsPanel *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top    (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start  (box, 12);
  gtk_widget_set_margin_end    (box, 6);

  GtkWidget *title = gtk_label_new ("Zones");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_add_css_class (title, "heading");
  gtk_box_append (GTK_BOX (box), title);

  self->zone_status = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->zone_status), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->zone_status), 0);
  gtk_widget_add_css_class (self->zone_status, "dim-label");
  gtk_widget_set_visible (self->zone_status, FALSE);
  gtk_box_append (GTK_BOX (box), self->zone_status);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_set_size_request (scroll, 220, -1);
  self->zone_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->zone_list), GTK_SELECTION_BROWSE);
  gtk_widget_add_css_class (self->zone_list, "boxed-list");
  g_signal_connect (self->zone_list, "row-selected", G_CALLBACK (on_zone_selected), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), self->zone_list);
  gtk_box_append (GTK_BOX (box), scroll);
  return box;
}

/* Builds the records side: title, search, Add, and the list. */
static GtkWidget *
build_records (SbvDnsPanel *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top    (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start  (box, 6);
  gtk_widget_set_margin_end    (box, 12);

  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->zone_title = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->zone_title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->zone_title), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class (self->zone_title, "title-4");
  gtk_widget_set_hexpand (self->zone_title, TRUE);
  gtk_box_append (GTK_BOX (bar), self->zone_title);

  self->search_entry = gtk_search_entry_new ();
  gtk_widget_set_tooltip_text (self->search_entry, "Search names, types and values");
  g_signal_connect (self->search_entry, "search-changed", G_CALLBACK (on_search_changed), self);
  gtk_box_append (GTK_BOX (bar), self->search_entry);

  self->add_btn = gtk_button_new_with_label ("Add Record…");
  gtk_widget_add_css_class (self->add_btn, "suggested-action");
  gtk_widget_set_sensitive (self->add_btn, FALSE);
  g_signal_connect (self->add_btn, "clicked", G_CALLBACK (on_add_clicked), self);
  gtk_box_append (GTK_BOX (bar), self->add_btn);
  gtk_box_append (GTK_BOX (box), bar);

  self->records_stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->records_stack, TRUE);

  GtkWidget *none = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (none), "network-server-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (none), "No Zone Selected");
  gtk_stack_add_named (GTK_STACK (self->records_stack), none, "none");

  GtkWidget *spin = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (spin));
  gtk_widget_set_halign (spin, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spin, GTK_ALIGN_CENTER);
  gtk_stack_add_named (GTK_STACK (self->records_stack), spin, "loading");

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  self->records_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->records_list), GTK_SELECTION_NONE);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (self->records_list), record_filter, self, NULL);
  gtk_widget_add_css_class (self->records_list, "boxed-list");
  GtkWidget *empty = gtk_label_new ("No records match");
  gtk_widget_add_css_class (empty, "dim-label");
  gtk_widget_set_margin_top (empty, 12);
  gtk_widget_set_margin_bottom (empty, 12);
  gtk_list_box_set_placeholder (GTK_LIST_BOX (self->records_list), empty);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), self->records_list);
  gtk_stack_add_named (GTK_STACK (self->records_stack), scroll, "list");

  self->records_error = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->records_error), TRUE);
  gtk_widget_add_css_class (self->records_error, "error");
  gtk_stack_add_named (GTK_STACK (self->records_stack), self->records_error, "error");

  gtk_stack_set_visible_child_name (GTK_STACK (self->records_stack), "none");
  gtk_box_append (GTK_BOX (box), self->records_stack);
  return box;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

GtkWidget *
sbv_dns_panel_new (void)
{
  return g_object_new (SBV_TYPE_DNS_PANEL, NULL);
}

void
sbv_dns_panel_load (SbvDnsPanel *self, SbvConnection *conn)
{
  g_return_if_fail (SBV_IS_DNS_PANEL (self));
  gboolean same_conn = (conn == self->conn);
  g_set_object (&self->conn, conn);

  /* Keep the zone across a refresh of the same connection. */
  g_free (self->reselect_zone);
  self->reselect_zone = same_conn ? g_strdup (self->zone_name) : NULL;

  self->zones_gen++;
  self->records_gen++;
  clear_list (self->zone_list);   /* clears the selection, which clears zone_dn */
  clear_list (self->records_list);
  g_clear_pointer (&self->zones, g_ptr_array_unref);
  gtk_stack_set_visible_child_name (GTK_STACK (self->records_stack), "none");
  if (!conn) return;

  gtk_label_set_text (GTK_LABEL (self->zone_status), "Loading…");
  gtk_widget_set_visible (self->zone_status, TRUE);
  sbv_dns_zones_list_async (conn, NULL, on_zones_loaded,
                            reply_ctx_new (self, self->zones_gen));
}

/* ── GObject ────────────────────────────────────────────────────────────── */

static void
sbv_dns_panel_finalize (GObject *object)
{
  SbvDnsPanel *self = SBV_DNS_PANEL (object);
  g_clear_object (&self->conn);
  g_clear_pointer (&self->zones, g_ptr_array_unref);
  g_free (self->zone_dn);
  g_free (self->zone_name);
  g_free (self->reselect_zone);
  g_free (self->filter_text);
  g_free (self->sample_a);
  g_free (self->sample_aaaa);
  G_OBJECT_CLASS (sbv_dns_panel_parent_class)->finalize (object);
}

static void
sbv_dns_panel_class_init (SbvDnsPanelClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_dns_panel_finalize;
}

/* Builds the two-pane layout: zones on the left, records on the right. */
static void
sbv_dns_panel_init (SbvDnsPanel *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_vexpand (paned, TRUE);
  gtk_paned_set_start_child (GTK_PANED (paned), build_zones (self));
  gtk_paned_set_end_child (GTK_PANED (paned), build_records (self));
  gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_resize_start_child (GTK_PANED (paned), FALSE);
  gtk_box_append (GTK_BOX (self), paned);
}
