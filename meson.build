project(
  'flatpak-xdg-utils', 'c',
  version: '1.0.5',
  license: 'LGPL-2.1+',
  default_options: [
    'buildtype=debugoptimized',
    'c_std=gnu99',
    'warning_level=2',
  ],
  meson_version: '>= 0.46.0',
)

bindir = join_paths(get_option('prefix'), get_option('bindir'))
installed_tests_metadir = join_paths(get_option('prefix'),
  get_option('datadir'),
  'installed-tests',
  meson.project_name())
installed_tests_execdir = join_paths(get_option('prefix'),
  get_option('libexecdir'),
  'installed-tests',
  meson.project_name())
installed_tests_enabled = get_option('installed_tests')

conf = configuration_data()
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('BINDIR', bindir)
conf.set('_GNU_SOURCE', 1)
config_h = configure_file(output: 'config.h', configuration: conf)

gio_unix = dependency('gio-unix-2.0')
threads = dependency('threads')

srcinc = include_directories('src')

subdir('src')
subdir('tests')
