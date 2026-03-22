#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SBV_TYPE_APP (sbv_app_get_type ())
G_DECLARE_FINAL_TYPE (SbvApp, sbv_app, SBV, APP, AdwApplication)

SbvApp *sbv_app_new (void);

G_END_DECLS
