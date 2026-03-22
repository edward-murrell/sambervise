#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SBV_ERROR (sbv_error_quark ())

GQuark sbv_error_quark (void);

typedef enum {
  SBV_ERROR_CONNECTION,
  SBV_ERROR_AUTH,
  SBV_ERROR_TLS,
  SBV_ERROR_LDAP,
} SbvError;

G_END_DECLS
