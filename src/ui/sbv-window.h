#pragma once

#include <adwaita.h>
#include "../backend/sbv-connection.h"
#include "../backend/sbv-profiles.h"

G_BEGIN_DECLS

#define SBV_TYPE_WINDOW (sbv_window_get_type ())
G_DECLARE_FINAL_TYPE (SbvWindow, sbv_window, SBV, WINDOW, AdwApplicationWindow)

GtkWidget *sbv_window_new          (GtkApplication *app, GListStore *profiles_store);
void       sbv_window_show_toast   (SbvWindow *self, const char *message);
void       sbv_window_on_connected (SbvWindow *self, SbvConnection *conn,
                                     SbvProfile *profile);

G_END_DECLS
