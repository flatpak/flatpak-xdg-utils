flatpak_spawn = executable(
  'flatpak-spawn',
  sources: 'flatpak-spawn.c',
  dependencies: [gio_unix, threads],
  c_args: ['-include', '@0@'.format(config_h)],
  install: true,
)

xdg_email = executable(
  'xdg-email',
  sources: 'xdg-email.c',
  dependencies: [gio_unix],
  c_args: ['-include', '@0@'.format(config_h)],
  install: true,
)

xdg_open = executable(
  'xdg-open',
  sources: 'xdg-open.c',
  dependencies: [gio_unix],
  c_args: ['-include', '@0@'.format(config_h)],
  install: true,
)
