/*
 * Copyright © 2018-2019 Collabora Ltd.
 * Copyright © 2021 Simon McVittie
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "backport-autoptr.h"
#include "common.h"

typedef enum
{
  FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS = 1 << 1,
} FlatpakHostCommandFlags;

#define FLATPAK_SESSION_HELPER_BUS_NAME "org.freedesktop.Flatpak"
#define FLATPAK_SESSION_HELPER_PATH_DEVELOPMENT "/org/freedesktop/Flatpak/Development"
#define FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT "org.freedesktop.Flatpak.Development"

typedef enum {
  FLATPAK_SPAWN_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_SPAWN_FLAGS_LATEST_VERSION = 1 << 1,
  FLATPAK_SPAWN_FLAGS_SANDBOX = 1 << 2,
  FLATPAK_SPAWN_FLAGS_NO_NETWORK = 1 << 3,
  FLATPAK_SPAWN_FLAGS_WATCH_BUS = 1 << 4,
  FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS = 1 << 5,
  FLATPAK_SPAWN_FLAGS_NOTIFY_START = 1 << 6,
  FLATPAK_SPAWN_FLAGS_SHARE_PIDS = 1 << 7,
  FLATPAK_SPAWN_FLAGS_EMPTY_APP = 1 << 8,
} FlatpakSpawnFlags;

typedef enum {
  FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_DISPLAY = 1 << 0,
  FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_SOUND = 1 << 1,
  FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_GPU = 1 << 2,
  FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS = 1 << 3,
  FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_A11Y = 1 << 4,
} FlatpakSpawnSandboxFlags;
#define FLATPAK_SPAWN_SANDBOX_FLAGS_FUTURE (1 << 23)

typedef enum {
  FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS = 1 << 0,
} FlatpakSpawnSupportFlags;

#define FLATPAK_PORTAL_BUS_NAME "org.freedesktop.portal.Flatpak"
#define FLATPAK_PORTAL_PATH "/org/freedesktop/portal/Flatpak"
#define FLATPAK_PORTAL_INTERFACE FLATPAK_PORTAL_BUS_NAME

typedef struct
{
  const char *extra_arg;
  FlatpakHostCommandFlags host_flags;
  FlatpakSpawnFlags subsandbox_flags;
  FlatpakSpawnSandboxFlags subsandbox_sandbox_flags;
  FlatpakSpawnSupportFlags portal_supports;
  const char *app_path;
  const char *usr_path;
  int fails_immediately;
  int fails_after_version_check;
  gboolean awkward_command_name;
  gboolean dbus_call_fails;
  gboolean extra;
  gboolean host;
  gboolean no_command;
  gboolean no_session_bus;
  gboolean sandbox_complex;
} Config;

typedef struct
{
  const Config *config;
  GSubprocess *dbus_daemon;
  gchar *dbus_address;
  GSubprocess *flatpak_spawn;
  gchar *flatpak_spawn_path;
  GDBusConnection *mock_development_conn;
  GDBusConnection *mock_portal_conn;
  guint mock_development_object;
  guint mock_portal_object;
  GQueue invocations;
  guint32 mock_development_version;
  guint32 mock_portal_version;
  guint32 mock_portal_supports;
} Fixture;

static const Config default_config = {};

static void
mock_method_call (GDBusConnection *conn G_GNUC_UNUSED,
                  const gchar *sender G_GNUC_UNUSED,
                  const gchar *object_path G_GNUC_UNUSED,
                  const gchar *interface_name,
                  const gchar *method_name,
                  GVariant *parameters G_GNUC_UNUSED,
                  GDBusMethodInvocation *invocation,
                  gpointer user_data)
{
  Fixture *f = user_data;
  g_autofree gchar *params = NULL;

  params = g_variant_print (parameters, TRUE);

  g_test_message ("Method called: %s.%s%s", interface_name, method_name,
                  params);

  g_queue_push_tail (&f->invocations, g_object_ref (invocation));

  if (f->config->dbus_call_fails)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "com.example.No",
                                                  "Mock portal failed");
      return;
    }

  if (strcmp (method_name, "HostCommand") == 0 ||
      strcmp (method_name, "Spawn") == 0)
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(u)", 12345));
  else    /* HostCommandSignal or SpawnSignal */
    g_dbus_method_invocation_return_value (invocation, NULL);
}

static GVariant *
mock_get_property (GDBusConnection *conn G_GNUC_UNUSED,
                   const gchar *sender G_GNUC_UNUSED,
                   const gchar *object_path G_GNUC_UNUSED,
                   const gchar *interface_name,
                   const gchar *property_name,
                   GError **error,
                   gpointer user_data)
{
  Fixture *f = user_data;

  g_test_message ("Property retrieved: %s.%s", interface_name, property_name);

  if (strcmp (interface_name, FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT) == 0)
    {
      if (strcmp (property_name, "version") == 0)
        return g_variant_new_uint32 (f->mock_development_version);
    }

  if (strcmp (interface_name, FLATPAK_PORTAL_INTERFACE) == 0)
    {
      if (strcmp (property_name, "supports") == 0)
        return g_variant_new_uint32 (f->mock_portal_supports);

      if (strcmp (property_name, "version") == 0)
        return g_variant_new_uint32 (f->mock_portal_version);
    }

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown interface or property %s.%s",
               interface_name, property_name);
  return NULL;
}

static GDBusArgInfo arg_cwd_path =
{
  -1,
  "cwd_path",
  "ay",
  NULL  /* annotations */
};

static GDBusArgInfo arg_argv =
{
  -1,
  "argv",
  "aay",
  NULL  /* annotations */
};

static GDBusArgInfo arg_fds =
{
  -1,
  "fds",
  "a{uh}",
  NULL  /* annotations */
};

static GDBusArgInfo arg_envs =
{
  -1,
  "envs",
  "a{ss}",
  NULL  /* annotations */
};

static GDBusArgInfo arg_flags =
{
  -1,
  "flags",
  "u",
  NULL  /* annotations */
};

static GDBusArgInfo arg_out_pid =
{
  -1,
  "pid",
  "u",
  NULL  /* annotations */
};

static GDBusArgInfo *host_command_in_args[] =
{
  &arg_cwd_path,
  &arg_argv,
  &arg_fds,
  &arg_envs,
  &arg_flags,
  NULL
};

static GDBusArgInfo *host_command_out_args[] =
{
  &arg_out_pid,
  NULL
};

static GDBusMethodInfo host_command_info =
{
  -1,
  "HostCommand",
  host_command_in_args,
  host_command_out_args,
  NULL  /* annotations */
};

static GDBusArgInfo arg_pid =
{
  -1,
  "pid",
  "u",
  NULL  /* annotations */
};

static GDBusArgInfo arg_signal =
{
  -1,
  "signal",
  "u",
  NULL  /* annotations */
};

static GDBusArgInfo arg_to_process_group =
{
  -1,
  "to_process_group",
  "b",
  NULL  /* annotations */
};

static GDBusArgInfo *host_command_signal_in_args[] =
{
  &arg_pid,
  &arg_signal,
  &arg_to_process_group,
  NULL
};

static GDBusMethodInfo host_command_signal_info =
{
  -1,
  "HostCommandSignal",
  host_command_signal_in_args,
  NULL, /* out args */
  NULL  /* annotations */
};

static GDBusMethodInfo *development_method_info[] =
{
  &host_command_info,
  &host_command_signal_info,
  NULL
};

static GDBusPropertyInfo version_property_info =
{
  -1,
  "version",
  "u",
  G_DBUS_PROPERTY_INFO_FLAGS_READABLE,
  NULL  /* annotations */
};

static GDBusPropertyInfo *version_properties_info[] =
{
  &version_property_info,
  NULL
};

static GDBusInterfaceInfo development_iface_info =
{
  -1,
  FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT,
  development_method_info,
  NULL, /* signals */
  version_properties_info,
  NULL  /* annotations */
};

static GDBusArgInfo arg_options =
{
  -1,
  "options",
  "a{sv}",
  NULL  /* annotations */
};

static GDBusArgInfo *spawn_in_args[] =
{
  &arg_cwd_path,
  &arg_argv,
  &arg_fds,
  &arg_envs,
  &arg_flags,
  &arg_options,
  NULL
};

static GDBusArgInfo *spawn_out_args[] =
{
  &arg_out_pid,
  NULL
};

static GDBusMethodInfo spawn_info =
{
  -1,
  "Spawn",
  spawn_in_args,
  spawn_out_args,
  NULL  /* annotations */
};

static GDBusMethodInfo spawn_signal_info =
{
  -1,
  "SpawnSignal",
  host_command_signal_in_args,    /* they're the same */
  NULL, /* out args */
  NULL  /* annotations */
};

static GDBusMethodInfo *portal_method_info[] =
{
  &spawn_info,
  &spawn_signal_info,
  NULL
};

static GDBusPropertyInfo supports_property_info =
{
  -1,
  "supports",
  "u",
  G_DBUS_PROPERTY_INFO_FLAGS_READABLE,
  NULL  /* annotations */
};

static GDBusPropertyInfo *portal_properties_info[] =
{
  &version_property_info,
  &supports_property_info,
  NULL
};

static GDBusInterfaceInfo portal_iface_info =
{
  -1,
  FLATPAK_PORTAL_INTERFACE,
  portal_method_info,
  NULL, /* signals */
  portal_properties_info,
  NULL  /* annotations */
};

static const GDBusInterfaceVTable vtable =
{
  mock_method_call,
  mock_get_property,
  NULL  /* set */
};

static void
setup (Fixture *f,
       gconstpointer context)
{
  g_autoptr(GError) error = NULL;

  if (context == NULL)
    f->config = &default_config;
  else
    f->config = context;

  f->mock_development_version = 1;
  f->mock_portal_version = 6;
  f->mock_portal_supports = f->config->portal_supports;

  g_queue_init (&f->invocations);

  setup_dbus_daemon (&f->dbus_daemon, &f->dbus_address);

  f->flatpak_spawn_path = g_strdup (g_getenv ("FLATPAK_SPAWN"));

  if (f->flatpak_spawn_path == NULL)
    f->flatpak_spawn_path = g_strdup (BINDIR "/flatpak-spawn");

  f->mock_development_conn = g_dbus_connection_new_for_address_sync (f->dbus_address,
                                                                     (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                                      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                                     NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->mock_development_conn);

  f->mock_portal_conn = g_dbus_connection_new_for_address_sync (f->dbus_address,
                                                                (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                                 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                                NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->mock_portal_conn);

  f->mock_development_object = g_dbus_connection_register_object (f->mock_development_conn,
                                                                  FLATPAK_SESSION_HELPER_PATH_DEVELOPMENT,
                                                                  &development_iface_info,
                                                                  &vtable,
                                                                  f,
                                                                  NULL,
                                                                  &error);
  g_assert_no_error (error);
  g_assert_cmpuint (f->mock_development_object, !=, 0);

  f->mock_portal_object = g_dbus_connection_register_object (f->mock_portal_conn,
                                                             FLATPAK_PORTAL_PATH,
                                                             &portal_iface_info,
                                                             &vtable,
                                                             f,
                                                             NULL,
                                                             &error);
  g_assert_no_error (error);
  g_assert_cmpuint (f->mock_portal_object, !=, 0);

  own_name_sync (f->mock_development_conn, FLATPAK_SESSION_HELPER_BUS_NAME);
  own_name_sync (f->mock_portal_conn, FLATPAK_PORTAL_BUS_NAME);
}

static void
test_help (Fixture *f,
           gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autofree gchar *stdout_buf;
  g_autofree gchar *stderr_buf;
  g_autoptr(GError) error = NULL;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_address,
                                TRUE);

  f->flatpak_spawn = g_subprocess_launcher_spawn (launcher, &error,
                                             f->flatpak_spawn_path,
                                             "--help",
                                             NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->flatpak_spawn);

  g_subprocess_communicate_utf8 (f->flatpak_spawn, NULL, NULL, &stdout_buf,
                                 &stderr_buf, &error);
  g_assert_cmpstr (stderr_buf, ==, "");
  g_assert_nonnull (stdout_buf);
  g_test_message ("flatpak-spawn --help: %s", stdout_buf);
  g_assert_true (strstr (stdout_buf, "--latest-version") != NULL);

  g_subprocess_wait_check (f->flatpak_spawn, NULL, &error);
  g_assert_no_error (error);
}

static void
test_command (Fixture *f,
              gconstpointer context)
{
  const Config *config = context;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  g_autoptr(GPtrArray) command = NULL;
  GVariant *parameters;
  const char *cwd;
  g_autofree const char **argv = NULL;
  g_autoptr(GVariant) fds_variant = NULL;
  g_autoptr(GVariant) envs_variant = NULL;
  guint32 flags;
  g_autoptr(GVariant) options_variant = NULL;
  GDBusMessage *message;
  GUnixFDList *fd_list;
  const int *fds;
  const char *s;
  gsize i;
  guint n_fds_for_options = 0;

  g_test_timer_start ();

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_set_cwd (launcher, "/");

  if (config->no_session_bus)
    g_subprocess_launcher_setenv (launcher,
                                  "DBUS_SESSION_BUS_ADDRESS",
                                  "nope:",
                                  TRUE);
  else
    g_subprocess_launcher_setenv (launcher,
                                  "DBUS_SESSION_BUS_ADDRESS",
                                  f->dbus_address,
                                  TRUE);

  command = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (command, g_strdup (f->flatpak_spawn_path));

  if (config->host)
    {
      g_assert_cmpint (config->subsandbox_flags, ==, 0);

      g_ptr_array_add (command, g_strdup ("--host"));

      if (config->host_flags & FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV)
        g_ptr_array_add (command, g_strdup ("--clear-env"));

      if (config->host_flags & FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS)
        g_ptr_array_add (command, g_strdup ("--watch-bus"));
    }
  else
    {
      g_assert_cmpint (config->host_flags, ==, 0);

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_CLEAR_ENV)
        g_ptr_array_add (command, g_strdup ("--clear-env"));

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_LATEST_VERSION)
        g_ptr_array_add (command, g_strdup ("--latest-version"));

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_SANDBOX)
        {
          g_ptr_array_add (command, g_strdup ("--sandbox"));

          if (config->subsandbox_sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_DISPLAY)
            g_ptr_array_add (command, g_strdup ("--sandbox-flag=share-display"));

          if (config->subsandbox_sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_SOUND)
            g_ptr_array_add (command, g_strdup ("--sandbox-flag=share-sound"));

          if (config->subsandbox_sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_GPU)
            g_ptr_array_add (command, g_strdup ("--sandbox-flag=share-gpu"));

          if (config->subsandbox_sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS)
            g_ptr_array_add (command, g_strdup ("--sandbox-flag=allow-dbus"));

          if (config->subsandbox_sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_A11Y)
            g_ptr_array_add (command, g_strdup ("--sandbox-flag=allow-a11y"));

          if (config->subsandbox_sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_FUTURE)
            g_ptr_array_add (command, g_strdup ("--sandbox-flag=8388608"));

          if (config->sandbox_complex)
            {
              g_ptr_array_add (command, g_strdup ("--sandbox-expose=/foo"));
              g_ptr_array_add (command, g_strdup ("--sandbox-expose=/bar"));
              g_ptr_array_add (command, g_strdup ("--sandbox-expose-ro=/proc"));
              g_ptr_array_add (command, g_strdup ("--sandbox-expose-ro=/sys"));
              g_ptr_array_add (command, g_strdup ("--sandbox-expose-path=/"));
              g_ptr_array_add (command, g_strdup ("--sandbox-expose-path-ro=/dev"));
              n_fds_for_options += 2;
            }
        }

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_NO_NETWORK)
        g_ptr_array_add (command, g_strdup ("--no-network"));

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_WATCH_BUS)
        g_ptr_array_add (command, g_strdup ("--watch-bus"));

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS)
        g_ptr_array_add (command, g_strdup ("--expose-pids"));

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_NOTIFY_START)
        g_assert_not_reached ();  /* TODO: unimplemented */

      if (config->subsandbox_flags & FLATPAK_SPAWN_FLAGS_SHARE_PIDS)
        g_ptr_array_add (command, g_strdup ("--share-pids"));
    }

  if (config->app_path != NULL)
    {
      g_ptr_array_add (command, g_strdup_printf ("--app-path=%s", config->app_path));

      if (config->app_path[0] != '\0')
        n_fds_for_options++;
    }

  if (config->usr_path != NULL)
    {
      g_ptr_array_add (command, g_strdup_printf ("--usr-path=%s", config->usr_path));
      n_fds_for_options++;
    }

  /* Generic "extra complexity" options */
  if (config->extra)
    {
      g_ptr_array_add (command, g_strdup ("--directory=/dev"));
      g_ptr_array_add (command, g_strdup ("--env=FOO=bar"));
      g_ptr_array_add (command, g_strdup ("--forward-fd=2"));
      g_subprocess_launcher_take_fd (launcher, open ("/dev/null", O_RDWR|O_CLOEXEC), 4);
      g_ptr_array_add (command, g_strdup ("--forward-fd=4"));
      g_ptr_array_add (command, g_strdup ("--unset-env=NOPE"));
      g_ptr_array_add (command, g_strdup ("--verbose"));
    }

  if (config->extra_arg != NULL)
    g_ptr_array_add (command, g_strdup (config->extra_arg));

  if (config->awkward_command_name)
    g_ptr_array_add (command, g_strdup ("some=command"));
  else if (!config->no_command)
    g_ptr_array_add (command, g_strdup ("some-command"));

  if (config->extra)
    {
      g_ptr_array_add (command, g_strdup ("--arg1"));
      g_ptr_array_add (command, g_strdup ("arg2"));
    }

  g_ptr_array_add (command, NULL);

  f->flatpak_spawn = g_subprocess_launcher_spawnv (launcher,
                                                   (const char * const *) command->pdata,
                                                   &error);

  g_assert_no_error (error);
  g_assert_nonnull (f->flatpak_spawn);

  if (config->fails_immediately)
    {
      g_subprocess_wait_check (f->flatpak_spawn, NULL, &error);
      g_assert_error (error, G_SPAWN_EXIT_ERROR, config->fails_immediately);

      /* Make sure we didn't wait for the entire 25 second D-Bus timeout */
      g_assert_cmpfloat (g_test_timer_elapsed (), <=, 20);

      g_test_minimized_result (g_test_timer_elapsed (),
                               "time to fail immediately: %.1f",
                               g_test_timer_elapsed ());
      return;
    }

  if (config->fails_after_version_check)
    {
      g_autoptr(GAsyncResult) result = NULL;

      g_subprocess_wait_check_async (f->flatpak_spawn, NULL,
                                     store_result_cb, &result);

      while (result == NULL)
        g_main_context_iteration (NULL, TRUE);

      g_subprocess_wait_check_finish (f->flatpak_spawn, result, &error);
      g_assert_error (error, G_SPAWN_EXIT_ERROR, config->fails_after_version_check);

      /* Make sure we didn't wait for the entire 25 second D-Bus timeout */
      g_assert_cmpfloat (g_test_timer_elapsed (), <=, 20);

      g_test_minimized_result (g_test_timer_elapsed (),
                               "time to fail after version check: %.1f",
                               g_test_timer_elapsed ());

      g_assert_cmpuint (g_queue_get_length (&f->invocations), ==, 0);
      return;
    }

  while (g_queue_get_length (&f->invocations) < 1)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpuint (g_queue_get_length (&f->invocations), ==, 1);
  invocation = g_queue_pop_head (&f->invocations);
  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);
  fds = g_unix_fd_list_peek_fds (fd_list, NULL);

  if (config->host)
    {
      g_assert_cmpstr (g_dbus_method_invocation_get_interface_name (invocation),
                       ==, FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT);
      g_assert_cmpstr (g_dbus_method_invocation_get_method_name (invocation),
                       ==, "HostCommand");
    }
  else
    {
      g_assert_cmpstr (g_dbus_method_invocation_get_interface_name (invocation),
                       ==, FLATPAK_PORTAL_INTERFACE);
      g_assert_cmpstr (g_dbus_method_invocation_get_method_name (invocation),
                       ==, "Spawn");
    }

  parameters = g_dbus_method_invocation_get_parameters (invocation);

  if (config->host)
    {
      g_assert_cmpstr (g_variant_get_type_string (parameters), ==,
                       "(ayaaya{uh}a{ss}u)");
      g_variant_get (parameters, "(^&ay^a&ay@a{uh}@a{ss}u)",
                     &cwd, &argv, &fds_variant, &envs_variant, &flags);
    }
  else
    {
      g_assert_cmpstr (g_variant_get_type_string (parameters), ==,
                       "(ayaaya{uh}a{ss}ua{sv})");
      g_variant_get (parameters, "(^&ay^a&ay@a{uh}@a{ss}u@a{sv})",
                     &cwd, &argv, &fds_variant, &envs_variant, &flags,
                     &options_variant);
    }

  if (config->extra)
    g_assert_cmpstr (cwd, ==, "/dev");
  else
    g_assert_cmpstr (cwd, ==, "/");

  g_assert_nonnull (argv);
  i = 0;

  if (config->extra && config->host)
    {
      g_assert_cmpstr (argv[i++], ==, "/usr/bin/env");
      g_assert_cmpstr (argv[i++], ==, "-u");
      g_assert_cmpstr (argv[i++], ==, "NOPE");

      if (config->awkward_command_name)
        {
          g_assert_cmpstr (argv[i++], ==, "/bin/sh");
          g_assert_cmpstr (argv[i++], ==, "-euc");
          g_assert_cmpstr (argv[i++], ==, "exec \"$@\"");
          g_assert_cmpstr (argv[i++], ==, "sh");  /* sh's argv[0] */
        }
    }

  if (config->awkward_command_name)
    {
      g_assert_cmpstr (argv[i++], ==, "some=command");
    }
  else
    {
      g_assert_cmpstr (argv[i++], ==, "some-command");
    }

  if (config->extra)
    {
      g_assert_cmpstr (argv[i++], ==, "--arg1");
      g_assert_cmpstr (argv[i++], ==, "arg2");
    }

  g_assert_cmpstr (argv[i++], ==, NULL);

  g_assert_cmpstr (g_variant_get_type_string (fds_variant), ==, "a{uh}");

  /* it carries stdin, stdout, stderr, and maybe fd 4 */
  if (config->extra)
    g_assert_cmpuint (g_variant_n_children (fds_variant), ==, 4);
  else
    g_assert_cmpuint (g_variant_n_children (fds_variant), ==, 3);

  g_assert_cmpstr (g_variant_get_type_string (envs_variant), ==, "a{ss}");

  if (config->extra)
    {
      g_assert_cmpuint (g_variant_n_children (envs_variant), ==, 1);
      g_assert_true (g_variant_lookup (envs_variant, "FOO", "&s", &s));
      g_assert_cmpstr (s, ==, "bar");
    }
  else
    {
      g_assert_cmpuint (g_variant_n_children (envs_variant), ==, 0);
    }

  if (config->host)
    {
      g_assert_cmpuint (flags, ==, config->host_flags);
    }
  else
    {
      guint options_handled = 0;

      g_assert_cmpuint (flags, ==, config->subsandbox_flags);
      g_assert_cmpstr (g_variant_get_type_string (options_variant), ==, "a{sv}");

      if (config->sandbox_complex)
        {
          g_autofree const char **expose = NULL;
          g_autofree const char **ro = NULL;
          GVariantIter *handles_iter;

          g_assert_true (g_variant_lookup (options_variant, "sandbox-expose", "^a&s", &expose));
          g_assert_nonnull (expose);
          i = 0;
          g_assert_cmpstr (expose[i++], ==, "/foo");
          g_assert_cmpstr (expose[i++], ==, "/bar");
          g_assert_cmpstr (expose[i++], ==, NULL);
          options_handled++;

          g_assert_true (g_variant_lookup (options_variant, "sandbox-expose-ro", "^a&s", &ro));
          g_assert_nonnull (ro);
          i = 0;
          g_assert_cmpstr (ro[i++], ==, "/proc");
          g_assert_cmpstr (ro[i++], ==, "/sys");
          g_assert_cmpstr (ro[i++], ==, NULL);
          options_handled++;

          g_assert_true (g_variant_lookup (options_variant, "sandbox-flags", "u", &flags));
          g_assert_cmpuint (flags, ==, config->subsandbox_sandbox_flags);
          options_handled++;

          g_assert_true (g_variant_lookup (options_variant, "sandbox-expose-fd", "ah", &handles_iter));
          g_assert_nonnull (handles_iter);
          g_variant_iter_free (handles_iter);
          options_handled++;

          g_assert_true (g_variant_lookup (options_variant, "sandbox-expose-fd-ro", "ah", &handles_iter));
          g_assert_nonnull (handles_iter);
          g_variant_iter_free (handles_iter);
          options_handled++;
        }

      if (config->extra)
        {
          g_autofree const char **unset = NULL;

          g_assert_true (g_variant_lookup (options_variant, "unset-env", "^a&s", &unset));
          g_assert_nonnull (unset);
          i = 0;
          g_assert_cmpstr (unset[i++], ==, "NOPE");
          g_assert_cmpstr (unset[i++], ==, NULL);
          options_handled++;
        }

      if (config->app_path != NULL && config->app_path[0] != '\0')
        {
          struct stat expected, got;
          gint32 handle;

          g_assert_no_errno (stat (config->app_path, &expected));
          g_assert_true (g_variant_lookup (options_variant, "app-fd", "h", &handle));
          g_assert_cmpint (handle, >=, 0);
          g_assert_cmpint (handle, <, g_unix_fd_list_get_length (fd_list));
          g_assert_no_errno (fstat (fds[handle], &got));
          g_assert_cmpint (expected.st_dev, ==, got.st_dev);
          g_assert_cmpint (expected.st_ino, ==, got.st_ino);
          options_handled++;
        }

      if (config->usr_path != NULL)
        {
          struct stat expected, got;
          gint32 handle;

          g_assert_no_errno (stat (config->usr_path, &expected));
          g_assert_true (g_variant_lookup (options_variant, "usr-fd", "h", &handle));
          g_assert_cmpint (handle, >=, 0);
          g_assert_cmpint (handle, <, g_unix_fd_list_get_length (fd_list));
          g_assert_no_errno (fstat (fds[handle], &got));
          g_assert_cmpint (expected.st_dev, ==, got.st_dev);
          g_assert_cmpint (expected.st_ino, ==, got.st_ino);
          options_handled++;
        }

      g_assert_cmpuint (g_variant_n_children (options_variant), ==, options_handled);
    }

  /* it carries stdin, stdout, stderr, and maybe fd 4 */
  g_assert_cmpuint (g_dbus_message_get_num_unix_fds (message), ==,
                    g_variant_n_children (fds_variant) +
                    n_fds_for_options);
  for (i = 0; i < g_variant_n_children (fds_variant) + n_fds_for_options; i++)
    g_assert_cmpint (fds[i], >=, 0);
  g_assert_cmpint (fds[i], ==, -1);

  if (f->config->dbus_call_fails)
    {
      g_subprocess_wait_check (f->flatpak_spawn, NULL, &error);
      g_assert_error (error, G_SPAWN_EXIT_ERROR, 1);
      g_clear_error (&error);
      g_test_minimized_result (g_test_timer_elapsed (),
                               "time to fail: %.1f",
                               g_test_timer_elapsed ());
      return;
    }

  if (config->host)
    {
      if (config->extra)
        {
          /* Pretend the command was killed by SIGSEGV and dumped core */
          g_dbus_connection_emit_signal (f->mock_development_conn,
                                         NULL,
                                         FLATPAK_SESSION_HELPER_PATH_DEVELOPMENT,
                                         FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT,
                                         "HostCommandExited",
                                         g_variant_new ("(uu)",
                                                        12345,
                                                        SIGSEGV | 0x80),
                                         &error);
          g_assert_no_error (error);

          g_subprocess_wait_check (f->flatpak_spawn, NULL, &error);
          g_assert_error (error, G_SPAWN_EXIT_ERROR, 128 + SIGSEGV);
          g_clear_error (&error);
        }
      else
        {
          /* Pretend the command exited with status 0 */
          g_dbus_connection_emit_signal (f->mock_development_conn,
                                         NULL,
                                         FLATPAK_SESSION_HELPER_PATH_DEVELOPMENT,
                                         FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT,
                                         "HostCommandExited",
                                         g_variant_new ("(uu)", 12345, 0),
                                         &error);
          g_assert_no_error (error);

          g_subprocess_wait_check (f->flatpak_spawn, NULL, &error);
          g_assert_no_error (error);
        }
    }
  else
    {
      if (config->extra)
        {
          /* Pretend the command exited with status 23 */
          g_dbus_connection_emit_signal (f->mock_portal_conn,
                                         NULL,
                                         FLATPAK_PORTAL_PATH,
                                         FLATPAK_PORTAL_INTERFACE,
                                         "SpawnExited",
                                         g_variant_new ("(uu)", 12345, (23 << 8)),
                                         &error);
          g_assert_no_error (error);

          g_subprocess_wait_check (f->flatpak_spawn, NULL, &error);
          g_assert_error (error, G_SPAWN_EXIT_ERROR, 23);
          g_clear_error (&error);
        }
      else
        {
          /* Pretend the command exited with status 0 */
          g_dbus_connection_emit_signal (f->mock_portal_conn,
                                         NULL,
                                         FLATPAK_PORTAL_PATH,
                                         FLATPAK_PORTAL_INTERFACE,
                                         "SpawnExited",
                                         g_variant_new ("(uu)", 12345, 0),
                                         &error);
          g_assert_no_error (error);

          g_subprocess_wait_check (f->flatpak_spawn, NULL, &error);
          g_assert_no_error (error);
        }
    }

  g_test_minimized_result (g_test_timer_elapsed (),
                           "time to succeed: %.1f",
                           g_test_timer_elapsed ());
}

static void
teardown (Fixture *f,
          gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;
  gpointer free_me;

  for (free_me = g_queue_pop_head (&f->invocations);
       free_me != NULL;
       free_me = g_queue_pop_head (&f->invocations))
    g_object_unref (free_me);

  if (f->mock_development_object != 0)
    g_dbus_connection_unregister_object (f->mock_development_conn,
                                         f->mock_development_object);

  if (f->mock_portal_object != 0)
    g_dbus_connection_unregister_object (f->mock_portal_conn,
                                         f->mock_portal_object);

  if (f->dbus_daemon != NULL)
    {
      g_subprocess_send_signal (f->dbus_daemon, SIGTERM);
      g_subprocess_wait (f->dbus_daemon, NULL, &error);
      g_assert_no_error (error);
    }

  g_clear_object (&f->dbus_daemon);
  g_clear_object (&f->flatpak_spawn);
  g_clear_object (&f->mock_development_conn);
  g_clear_object (&f->mock_portal_conn);
  g_free (f->dbus_address);
  g_free (f->flatpak_spawn_path);
  alarm (0);
}

static const Config subsandbox_fails =
{
  .dbus_call_fails = TRUE,
};

static const Config subsandbox_complex =
{
  .awkward_command_name = TRUE,
  .extra = TRUE,
  /* This is obviously not a realistic thing to put at /app, but it needs
   * to be something that will certainly exist on the host system! */
  .app_path = "/dev",
  /* Similar */
  .usr_path = "/",
};

static const Config subsandbox_empty_app =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_EMPTY_APP,
  .app_path = "",
};

static const Config subsandbox_clear_env =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_CLEAR_ENV,
};

static const Config subsandbox_latest =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_LATEST_VERSION,
};

static const Config subsandbox_no_net =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_NO_NETWORK,
};

static const Config subsandbox_watch_bus =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_WATCH_BUS,
};

static const Config subsandbox_expose_pids =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS,
  .portal_supports = FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS,
};

static const Config subsandbox_share_pids =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_SHARE_PIDS,
  .portal_supports = FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS,
};

static const Config subsandbox_sandbox_simple =
{
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_SANDBOX,
};

static const Config subsandbox_sandbox_complex =
{
  .sandbox_complex = TRUE,
  .subsandbox_flags = FLATPAK_SPAWN_FLAGS_SANDBOX,
  /* TODO: Exercise these separately */
  .subsandbox_sandbox_flags = (FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_DISPLAY |
                               FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_SOUND |
                               FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_GPU |
                               FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS |
                               FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_A11Y |
                               FLATPAK_SPAWN_SANDBOX_FLAGS_FUTURE),
};

static const Config host_simple =
{
  .host = TRUE,
};

static const Config host_fails =
{
  .dbus_call_fails = TRUE,
  .host = TRUE,
};

static const Config host_complex1 =
{
  .extra = TRUE,
  .host = TRUE,
  .host_flags = FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV,
};

static const Config host_complex2 =
{
  .awkward_command_name = TRUE,
  .extra = TRUE,
  .host = TRUE,
  .host_flags = FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS,
};

static const Config fail_invalid_env =
{
  .fails_immediately = 1,
  .extra_arg = "--env=",
};

static const Config fail_invalid_env2 =
{
  .fails_immediately = 1,
  .extra_arg = "--env=NOPE",
};

static const Config fail_invalid_fd =
{
  .fails_immediately = 1,
  .extra_arg = "--forward-fd=",
};

static const Config fail_invalid_fd2 =
{
  .fails_immediately = 1,
  .extra_arg = "--forward-fd=yesplease",
};

/* TODO: These should fail, but succeed */
#if 0
static const Config fail_invalid_fd3 =
{
  .fails_immediately = 1,
  .extra_arg = "--forward-fd=1a",
};

static const Config fail_invalid_fd4 =
{
  .fails_immediately = 1,
  .extra_arg = "--forward-fd=-1",
};
#endif

static const Config fail_invalid_sandbox_flag =
{
  .fails_immediately = 1,
  .extra_arg = "--sandbox-flag=tricolore",
};

static const Config fail_invalid_sandbox_flag2 =
{
  .fails_immediately = 1,
  .extra_arg = "--sandbox-flag=1e6",
};

static const Config fail_no_command =
{
  .fails_immediately = 1,
  .no_command = TRUE,
};

static const Config fail_no_session_bus =
{
  .fails_immediately = 1,
  .no_session_bus = TRUE,
};

static const Config fail_no_usr_path =
{
  .fails_after_version_check = 1,
  .usr_path = "",
};

static const Config fail_nonexistent_app_path =
{
  .fails_after_version_check = 1,
  .app_path = "/nonexistent",
};

static const Config fail_nonexistent_usr_path =
{
  .fails_after_version_check = 1,
  .usr_path = "/nonexistent",
};

static const Config host_cannot[] =
{
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--expose-pids" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--latest-version" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--no-network" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--sandbox" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--sandbox-expose=/" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--sandbox-expose-path=/" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--sandbox-expose-path-ro=/" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--sandbox-expose-ro=/" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--sandbox-flag=1" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--share-pids" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--app-path=" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--app-path=/" },
  { .fails_immediately = 1, .host = TRUE, .extra_arg = "--usr-path=/" },
};

int
main (int argc,
      char **argv)
{
  gsize i;

  g_test_init (&argc, &argv, NULL);

  g_test_add ("/help", Fixture, NULL, setup, test_help, teardown);

  g_test_add ("/host/simple", Fixture, &host_simple, setup, test_command, teardown);
  g_test_add ("/host/complex1", Fixture, &host_complex1, setup, test_command, teardown);
  g_test_add ("/host/complex2", Fixture, &host_complex2, setup, test_command, teardown);
  g_test_add ("/host/fails", Fixture, &host_fails, setup, test_command, teardown);

  g_test_add ("/subsandbox/simple", Fixture, &default_config, setup, test_command, teardown);
  g_test_add ("/subsandbox/clear-env", Fixture, &subsandbox_clear_env, setup, test_command, teardown);
  g_test_add ("/subsandbox/complex", Fixture, &subsandbox_complex, setup, test_command, teardown);
  g_test_add ("/subsandbox/empty_app", Fixture, &subsandbox_empty_app, setup, test_command, teardown);
  g_test_add ("/subsandbox/expose-pids", Fixture, &subsandbox_expose_pids, setup, test_command, teardown);
  g_test_add ("/subsandbox/fails", Fixture, &subsandbox_fails, setup, test_command, teardown);
  g_test_add ("/subsandbox/latest", Fixture, &subsandbox_latest, setup, test_command, teardown);
  g_test_add ("/subsandbox/no-net", Fixture, &subsandbox_no_net, setup, test_command, teardown);
  g_test_add ("/subsandbox/sandbox/simple", Fixture, &subsandbox_sandbox_simple, setup, test_command, teardown);
  g_test_add ("/subsandbox/sandbox/complex", Fixture, &subsandbox_sandbox_complex, setup, test_command, teardown);
  g_test_add ("/subsandbox/share-pids", Fixture, &subsandbox_share_pids, setup, test_command, teardown);
  g_test_add ("/subsandbox/watch-bus", Fixture, &subsandbox_watch_bus, setup, test_command, teardown);

  g_test_add ("/fail/invalid-env", Fixture, &fail_invalid_env, setup, test_command, teardown);
  g_test_add ("/fail/invalid-env2", Fixture, &fail_invalid_env2, setup, test_command, teardown);
  g_test_add ("/fail/invalid-fd", Fixture, &fail_invalid_fd, setup, test_command, teardown);
  g_test_add ("/fail/invalid-fd2", Fixture, &fail_invalid_fd2, setup, test_command, teardown);
  g_test_add ("/fail/invalid-sandbox-flag", Fixture, &fail_invalid_sandbox_flag, setup, test_command, teardown);
  g_test_add ("/fail/invalid-sandbox-flag2", Fixture, &fail_invalid_sandbox_flag2, setup, test_command, teardown);
  g_test_add ("/fail/no-command", Fixture, &fail_no_command, setup, test_command, teardown);
  g_test_add ("/fail/no-session-bus", Fixture, &fail_no_session_bus, setup, test_command, teardown);
  g_test_add ("/fail/no-usr-path", Fixture, &fail_no_usr_path, setup, test_command, teardown);
  g_test_add ("/fail/nonexistent-app-path", Fixture, &fail_nonexistent_app_path, setup, test_command, teardown);
  g_test_add ("/fail/nonexistent-usr-path", Fixture, &fail_nonexistent_usr_path, setup, test_command, teardown);

  for (i = 0; i < G_N_ELEMENTS (host_cannot); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fail/host-cannot/%s",
                                                host_cannot[i].extra_arg);

      g_strdelimit (name + strlen ("/fail/host-cannot/"), "/", 'x');

      g_test_add (name, Fixture, &host_cannot[i], setup, test_command, teardown);
    }

  return g_test_run ();
}
