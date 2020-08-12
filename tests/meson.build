test_env = environment()
test_env.set('G_DEBUG', 'gc-friendly,fatal-criticals')
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
test_env.set('GIO_USE_VFS', 'local')
test_env.set('XDG_EMAIL', xdg_email.full_path())
test_env.set('XDG_OPEN', xdg_open.full_path())
test_env.set('MALLOC_CHECK_', '2')

tests = [
  'test-email',
  'test-open',
]

test_timeout = 60

installed_tests_template_tap = files('template-tap.test.in')

foreach test_name : tests
  template = installed_tests_template_tap

  if installed_tests_enabled
    test_conf = configuration_data()
    test_conf.set('installed_tests_dir', installed_tests_execdir)
    test_conf.set('program', test_name)
    configure_file(
      input: template,
      output: test_name + '.test',
      install_dir: installed_tests_metadir,
      configuration: test_conf
    )
  endif

  exe = executable(test_name, [test_name + '.c', 'common.c', 'common.h'],
    c_args: ['-include', '@0@'.format(config_h)],
    dependencies: [gio_unix],
    include_directories : [srcinc],
    install_dir: installed_tests_execdir,
    install: installed_tests_enabled,
  )

  test(test_name, exe, env : test_env, timeout : test_timeout,
    suite : ['flatpak-xdg-utils'], args : ['--tap'])
endforeach
