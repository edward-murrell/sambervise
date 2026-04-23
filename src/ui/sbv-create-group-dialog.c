#include "sbv-create-group-dialog.h"
#include "../backend/sbv-groups-backend.h"

#include <adwaita.h>
#include <string.h>

/* Per-dialog state. Lives until the dialog window is destroyed. */
typedef struct {
  GtkWindow             *window;
  SbvConnection         *conn;
  SbvCreateGroupCallback callback;
  gpointer               user_data;

  GtkWidget *sam_entry;
  GtkWidget *desc_entry;
  GtkWidget *container_entry;
  GtkWidget *scope_drop;     /* Global / Domain Local / Universal */
  GtkWidget *type_drop;      /* Security / Distribution           */

  GtkWidget *create_btn;
  GtkWidget *spinner;
  GtkWidget *error_label;
} CreateCtx;

/* Frees the per-dialog context. */
static void
ctx_free (CreateCtx *ctx)
{
  g_free (ctx);
}

/* Toggles the action area's busy state during the LDAP add round-trip. */
static void
set_busy (CreateCtx *ctx, gboolean busy)
{
  gtk_widget_set_sensitive (ctx->create_btn, !busy);
  gtk_widget_set_visible   (ctx->spinner,    busy);
  if (busy) gtk_spinner_start (GTK_SPINNER (ctx->spinner));
  else      gtk_spinner_stop  (GTK_SPINNER (ctx->spinner));
}

/* Maps the scope dropdown's selected index to the AD groupType scope bit. */
static gint32
scope_index_to_bit (guint idx)
{
  switch (idx) {
    case 0: return SBV_GT_GLOBAL;
    case 1: return SBV_GT_DOMAIN_LOCAL;
    case 2: return SBV_GT_UNIVERSAL;
    default: return SBV_GT_GLOBAL;
  }
}

/* Async-completion callback for the create operation. Errors render
 * inline; success closes the dialog and notifies the parent. */
static void
on_create_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CreateCtx     *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  char *new_dn = sbv_groups_create_finish (conn, result, &err);

  set_busy (ctx, FALSE);

  if (!new_dn) {
    gtk_label_set_text (GTK_LABEL (ctx->error_label), err->message);
    gtk_widget_set_visible (ctx->error_label, TRUE);
    g_error_free (err);
    return;
  }

  if (ctx->callback)
    ctx->callback (conn, new_dn, ctx->user_data);
  g_free (new_dn);

  gtk_window_destroy (ctx->window);
}

/* Validates the form and dispatches the create call. */
static void
on_create_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  CreateCtx *ctx = user_data;

  const char *sam       = gtk_editable_get_text (GTK_EDITABLE (ctx->sam_entry));
  const char *desc      = gtk_editable_get_text (GTK_EDITABLE (ctx->desc_entry));
  const char *container = gtk_editable_get_text (GTK_EDITABLE (ctx->container_entry));

  gtk_widget_set_visible (ctx->error_label, FALSE);

  if (!*sam || !*container) {
    gtk_label_set_text (GTK_LABEL (ctx->error_label),
                        "Group name and container are required.");
    gtk_widget_set_visible (ctx->error_label, TRUE);
    return;
  }

  guint  scope_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (ctx->scope_drop));
  guint  type_idx  = gtk_drop_down_get_selected (GTK_DROP_DOWN (ctx->type_drop));
  gint32 group_type = scope_index_to_bit (scope_idx);
  if (type_idx == 0) /* Security */
    group_type |= SBV_GT_SECURITY;

  set_busy (ctx, TRUE);
  sbv_groups_create_async (ctx->conn, sam, desc, group_type, container,
                            NULL, on_create_done, ctx);
}

/* Builds a labelled form row with the project's standard 140px label width. */
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

/* Public entry point — see header. */
void
sbv_create_group_dialog_show (GtkWindow             *parent,
                               SbvConnection         *conn,
                               SbvCreateGroupCallback callback,
                               gpointer               user_data)
{
  if (!conn) return;

  CreateCtx *ctx = g_new0 (CreateCtx, 1);
  ctx->conn      = conn;
  ctx->callback  = callback;
  ctx->user_data = user_data;

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), "Create Group");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 460, 320);
  if (parent) gtk_window_set_transient_for (GTK_WINDOW (win), parent);
  ctx->window = GTK_WINDOW (win);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* Header bar */
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);
  g_signal_connect_swapped (cancel_btn, "clicked",
                             G_CALLBACK (gtk_window_destroy), win);

  ctx->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (ctx->spinner, FALSE);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->spinner);

  ctx->create_btn = gtk_button_new_with_label ("Create");
  gtk_widget_add_css_class (ctx->create_btn, "suggested-action");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->create_btn);
  g_signal_connect (ctx->create_btn, "clicked",
                    G_CALLBACK (on_create_clicked), ctx);

  gtk_box_append (GTK_BOX (vbox), header);

  /* Form */
  GtkWidget *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top    (form, 16);
  gtk_widget_set_margin_bottom (form, 16);
  gtk_widget_set_margin_start  (form, 20);
  gtk_widget_set_margin_end    (form, 20);

  ctx->sam_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->sam_entry), "Marketing");
  gtk_widget_add_css_class (ctx->sam_entry, "monospace");
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Group Name (sam)", ctx->sam_entry));

  ctx->desc_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->desc_entry),
                                   "Optional description");
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Description", ctx->desc_entry));

  /* Scope dropdown */
  {
    const char * const scopes[] = {
      "Global", "Domain Local", "Universal", NULL
    };
    ctx->scope_drop = gtk_drop_down_new_from_strings (scopes);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (ctx->scope_drop), 0);
    gtk_box_append (GTK_BOX (form), make_field_row ("Scope", ctx->scope_drop));
  }

  /* Type dropdown */
  {
    const char * const types[] = { "Security", "Distribution", NULL };
    ctx->type_drop = gtk_drop_down_new_from_strings (types);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (ctx->type_drop), 0);
    gtk_box_append (GTK_BOX (form), make_field_row ("Type", ctx->type_drop));
  }

  ctx->container_entry = gtk_entry_new ();
  gtk_widget_add_css_class (ctx->container_entry, "monospace");
  /* Default container: CN=Users,<base-dn> (matches AD's default for new
   * groups created via standard tooling). */
  const char *base = sbv_connection_get_base_dn (conn);
  if (base) {
    char *def = g_strdup_printf ("CN=Users,%s", base);
    gtk_editable_set_text (GTK_EDITABLE (ctx->container_entry), def);
    g_free (def);
  }
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Container DN", ctx->container_entry));

  ctx->error_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (ctx->error_label), 0);
  gtk_label_set_wrap (GTK_LABEL (ctx->error_label), TRUE);
  gtk_widget_add_css_class (ctx->error_label, "error");
  gtk_widget_set_margin_top (ctx->error_label, 6);
  gtk_widget_set_margin_start (ctx->error_label, 148);
  gtk_widget_set_visible (ctx->error_label, FALSE);
  gtk_box_append (GTK_BOX (form), ctx->error_label);

  gtk_box_append (GTK_BOX (vbox), form);

  gtk_window_set_child (GTK_WINDOW (win), vbox);
  g_signal_connect_swapped (win, "destroy", G_CALLBACK (ctx_free), ctx);
  gtk_window_present (GTK_WINDOW (win));

  gtk_widget_grab_focus (ctx->sam_entry);
}
