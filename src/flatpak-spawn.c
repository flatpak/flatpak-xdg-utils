/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "backport-autoptr.h"

typedef enum {
  FLATPAK_SPAWN_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_SPAWN_FLAGS_LATEST_VERSION = 1 << 1,
  FLATPAK_SPAWN_FLAGS_SANDBOX = 1 << 2,
  FLATPAK_SPAWN_FLAGS_NO_NETWORK = 1 << 3,
  FLATPAK_SPAWN_FLAGS_WATCH_BUS = 1 << 4, /* Since 1.2 */
  FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS = 1 << 5, /* Since 1.6, optional */
} FlatpakSpawnFlags;

typedef enum {
  FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS = 1 << 1, /* Since 1.2 */
} FlatpakHostCommandFlags;

/* Since 1.6 */
typedef enum {
  FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_DISPLAY = 1 << 0,
  FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_SOUND = 1 << 1,
  FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_GPU = 1 << 2,
  FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS = 1 << 3,
  FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_A11Y = 1 << 4,
} FlatpakSpawnSandboxFlags;

/* Since 1.6 */
typedef enum {
  FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS = 1 << 0,
} FlatpakSpawnSupportFlags;

static GDBusConnection *session_bus = NULL;

guint child_pid = 0;
gboolean opt_host;

const char *service_iface;
const char *service_obj_path;
const char *service_bus_name;

static void
spawn_exited_cb (G_GNUC_UNUSED GDBusConnection *connection,
                 G_GNUC_UNUSED const gchar     *sender_name,
                 G_GNUC_UNUSED const gchar     *object_path,
                 G_GNUC_UNUSED const gchar     *interface_name,
                 G_GNUC_UNUSED const gchar     *signal_name,
                 GVariant                      *parameters,
                 G_GNUC_UNUSED gpointer         user_data)
{
  guint32 client_pid = 0;
  guint32 exit_status = 0;

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(uu)")))
    return;

  g_variant_get (parameters, "(uu)", &client_pid, &exit_status);
  g_debug ("child exited %d: %d", client_pid, exit_status);

  if (child_pid == client_pid)
    {
      int exit_code = 0;

      if (WIFEXITED (exit_status))
        {
          exit_code = WEXITSTATUS (exit_status);
        }
      else if (WIFSIGNALED (exit_status))
        {
          /* Smush the signal into an unsigned byte, as the shell does. This is
           * not quite right from the perspective of whatever ran flatpak-spawn
           * — it will get WIFEXITED() not WIFSIGNALED() — but the
           *  alternative is to disconnect all signal() handlers then send this
           *  signal to ourselves and hope it kills us.
           */
          exit_code = 128 + WTERMSIG (exit_status);
        }
      else
        {
          /* wait(3p) claims that if the waitpid() call that returned the exit
           * code specified neither WUNTRACED nor WIFSIGNALED, then exactly one
           * of WIFEXITED() or WIFSIGNALED() will be true.
           */
          g_warning ("exit status %d is neither WIFEXITED() nor WIFSIGNALED()",
                     exit_status);
          /* EX_SOFTWARE "internal software error" from sysexits.h, for want of
           * a better code.
           */
          exit_status = 70;
        }

      g_debug ("child exit code %d: %d", client_pid, exit_code);
      exit (exit_code);
  }
}

static void
message_handler (G_GNUC_UNUSED const gchar   *log_domain,
                 GLogLevelFlags               log_level,
                 const gchar                 *message,
                 G_GNUC_UNUSED gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

static gboolean
forward_signal_idle_cb (gpointer user_data)
{
  int sig = GPOINTER_TO_INT(user_data);
  g_autoptr(GVariant) reply = NULL;
  gboolean to_process_group = FALSE;
  g_autoptr(GError) error = NULL;

  g_debug ("Forwarding signal: %d", sig);

  /* We forward stop requests as real stop, because the default doesn't
     seem to be to stop for non-kernel sent TSTP??? */
  if (sig == SIGTSTP)
    sig = SIGSTOP;

  /* ctrl-c/z is typically for the entire process group */
  if (sig == SIGINT || sig == SIGSTOP || sig == SIGCONT)
    to_process_group = TRUE;

  reply = g_dbus_connection_call_sync (session_bus,
                                       service_bus_name,
                                       service_obj_path,
                                       service_iface,
                                       opt_host ? "HostCommandSignal" : "SpawnSignal",
                                       g_variant_new ("(uub)",
                                                      child_pid, sig, to_process_group),
                                       G_VARIANT_TYPE ("()"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1, NULL, &error);

  if (error)
    g_debug ("Failed to forward signal: %s", error->message);

  if (sig == SIGSTOP)
    {
      g_debug ("SIGSTOP:ing flatpak-spawn");
      raise (SIGSTOP);
    }

  return G_SOURCE_REMOVE;
}

static void
forward_signal_handler (int sig)
{
  g_idle_add (forward_signal_idle_cb, GINT_TO_POINTER(sig));
}

static void
forward_signals (void)
{
  int forward[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCONT, SIGTSTP, SIGUSR1, SIGUSR2
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS(forward); i++)
    signal (forward[i], forward_signal_handler);
}

static void
name_owner_changed (G_GNUC_UNUSED GDBusConnection *connection,
                    G_GNUC_UNUSED const gchar     *sender_name,
                    G_GNUC_UNUSED const gchar     *object_path,
                    G_GNUC_UNUSED const gchar     *interface_name,
                    G_GNUC_UNUSED const gchar     *signal_name,
                    GVariant                      *parameters,
                    G_GNUC_UNUSED gpointer         user_data)
{
  const char *name, *from, *to;
  g_variant_get (parameters, "(sss)", &name, &from, &to);

  /* Check if the service dies, then we exit, because we can't track it anymore */
  if (strcmp (name, service_bus_name) == 0 &&
      strcmp (to, "") == 0)
    {
      g_debug ("portal exited");
      exit (1);
    }
}

static gboolean
command_specified (GPtrArray *child_argv,
                   GError   **error)
{
  if (child_argv->len > 1)
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               "No command specified");
  return FALSE;
}

static void
session_bus_closed_cb (G_GNUC_UNUSED GDBusConnection *bus,
                       G_GNUC_UNUSED gboolean remote_peer_vanished,
                       G_GNUC_UNUSED GError *error,
                       GMainLoop *loop)
{
  g_debug ("Session bus connection closed, quitting");
  g_main_loop_quit (loop);
}

static gboolean opt_sandbox_flags = 0;

static gboolean
sandbox_flag_callback (G_GNUC_UNUSED const gchar *option_name,
                       const gchar *value,
                       G_GNUC_UNUSED gpointer data,
                       GError **error)
{
  long val;
  char *end;

  if (strcmp (value, "share-display") == 0)
    {
      opt_sandbox_flags |= FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_DISPLAY;
      return TRUE;
    }

  if (strcmp (value, "share-sound") == 0)
    {
      opt_sandbox_flags |= FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_SOUND;
      return TRUE;
    }

  if (strcmp (value, "share-gpu") == 0)
    {
      opt_sandbox_flags |= FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_GPU;
      return TRUE;
    }

  if (strcmp (value, "allow-dbus") == 0)
    {
      opt_sandbox_flags |= FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS;
      return TRUE;
    }

  if (strcmp (value, "allow-a11y") == 0)
    {
      opt_sandbox_flags |= FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS;
      return TRUE;
    }

  val = strtol (value, &end, 10);
  if (val > 0 && *end == 0)
    {
      opt_sandbox_flags |= val;
      return TRUE;
    }

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               "Unknown sandbox flag %s", value);
  return FALSE;
}

static guint32
get_portal_version (void)
{
  static guint32 version = 0;

  if (version == 0)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) reply =
        g_dbus_connection_call_sync (session_bus,
                                     service_bus_name,
                                     service_obj_path,
                                     "org.freedesktop.DBus.Properties",
                                     "Get",
                                     g_variant_new ("(ss)", service_iface, "version"),
                                     G_VARIANT_TYPE ("(v)"),
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL, &error);

      if (reply == NULL)
        g_debug ("Failed to get version: %s", error->message);
      else
        {
          g_autoptr(GVariant) v = g_variant_get_child_value (reply, 0);
          g_autoptr(GVariant) v2 = g_variant_get_variant (v);
          version = g_variant_get_uint32 (v2);
        }
    }

  return version;
}

static void
check_portal_version (const char *option, guint32 version_needed)
{
  guint32 portal_version = get_portal_version ();
  if (portal_version < version_needed)
    {
      g_printerr ("--%s not supported by host portal version (need version %d, has %d)\n", option, version_needed, portal_version);
      exit (1);
    }
}

static guint32
get_portal_supports (void)
{
  static guint32 supports = 0;
  static gboolean ran = FALSE;

  if (!ran)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) reply = NULL;

      ran = TRUE;

      /* Support flags were added in version 3 */
      if (get_portal_version () >= 3)
        {
          reply = g_dbus_connection_call_sync (session_bus,
                                               service_bus_name,
                                               service_obj_path,
                                               "org.freedesktop.DBus.Properties",
                                               "Get",
                                               g_variant_new ("(ss)", service_iface, "supports"),
                                               G_VARIANT_TYPE ("(v)"),
                                               G_DBUS_CALL_FLAGS_NONE,
                                               -1,
                                               NULL, &error);
          if (reply == NULL)
            g_debug ("Failed to get supports: %s", error->message);
          else
            {
              g_autoptr(GVariant) v = g_variant_get_child_value (reply, 0);
              g_autoptr(GVariant) v2 = g_variant_get_variant (v);
              supports = g_variant_get_uint32 (v2);
            }
        }
    }

  return supports;
}

static void
check_portal_supports (const char *option, guint32 supports_needed)
{
  guint32 supports = get_portal_supports ();

  if ((supports & supports_needed) != supports_needed)
    {
      g_printerr ("--%s not supported by host portal\n", option);
      exit (1);
    }
}

int
main (int    argc,
      char **argv)
{
  GMainLoop *loop;
  g_autoptr(GError) error = NULL;
  GOptionContext *context;
  g_autoptr(GPtrArray) child_argv = NULL;
  int i, opt_argc;
  gboolean verbose = FALSE;
  char **forward_fds = NULL;
  char **opt_envs = NULL;
  guint spawn_flags;
  gboolean opt_clear_env = FALSE;
  gboolean opt_watch_bus = FALSE;
  gboolean opt_expose_pids = FALSE;
  gboolean opt_latest_version = FALSE;
  gboolean opt_sandbox = FALSE;
  gboolean opt_no_network = FALSE;
  char **opt_sandbox_expose = NULL;
  char **opt_sandbox_expose_ro = NULL;
  char **opt_sandbox_expose_path = NULL;
  char **opt_sandbox_expose_path_ro = NULL;
  char *opt_directory = NULL;
  g_autofree char *cwd = NULL;
  GVariantBuilder options_builder;
  const GOptionEntry options[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output", NULL },
    { "forward-fd", 0, 0, G_OPTION_ARG_STRING_ARRAY, &forward_fds,  "Forward file descriptor", "FD" },
    { "clear-env", 0, 0, G_OPTION_ARG_NONE, &opt_clear_env,  "Run with clean environment", NULL },
    { "watch-bus", 0, 0, G_OPTION_ARG_NONE, &opt_watch_bus,  "Make the spawned command exit if we do", NULL },
    { "expose-pids", 0, 0, G_OPTION_ARG_NONE, &opt_expose_pids, "Expose sandbox pid in calling sandbox", NULL },
    { "env", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_envs, "Set environment variable", "VAR=VALUE" },
    { "latest-version", 0, 0, G_OPTION_ARG_NONE, &opt_latest_version,  "Run latest version", NULL },
    { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandbox,  "Run sandboxed", NULL },
    { "no-network", 0, 0, G_OPTION_ARG_NONE, &opt_no_network,  "Run without network access", NULL },
    { "sandbox-expose", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sandbox_expose, "Expose access to named file", "NAME" },
    { "sandbox-expose-ro", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sandbox_expose_ro, "Expose readonly access to named file", "NAME" },
    { "sandbox-expose-path", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sandbox_expose_path, "Expose access to path", "PATH" },
    { "sandbox-expose-path-ro", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sandbox_expose_path_ro, "Expose readonly access to path", "PATH" },
    { "sandbox-flag", 0, 0, G_OPTION_ARG_CALLBACK, sandbox_flag_callback, "Enable sandbox flag", "FLAG" },
    { "host", 0, 0, G_OPTION_ARG_NONE, &opt_host, "Start the command on the host", NULL },
    { "directory", 0, 0, G_OPTION_ARG_FILENAME, &opt_directory, "Working directory in which to run the command", "DIR" },
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  child_argv = g_ptr_array_new ();

  cwd = g_get_current_dir ();

  i = 1;
  while (i < argc && argv[i][0] == '-')
    i++;

  opt_argc = i;

  while (i < argc)
    {
      g_ptr_array_add (child_argv, argv[i]);
      i++;
    }
  g_ptr_array_add (child_argv, NULL);

  context = g_option_context_new ("COMMAND [ARGUMENT…]");

  g_option_context_set_summary (context, "Run a command in a sandbox");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &opt_argc, &argv, &error) ||
      !command_specified (child_argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name(), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      g_option_context_free (context);
      return 1;
    }

  if (verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("Can't find bus: %s\n", error->message);
      return 1;
    }

  if (opt_host)
    {
      service_iface = "org.freedesktop.Flatpak.Development";
      service_obj_path = "/org/freedesktop/Flatpak/Development";
      service_bus_name = "org.freedesktop.Flatpak";
    }
  else
    {
      service_iface = "org.freedesktop.portal.Flatpak";
      service_obj_path = "/org/freedesktop/portal/Flatpak";
      service_bus_name = "org.freedesktop.portal.Flatpak";
    }

  g_dbus_connection_signal_subscribe (session_bus,
                                      NULL,
                                      service_iface,
                                      opt_host ? "HostCommandExited" : "SpawnExited",
                                      service_obj_path,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      spawn_exited_cb,
                                      NULL, NULL);

  g_autoptr(GVariantBuilder) fd_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{uh}"));
  g_autoptr(GVariantBuilder) env_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{ss}"));
  g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new ();
  gint stdin_handle = -1;
  gint stdout_handle = -1;
  gint stderr_handle = -1;

  stdin_handle = g_unix_fd_list_append (fd_list, 0, &error);
  if (stdin_handle == -1)
    {
      g_printerr ("Can't append fd: %s\n", error->message);
      return 1;
    }
  stdout_handle = g_unix_fd_list_append (fd_list, 1, &error);
  if (stdout_handle == -1)
    {
      g_printerr ("Can't append fd: %s\n", error->message);
      return 1;
    }
  stderr_handle = g_unix_fd_list_append (fd_list, 2, &error);
  if (stderr_handle == -1)
    {
      g_printerr ("Can't append fd: %s\n", error->message);
      return 1;
    }

  g_variant_builder_add (fd_builder, "{uh}", 0, stdin_handle);
  g_variant_builder_add (fd_builder, "{uh}", 1, stdout_handle);
  g_variant_builder_add (fd_builder, "{uh}", 2, stderr_handle);
  g_autoptr(GVariant) reply = NULL;

  for (i = 0; forward_fds != NULL && forward_fds[i] != NULL; i++)
    {
      int fd = strtol (forward_fds[i],  NULL, 10);
      gint handle = -1;

      if (fd == 0)
        {
          g_printerr ("Invalid fd '%s'\n", forward_fds[i]);
          return 1;
        }

      if (fd >= 0 && fd <= 2)
        continue; // We always forward these

      handle = g_unix_fd_list_append (fd_list, fd, &error);
      if (handle == -1)
        {
          g_printerr ("Can't append fd: %s\n", error->message);
          return 1;
        }
      g_variant_builder_add (fd_builder, "{uh}", fd, handle);
    }

  for (i = 0; opt_envs != NULL && opt_envs[i] != NULL; i++)
    {
      const char *opt_env = opt_envs[i];
      g_auto(GStrv) split = g_strsplit (opt_env, "=", 2);

      if (split == NULL || split[0] == NULL || split[0][0] == 0 || split[1] == NULL)
        {
          g_printerr ("Invalid env format %s\n", opt_env);
          return 1;
        }
      g_variant_builder_add (env_builder, "{ss}", split[0], split[1]);
    }

  spawn_flags = 0;

  if (opt_clear_env)
    spawn_flags |= opt_host ? FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV : FLATPAK_SPAWN_FLAGS_CLEAR_ENV;

  if (opt_watch_bus)
    spawn_flags |= opt_host ? FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS : FLATPAK_SPAWN_FLAGS_WATCH_BUS;

  if (opt_expose_pids)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --expose-pids");
          return 1;
        }

      check_portal_version ("expose-pids", 3);
      check_portal_supports ("expose-pids", FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS);

      spawn_flags |= FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS;
    }

  if (opt_latest_version)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --latest-version");
          return 1;
        }
      spawn_flags |= FLATPAK_SPAWN_FLAGS_LATEST_VERSION;
    }

  if (opt_sandbox)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox");
          return 1;
        }
      spawn_flags |= FLATPAK_SPAWN_FLAGS_SANDBOX;
    }

  if (opt_no_network)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --no-network");
          return 1;
        }
      spawn_flags |= FLATPAK_SPAWN_FLAGS_NO_NETWORK;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE ("a{sv}"));

  if (opt_sandbox_expose)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose");
          return 1;
        }
      g_variant_builder_add (&options_builder, "{s@v}", "sandbox-expose",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *)opt_sandbox_expose, -1)));
    }

  if (opt_sandbox_expose_ro)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose-ro");
          return 1;
        }
      g_variant_builder_add (&options_builder, "{s@v}", "sandbox-expose-ro",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *)opt_sandbox_expose_ro, -1)));
    }

  if (opt_sandbox_flags)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-flag\n");
          return 1;
        }

      check_portal_version ("sandbox-flags", 3);

      g_variant_builder_add (&options_builder, "{s@v}", "sandbox-flags",
                             g_variant_new_variant (g_variant_new_uint32 (opt_sandbox_flags)));
    }

  if (opt_sandbox_expose_path)
    {
      g_autoptr(GVariantBuilder) expose_fd_builder = g_variant_builder_new (G_VARIANT_TYPE ("ah"));

      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose-path\n");
          return 1;
        }

      check_portal_version ("sandbox-expose-path", 3);

      for (i = 0; opt_sandbox_expose_path[i] != NULL; i++)
        {
          gint handle = -1;
          int path_fd = open (opt_sandbox_expose_path[i], O_PATH|O_CLOEXEC|O_NOFOLLOW|O_RDONLY);
          if (path_fd == -1)
            {
              g_printerr ("Failed to open %s for --sandbox-expose-path\n", opt_sandbox_expose_path[i]);
              return 1;
            }

          handle = g_unix_fd_list_append (fd_list, path_fd, &error);
          g_variant_builder_add (expose_fd_builder, "h", handle);
          g_variant_builder_add (&options_builder, "{s@v}", "sandbox-expose-fd",
                                 g_variant_new_variant (g_variant_builder_end (g_steal_pointer (&expose_fd_builder))));
        }
    }

  if (opt_sandbox_expose_path_ro)
    {
      g_autoptr(GVariantBuilder) expose_fd_builder = g_variant_builder_new (G_VARIANT_TYPE ("ah"));

      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose-path-ro\n");
          return 1;
        }

      check_portal_version ("sandbox-expose-path-ro", 3);

      for (i = 0; opt_sandbox_expose_path_ro[i] != NULL; i++)
        {
          gint handle = -1;
          int path_fd = open (opt_sandbox_expose_path_ro[i], O_PATH|O_CLOEXEC|O_NOFOLLOW|O_RDONLY);
          if (path_fd == -1)
            {
              g_printerr ("Failed to open %s for --sandbox-expose-path-ro\n", opt_sandbox_expose_path_ro[i]);
              return 1;
            }

          handle = g_unix_fd_list_append (fd_list, path_fd, &error);
          g_variant_builder_add (expose_fd_builder, "h", handle);
          g_variant_builder_add (&options_builder, "{s@v}", "sandbox-expose-fd-ro",
                                 g_variant_new_variant (g_variant_builder_end (g_steal_pointer (&expose_fd_builder))));
        }
    }

  if (!opt_directory)
    {
      opt_directory = cwd;
    }

  g_dbus_connection_signal_subscribe (session_bus,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  {
    g_autoptr(GVariant) fds = NULL;
    g_autoptr(GVariant) env = NULL;
    g_autoptr(GVariant) opts = NULL;

    fds = g_variant_ref_sink (g_variant_builder_end (g_steal_pointer (&fd_builder)));
    env = g_variant_ref_sink (g_variant_builder_end (g_steal_pointer (&env_builder)));
    opts = g_variant_ref_sink (g_variant_builder_end (&options_builder));

retry:
    reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                           service_bus_name,
                                                           service_obj_path,
                                                           service_iface,
                                                           opt_host ? "HostCommand" : "Spawn",
                                                           opt_host ?
                                                           g_variant_new ("(^ay^aay@a{uh}@a{ss}u)",
                                                                          opt_directory,
                                                                          (const char * const *) child_argv->pdata,
                                                                          fds,
                                                                          env,
                                                                          spawn_flags)
                                                           :
                                                           g_variant_new ("(^ay^aay@a{uh}@a{ss}u@a{sv})",
                                                                          opt_directory,
                                                                          (const char * const *) child_argv->pdata,
                                                                          fds,
                                                                          env,
                                                                          spawn_flags,
                                                                          opts),
                                                           G_VARIANT_TYPE ("(u)"),
                                                           G_DBUS_CALL_FLAGS_NONE,
                                                           -1,
                                                           fd_list,
                                                           NULL,
                                                           NULL, &error);

    if (reply == NULL)
      {
        if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS) &&
            opt_watch_bus)
          {
            g_debug ("Got an invalid argument error; trying again without --watch-bus");

            opt_watch_bus = FALSE;
            spawn_flags &= opt_host ? ~FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS : ~FLATPAK_SPAWN_FLAGS_WATCH_BUS;
            g_clear_error (&error);

            goto retry;
          }

        g_dbus_error_strip_remote_error (error);
        g_printerr ("Portal call failed: %s\n", error->message);
        return 1;
      }

    g_variant_get (reply, "(u)", &child_pid);
  }

  g_debug ("child_pid: %d", child_pid);

  forward_signals ();

  loop = g_main_loop_new (NULL, FALSE);

  g_signal_connect (session_bus, "closed", G_CALLBACK (session_bus_closed_cb), loop);

  g_main_loop_run (loop);

  return 0;
}
