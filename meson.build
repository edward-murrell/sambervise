project('sambervise', 'c',
  version: '0.1.0',
  meson_version: '>= 0.60',
  default_options: [
    'warning_level=2',
    'c_std=c11',
  ],
)

gnome = import('gnome')

gtk_dep  = dependency('gtk4')
adw_dep  = dependency('libadwaita-1')
cc       = meson.get_compiler('c')
ldap_dep = cc.find_library('ldap', required: true)
lber_dep = cc.find_library('lber', required: true)

subdir('data')
subdir('src')
subdir('tests')
