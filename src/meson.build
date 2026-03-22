sambervise_resources = gnome.compile_resources(
  'sambervise-resources',
  meson.project_source_root() / 'data' / 'sambervise.gresource.xml',
  source_dir: meson.project_source_root() / 'data',
  c_name: 'sambervise',
)

sambervise_sources = [
  'main.c',
  'sbv-app.c',
  'sbv-error.c',
  'model/sbv-user.c',
  'model/sbv-group.c',
  'model/sbv-profile.c',
  'backend/sbv-connection.c',
  'backend/sbv-dns.c',
  'backend/sbv-profiles.c',
  'backend/sbv-users-backend.c',
  'backend/sbv-groups-backend.c',
  'ui/sbv-window.c',
  'ui/sbv-connect-dialog.c',
  'ui/sbv-password-dialog.c',
  'ui/sbv-users-panel.c',
  'ui/sbv-groups-panel.c',
]

executable('sambervise',
  sambervise_sources,
  sambervise_resources,
  dependencies: [gtk_dep, adw_dep, ldap_dep, lber_dep],
  include_directories: include_directories('.'),
  install: true,
)
