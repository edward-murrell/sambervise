#include "sbv-app.h"
#include "ui/sbv-window.h"
#include "backend/sbv-profiles.h"

struct _SbvApp {
  AdwApplication  parent;
  GListStore     *profiles_store;
};

G_DEFINE_TYPE (SbvApp, sbv_app, ADW_TYPE_APPLICATION)

static void
sbv_app_activate (GApplication *app)
{
  SbvApp *self = SBV_APP (app);

  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!win) {
    win = GTK_WINDOW (sbv_window_new (GTK_APPLICATION (app),
                                       self->profiles_store));
    gtk_window_present (win);
  } else {
    gtk_window_present (win);
  }
}

static void
sbv_app_startup (GApplication *app)
{
  G_APPLICATION_CLASS (sbv_app_parent_class)->startup (app);
  adw_init ();

  /* Register our bundled symbolic icons so they render even when the
   * user's icon theme (e.g. Mint-X) doesn't inherit from Adwaita and is
   * missing names like document-edit-symbolic / view-list-bullet-symbolic. */
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                     "/org/ekm/sambervise/icons");

  SbvApp *self = SBV_APP (app);
  GError *err  = NULL;
  self->profiles_store = sbv_profiles_load (&err);
  if (err) {
    g_warning ("Could not load connection profiles: %s", err->message);
    g_error_free (err);
  }
}

static void
sbv_app_finalize (GObject *object)
{
  g_clear_object (&SBV_APP (object)->profiles_store);
  G_OBJECT_CLASS (sbv_app_parent_class)->finalize (object);
}

static void
sbv_app_class_init (SbvAppClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize      = sbv_app_finalize;
  G_APPLICATION_CLASS (klass)->activate = sbv_app_activate;
  G_APPLICATION_CLASS (klass)->startup  = sbv_app_startup;
}

static void
sbv_app_init (SbvApp *self)
{
  (void) self;
}

SbvApp *
sbv_app_new (void)
{
  return g_object_new (SBV_TYPE_APP,
                        "application-id", "org.ekm.sambervise",
                        "flags", G_APPLICATION_DEFAULT_FLAGS,
                        NULL);
}
