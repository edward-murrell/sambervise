#include "sbv-create-user-dialog.h"
#include "../backend/sbv-users-backend.h"
#include "../backend/sbv-bulk.h"
#include "../backend/sbv-dn.h"

#include <adwaita.h>
#include <string.h>

/* Per-dialog state. Reference counted with GRcBox: the window holds one
 * reference and each LDAP call in flight holds another. `window` is NULL
 * once the window is gone, so late completions don't touch widgets. */
typedef struct {
  GtkWindow            *window;
  SbvUser              *template_user;  /* owned; NULL unless copying */
  char                 *new_sam;        /* kept for the template step */
  char                 *new_dn;         /* set once the user exists */
  gboolean              finished;       /* Create has become Close */
  SbvConnection        *conn;
  SbvCreateUserCallback callback;
  gpointer              user_data;

  GtkWidget *sam_entry;
  GtkWidget *cn_entry;
  GtkWidget *given_entry;
  GtkWidget *sn_entry;
  GtkWidget *container_entry;

  GtkWidget *create_btn;
  GtkWidget *spinner;
  GtkWidget *error_label;

  /* Auto-fill state for cn:
   *   syncing   — true while we're programmatically updating cn so the
   *               cn-changed handler doesn't mistake it for a user edit.
   *   cn_manual — true once the user has typed into cn directly; while set
   *               we stop auto-filling. Reset to false if the user clears
   *               cn, so auto-fill resumes. */
  gboolean syncing;
  gboolean cn_manual;
} CreateCtx;

/* Frees the context's contents when the last reference drops. */
static void
ctx_clear (gpointer data)
{
  CreateCtx *ctx = data;
  g_clear_object (&ctx->template_user);
  g_free (ctx->new_sam);
  g_free (ctx->new_dn);
}

/* Drops one reference to the context. */
static void
ctx_release (CreateCtx *ctx)
{
  g_rc_box_release_full (ctx, ctx_clear);
}

/* Window "destroy": marks the widgets gone and drops the window's ref. */
static void
on_window_destroy (GtkWidget *win, gpointer user_data)
{
  (void) win;
  CreateCtx *ctx = user_data;
  ctx->window = NULL;
  ctx_release (ctx);
}

/* Shows an error under the form. */
static void
show_error (CreateCtx *ctx, const char *message)
{
  gtk_label_set_text (GTK_LABEL (ctx->error_label), message);
  gtk_widget_set_visible (ctx->error_label, TRUE);
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

/* Template step completion: tell the caller about the new user. Close on
 * full success. Otherwise list what didn't copy and turn Create into
 * Close, since the user exists either way. */
static void
on_template_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CreateCtx     *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  SbvBulkResult *r = sbv_bulk_apply_user_template_finish (conn, result, &err);

  /* Tell the caller after the groups are in place, so its reloads show
   * them. The user exists even if some settings failed. */
  if (ctx->callback) ctx->callback (conn, ctx->new_dn, ctx->user_data);

  if (ctx->window) {
    set_busy (ctx, FALSE);
    if (r && r->failures->len == 0) {
      gtk_window_destroy (ctx->window);
    } else {
      GString *msg = g_string_new ("The user was created, but some settings didn't copy:\n");
      if (!r) {
        g_string_append (msg, err->message);
      } else {
        for (guint i = 0; i < r->failures->len; i++) {
          SbvBulkFailure *f    = g_ptr_array_index (r->failures, i);
          char           *name = sbv_dn_rdn_value (f->dn);
          g_string_append_printf (msg, "%s: %s\n", name ? name : f->dn, f->message);
          g_free (name);
        }
      }
      show_error (ctx, g_strchomp (msg->str));
      g_string_free (msg, TRUE);
      ctx->finished = TRUE;
      gtk_button_set_label (GTK_BUTTON (ctx->create_btn), "Close");
    }
  }

  g_clear_error (&err);
  if (r) sbv_bulk_result_free (r);
  ctx_release (ctx);
}

/* Async-completion callback: surfaces errors inline. On success it either
 * copies the template's settings or closes, and tells the parent so it
 * can refresh and select the new row. */
static void
on_create_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CreateCtx     *ctx  = user_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  GError        *err  = NULL;

  char *new_dn = sbv_users_create_finish (conn, result, &err);

  if (!new_dn) {
    if (ctx->window) {
      set_busy (ctx, FALSE);
      show_error (ctx, err->message);
    }
    g_error_free (err);
    ctx_release (ctx);
    return;
  }

  if (ctx->template_user) {
    ctx->new_dn = new_dn;   /* takes ownership */
    sbv_bulk_apply_user_template_async (conn, new_dn, ctx->new_sam,
                                        ctx->template_user, NULL,
                                        on_template_done, g_rc_box_acquire (ctx));
    ctx_release (ctx);
    return;
  }

  if (ctx->window) set_busy (ctx, FALSE);
  if (ctx->callback)
    ctx->callback (conn, new_dn, ctx->user_data);
  g_free (new_dn);

  if (ctx->window) gtk_window_destroy (ctx->window);
  ctx_release (ctx);
}

/* Validates the form, then dispatches the create call. Inline validation
 * keeps the dialog usable without round-tripping to the DC for obvious
 * mistakes. */
static void
on_create_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  CreateCtx *ctx = user_data;

  if (ctx->finished) {
    gtk_window_destroy (ctx->window);
    return;
  }

  const char *sam       = gtk_editable_get_text (GTK_EDITABLE (ctx->sam_entry));
  const char *cn        = gtk_editable_get_text (GTK_EDITABLE (ctx->cn_entry));
  const char *given     = gtk_editable_get_text (GTK_EDITABLE (ctx->given_entry));
  const char *sn        = gtk_editable_get_text (GTK_EDITABLE (ctx->sn_entry));
  const char *container = gtk_editable_get_text (GTK_EDITABLE (ctx->container_entry));

  gtk_widget_set_visible (ctx->error_label, FALSE);

  if (!*sam || !*cn || !*container) {
    gtk_label_set_text (GTK_LABEL (ctx->error_label),
                        "Username, full name and container are required.");
    gtk_widget_set_visible (ctx->error_label, TRUE);
    return;
  }

  g_free (ctx->new_sam);
  ctx->new_sam = g_strdup (sam);
  set_busy (ctx, TRUE);
  sbv_users_create_async (ctx->conn, sam, cn, given, sn, container,
                           NULL, on_create_done, g_rc_box_acquire (ctx));
}

/* Auto-fill cn from "given sn" as the user types — a small convenience
 * matching how AD admins typically build display names. Stops auto-filling
 * once the user has manually edited the cn field (see on_cn_changed). */
static void
on_name_part_changed (GtkEditable *editable, gpointer user_data)
{
  (void) editable;
  CreateCtx *ctx = user_data;
  if (ctx->cn_manual) return;

  const char *given = gtk_editable_get_text (GTK_EDITABLE (ctx->given_entry));
  const char *sn    = gtk_editable_get_text (GTK_EDITABLE (ctx->sn_entry));

  char *combined = g_strdup_printf ("%s%s%s", given,
                                     (*given && *sn) ? " " : "", sn);

  ctx->syncing = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (ctx->cn_entry), combined);
  ctx->syncing = FALSE;

  g_free (combined);
}

/* Tracks whether cn has been edited by the user. Programmatic updates done
 * via on_name_part_changed set ctx->syncing first so we ignore those. If
 * the user clears cn entirely, treat that as "give me the auto-fill back"
 * and resume syncing. */
static void
on_cn_changed (GtkEditable *editable, gpointer user_data)
{
  CreateCtx *ctx = user_data;
  if (ctx->syncing) return;
  const char *text = gtk_editable_get_text (editable);
  ctx->cn_manual = (*text != '\0');
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

/* Describes what a copy brings across from the template. Caller frees. */
static char *
copy_summary (SbvUser *t)
{
  const char *sam  = sbv_user_get_sam (t);
  const char *name = sbv_user_get_display_name (t);
  if (!name || !*name) name = sbv_user_get_cn (t);
  if (!name || !*name) name = sam;

  const char * const *groups = sbv_user_get_member_of (t);
  guint ngroups = groups ? g_strv_length ((char **) groups) : 0;

  return g_strdup_printf (
    "Copies from %s: the container, %u group%s, the Windows profile paths "
    "and account options. Paths that contain \"%s\" or %%username%% get "
    "the new username. The new account starts disabled with no password.",
    name ? name : "the template", ngroups, ngroups == 1 ? "" : "s",
    sam ? sam : "");
}

/* Builds and presents the dialog. A non-NULL `template_user` selects copy
 * mode. */
static void
show_dialog (GtkWindow            *parent,
             SbvConnection        *conn,
             SbvUser              *template_user,
             SbvCreateUserCallback callback,
             gpointer              user_data)
{
  if (!conn) return;

  CreateCtx *ctx = g_rc_box_new0 (CreateCtx);
  ctx->conn      = conn;
  ctx->callback  = callback;
  ctx->user_data = user_data;
  ctx->template_user = template_user ? g_object_ref (template_user) : NULL;

  GtkWidget *win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (win), template_user ? "Copy User" : "Create User");
  gtk_window_set_modal (GTK_WINDOW (win), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (win), 460, 360);
  if (parent)
    gtk_window_set_transient_for (GTK_WINDOW (win), parent);
  ctx->window = GTK_WINDOW (win);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* Header bar with Cancel + Create */
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
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->sam_entry), "jsmith");
  gtk_widget_add_css_class (ctx->sam_entry, "monospace");
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Username (sam)", ctx->sam_entry));

  ctx->given_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->given_entry), "First name");
  g_signal_connect (ctx->given_entry, "changed",
                    G_CALLBACK (on_name_part_changed), ctx);
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("First Name", ctx->given_entry));

  ctx->sn_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->sn_entry), "Last name");
  g_signal_connect (ctx->sn_entry, "changed",
                    G_CALLBACK (on_name_part_changed), ctx);
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Last Name", ctx->sn_entry));

  ctx->cn_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->cn_entry),
                                   "Used as the CN/RDN");
  g_signal_connect (ctx->cn_entry, "changed",
                    G_CALLBACK (on_cn_changed), ctx);
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Full Name (cn)", ctx->cn_entry));

  ctx->container_entry = gtk_entry_new ();
  gtk_widget_add_css_class (ctx->container_entry, "monospace");
  /* Default container: the template's, or CN=Users,<base-dn> */
  char *template_parent = NULL;
  const char *base = sbv_connection_get_base_dn (conn);
  if (template_user
      && sbv_dn_split (sbv_user_get_dn (template_user), NULL, &template_parent)) {
    gtk_editable_set_text (GTK_EDITABLE (ctx->container_entry), template_parent);
    g_free (template_parent);
  } else if (base) {
    char *def = g_strdup_printf ("CN=Users,%s", base);
    gtk_editable_set_text (GTK_EDITABLE (ctx->container_entry), def);
    g_free (def);
  }
  gtk_box_append (GTK_BOX (form),
                   make_field_row ("Container DN", ctx->container_entry));

  /* Hint about disabled-on-create, or what a copy brings across */
  char *hint_text = template_user ? copy_summary (template_user) : g_strdup (
    "Created accounts are disabled with no password. "
    "Set a password and enable from the user's detail page after creation.");
  GtkWidget *hint = gtk_label_new (hint_text);
  g_free (hint_text);
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_widget_set_margin_top (hint, 6);
  gtk_widget_set_margin_start (hint, 148);
  gtk_box_append (GTK_BOX (form), hint);

  /* Error label (hidden until needed) */
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
  g_signal_connect (win, "destroy", G_CALLBACK (on_window_destroy), ctx);
  gtk_window_present (GTK_WINDOW (win));

  gtk_widget_grab_focus (ctx->sam_entry);
}

/* Public entry point — see header. */
void
sbv_create_user_dialog_show (GtkWindow            *parent,
                              SbvConnection        *conn,
                              SbvCreateUserCallback callback,
                              gpointer              user_data)
{
  show_dialog (parent, conn, NULL, callback, user_data);
}

/* Public entry point — see header. */
void
sbv_create_user_dialog_show_copy (GtkWindow            *parent,
                                   SbvConnection        *conn,
                                   SbvUser              *template_user,
                                   SbvCreateUserCallback callback,
                                   gpointer              user_data)
{
  g_return_if_fail (template_user != NULL);
  show_dialog (parent, conn, template_user, callback, user_data);
}
