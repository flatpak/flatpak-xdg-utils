/*
 * Copyright © 2018-2019 Collabora Ltd.
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

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE_NAME "org.freedesktop.portal.OpenURI"

typedef struct
{
  GSubprocess *dbus_daemon;
  gchar *dbus_address;
  GSubprocess *xdg_open;
  gchar *xdg_open_path;
  GDBusConnection *mock_conn;
  guint mock_object;
  GQueue invocations;
} Fixture;

typedef struct
{
  int dummy;
} Config;

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
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", "/foo"));
}

static GDBusArgInfo arg_parent_window =
{
  -1,
  "parent_window",
  "s",
  NULL  /* annotations */
};

static GDBusArgInfo arg_uri =
{
  -1,
  "uri",
  "s",
  NULL  /* annotations */
};

static GDBusArgInfo arg_options =
{
  -1,
  "options",
  "a{sv}",
  NULL  /* annotations */
};

static GDBusArgInfo arg_fd =
{
  -1,
  "fd",
  "h",
  NULL  /* annotations */
};

static GDBusArgInfo arg_out_handle =
{
  -1,
  "handle",
  "o",
  NULL  /* annotations */
};

static GDBusArgInfo *open_uri_in_args[] =
{
  &arg_parent_window,
  &arg_uri,
  &arg_options,
  NULL
};

static GDBusArgInfo *open_uri_out_args[] =
{
  &arg_out_handle,
  NULL
};

static GDBusMethodInfo open_uri_info =
{
  -1,
  "OpenURI",
  open_uri_in_args,
  open_uri_out_args,
  NULL  /* annotations */
};

static GDBusArgInfo *open_file_in_args[] =
{
  &arg_parent_window,
  &arg_fd,
  &arg_options,
  NULL
};

static GDBusMethodInfo open_file_info =
{
  -1,
  "OpenFile",
  open_file_in_args,
  open_uri_out_args,   /* the same */
  NULL  /* annotations */
};

static GDBusMethodInfo *method_info[] =
{
  &open_uri_info,
  &open_file_info,
  NULL
};

static GDBusInterfaceInfo iface_info =
{
  -1,
  PORTAL_IFACE_NAME,
  method_info,
  NULL, /* signals */
  NULL, /* properties */
  NULL  /* annotations */
};

static const GDBusInterfaceVTable vtable =
{
  mock_method_call,
  NULL, /* get */
  NULL  /* set */
};

static void
setup (Fixture *f,
       gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;

  g_queue_init (&f->invocations);

  setup_dbus_daemon (&f->dbus_daemon, &f->dbus_address);

  f->xdg_open_path = g_strdup (g_getenv ("XDG_OPEN"));

  if (f->xdg_open_path == NULL)
    f->xdg_open_path = g_strdup (BINDIR "/xdg-open");

  f->mock_conn = g_dbus_connection_new_for_address_sync (f->dbus_address,
                                                         (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                         NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->mock_conn);

  f->mock_object = g_dbus_connection_register_object (f->mock_conn,
                                                      PORTAL_OBJECT_PATH,
                                                      &iface_info,
                                                      &vtable,
                                                      f,
                                                      NULL,
                                                      &error);
  g_assert_no_error (error);
  g_assert_cmpuint (f->mock_object, !=, 0);

  own_name_sync (f->mock_conn, PORTAL_BUS_NAME);
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

  f->xdg_open = g_subprocess_launcher_spawn (launcher, &error,
                                             f->xdg_open_path,
                                             "--help",
                                             NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->xdg_open);

  g_subprocess_communicate_utf8 (f->xdg_open, NULL, NULL, &stdout_buf,
                                 &stderr_buf, &error);
  g_assert_cmpstr (stderr_buf, ==, "");
  g_assert_nonnull (stdout_buf);
  g_test_message ("xdg-open --help: %s", stdout_buf);
  g_assert_true (strstr (stdout_buf, "--version") != NULL);

  g_subprocess_wait_check (f->xdg_open, NULL, &error);
  g_assert_no_error (error);
}

static void
test_uri (Fixture *f,
          gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  GVariant *parameters;
  const gchar *window;
  const gchar *uri;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_address,
                                TRUE);

  f->xdg_open = g_subprocess_launcher_spawn (launcher, &error,
                                             f->xdg_open_path,
                                             "http://example.com/",
                                             NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->xdg_open);

  while (g_queue_get_length (&f->invocations) < 1)
    g_main_context_iteration (NULL, TRUE);

  g_subprocess_wait_check (f->xdg_open, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (g_queue_get_length (&f->invocations), ==, 1);
  invocation = g_queue_pop_head (&f->invocations);

  g_assert_cmpstr (g_dbus_method_invocation_get_interface_name (invocation),
                   ==, PORTAL_IFACE_NAME);
  g_assert_cmpstr (g_dbus_method_invocation_get_method_name (invocation),
                   ==, "OpenURI");

  parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_assert_cmpstr (g_variant_get_type_string (parameters), ==, "(ssa{sv})");
  g_variant_get (parameters, "(&s&sa{sv})",
                 &window, &uri, NULL);
  g_assert_cmpstr (window, ==, "");
  g_assert_cmpstr (uri, ==, "http://example.com/");
}

static void
test_file (Fixture *f,
           gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  GVariant *parameters;
  const gchar *window;
  gint32 handle;
  GDBusMessage *message;
  GUnixFDList *fd_list;
  const int *fds;
  struct stat ours, theirs;

  if (stat ("/dev/null", &ours) < 0)
    g_error ("stat(/dev/null): %s", g_strerror (errno));

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_address,
                                TRUE);

  f->xdg_open = g_subprocess_launcher_spawn (launcher, &error,
                                             f->xdg_open_path,
                                             "/dev/null",
                                             NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->xdg_open);

  while (g_queue_get_length (&f->invocations) < 1)
    g_main_context_iteration (NULL, TRUE);

  g_subprocess_wait_check (f->xdg_open, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (g_queue_get_length (&f->invocations), ==, 1);
  invocation = g_queue_pop_head (&f->invocations);

  g_assert_cmpstr (g_dbus_method_invocation_get_interface_name (invocation),
                   ==, PORTAL_IFACE_NAME);
  g_assert_cmpstr (g_dbus_method_invocation_get_method_name (invocation),
                   ==, "OpenFile");

  parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_assert_cmpstr (g_variant_get_type_string (parameters), ==, "(sha{sv})");
  g_variant_get (parameters, "(&sha{sv})",
                 &window, &handle, NULL);
  g_assert_cmpstr (window, ==, "");
  g_assert_cmpint (handle, ==, 0);

  message = g_dbus_method_invocation_get_message (invocation);
  g_assert_cmpuint (g_dbus_message_get_num_unix_fds (message), ==, 1);
  fd_list = g_dbus_message_get_unix_fd_list (message);
  fds = g_unix_fd_list_peek_fds (fd_list, NULL);
  g_assert_cmpint (fds[0], >=, 0);
  g_assert_cmpint (fds[1], ==, -1);

  if (fstat (fds[0], &theirs) < 0)
    g_error ("stat(their fd): %s", g_strerror (errno));

  /* It's really /dev/null */
  g_assert_cmpuint (ours.st_dev, ==, theirs.st_dev);
  g_assert_cmpuint (ours.st_ino, ==, theirs.st_ino);
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

  if (f->mock_object != 0)
    g_dbus_connection_unregister_object (f->mock_conn, f->mock_object);

  if (f->dbus_daemon != NULL)
    {
      g_subprocess_send_signal (f->dbus_daemon, SIGTERM);
      g_subprocess_wait (f->dbus_daemon, NULL, &error);
      g_assert_no_error (error);
    }

  g_clear_object (&f->dbus_daemon);
  g_clear_object (&f->xdg_open);
  g_clear_object (&f->mock_conn);
  g_free (f->dbus_address);
  g_free (f->xdg_open_path);
  alarm (0);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/help", Fixture, NULL, setup, test_help, teardown);
  g_test_add ("/uri", Fixture, NULL, setup, test_uri, teardown);
  g_test_add ("/file", Fixture, NULL, setup, test_file, teardown);

  return g_test_run ();
}
