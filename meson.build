project(
  'flatpak-xdg-utils', 'c',
  version: '1.0.0',
  license: 'LGPL-2.1+',
  default_options: [
    'buildtype=debugoptimized',
    'warning_level=2',
  ],
  meson_version: '>= 0.46.0',
)

conf = configuration_data()
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set('_GNU_SOURCE', 1)
config_h = configure_file(output: 'config.h', configuration: conf)

gio_unix = dependency('gio-unix-2.0')

subdir('src')
