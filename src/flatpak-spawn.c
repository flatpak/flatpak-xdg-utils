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

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

typedef enum {
  FLATPAK_SPAWN_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_SPAWN_FLAGS_LATEST_VERSION = 1 << 1,
  FLATPAK_SPAWN_FLAGS_SANDBOX = 1 << 2,
  FLATPAK_SPAWN_FLAGS_NO_NETWORK = 1 << 3,
} FlatpakSpawnFlags;

typedef enum {
  FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV = 1 << 0,
} FlatpakHostCommandFlags;

static GDBusConnection *session_bus = NULL;

guint child_pid = 0;
gboolean opt_host;

const char *service_iface;
const char *service_obj_path;
const char *service_bus_name;

static void
spawn_exited_cb (GDBusConnection *connection,
                 const gchar     *sender_name,
                 const gchar     *object_path,
                 const gchar     *interface_name,
                 const gchar     *signal_name,
                 GVariant        *parameters,
                 gpointer         user_data)
{
  guint32 client_pid = 0;
  guint32 exit_status = 0;

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(uu)")))
    return;

  g_variant_get (parameters, "(uu)", &client_pid, &exit_status);
  g_debug ("child exited %d: %d", client_pid, exit_status);

  if (child_pid == client_pid)
    exit (exit_status);
}

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
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
                                       0, NULL, NULL);

  if (sig == SIGSTOP)
    {
      g_debug ("SIGSTOP:ing flatpak-portal-spawn");
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
  int i;

  for (i = 0; i < G_N_ELEMENTS(forward); i++)
    signal (forward[i], forward_signal_handler);
}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
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
  guint spawn_flags;
  gboolean opt_clear_env = FALSE;
  gboolean opt_latest_version = FALSE;
  gboolean opt_sandbox = FALSE;
  gboolean opt_no_network = FALSE;
  char **opt_sandbox_expose = NULL;
  char **opt_sandbox_expose_ro = NULL;
  GVariantBuilder options_builder;
  const GOptionEntry options[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output.", NULL },
    { "forward-fd", 0, 0, G_OPTION_ARG_STRING_ARRAY, &forward_fds,  "Forward file descriptor.", NULL },
    { "clear-env", 0, 0, G_OPTION_ARG_NONE, &opt_clear_env,  "Run with clean env.", NULL },
    { "latest-version", 0, 0, G_OPTION_ARG_NONE, &opt_latest_version,  "Run latest version.", NULL },
    { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandbox,  "Run sandboxed.", NULL },
    { "no-network", 0, 0, G_OPTION_ARG_NONE, &opt_no_network,  "Run without network access.", NULL },
    { "sandbox-expose", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sandbox_expose, "Expose access to named sandbox", "NAME" },
    { "sandbox-expose-ro", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sandbox_expose_ro, "Expose readonly access to named sandbox", "NAME" },
    { "host", 0, 0, G_OPTION_ARG_NONE, &opt_host, "Start the command on the host (requires access to org.freedesktop.Flatpak)", NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  child_argv = g_ptr_array_new ();

  i = 1;
  while (i < argc && argv[i][0] == '-')
    i++;

  if (i == argc)
    {
      g_printerr ("No command specified\n");
      return 1;
    }

  opt_argc = i;

  while (i < argc)
    {
      g_ptr_array_add (child_argv, argv[i]);
      i++;
    }
  g_ptr_array_add (child_argv, NULL);

  context = g_option_context_new ("");

  g_option_context_set_summary (context, "Flatpak portal spawn");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &opt_argc, &argv, &error))
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

  spawn_flags = 0;

  if (opt_clear_env)
    spawn_flags |= opt_host ? FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV : FLATPAK_SPAWN_FLAGS_CLEAR_ENV;

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

  g_dbus_connection_signal_subscribe (session_bus,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                         service_bus_name,
                                                         service_obj_path,
                                                         service_iface,
                                                         opt_host ? "HostCommand" : "Spawn",
                                                         opt_host ?
                                                         g_variant_new ("(^ay^aay@a{uh}@a{ss}u)",
                                                                        "",
                                                                        (const char * const *) child_argv->pdata,
                                                                        g_variant_builder_end (g_steal_pointer (&fd_builder)),
                                                                        g_variant_builder_end (g_steal_pointer (&env_builder)),
                                                                        spawn_flags)
                                                         :
                                                         g_variant_new ("(^ay^aay@a{uh}@a{ss}u@a{sv})",
                                                                        "",
                                                                        (const char * const *) child_argv->pdata,
                                                                        g_variant_builder_end (g_steal_pointer (&fd_builder)),
                                                                        g_variant_builder_end (g_steal_pointer (&env_builder)),
                                                                        spawn_flags, g_variant_builder_end (&options_builder)),
                                                         G_VARIANT_TYPE ("(u)"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         -1,
                                                         fd_list,
                                                         NULL,
                                                         NULL, &error);

  if (reply == NULL)
    {
      g_printerr ("Failed to call flatpak portal: %s\n", error->message);
      return 1;
    }

  g_variant_get (reply, "(u)", &child_pid);

  g_debug ("child_pid: %d", child_pid);

  forward_signals ();

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}
