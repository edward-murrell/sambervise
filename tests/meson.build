test_deps = [gtk_dep, adw_dep, ldap_dep, lber_dep]

test_sources_common = [
  '../src/sbv-error.c',
  '../src/model/sbv-user.c',
  '../src/model/sbv-group.c',
]

foreach t : ['users', 'groups']
  exe = executable('test-' + t,
    'test-' + t + '.c',
    test_sources_common,
    dependencies: test_deps,
    include_directories: include_directories('../src'),
  )
  test(t, exe)
endforeach
