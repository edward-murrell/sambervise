install_data('org.ekm.sambervise.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

gnome.compile_schemas(
  build_by_default: true,
  depend_files: 'org.ekm.sambervise.gschema.xml',
)

install_data('org.ekm.sambervise.desktop',
  install_dir: get_option('datadir') / 'applications',
)
