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
#define PORTAL_IFACE_NAME "org.freedesktop.portal.Email"

typedef struct
{
  GSubprocess *dbus_daemon;
  gchar *dbus_address;
  GSubprocess *xdg_email;
  gchar *xdg_email_path;
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

static GDBusArgInfo arg_options =
{
  -1,
  "options",
  "a{sv}",
  NULL  /* annotations */
};

static GDBusArgInfo arg_out_handle =
{
  -1,
  "handle",
  "o",
  NULL  /* annotations */
};

static GDBusArgInfo *in_args[] =
{
  &arg_parent_window,
  &arg_options,
  NULL
};

static GDBusArgInfo *out_args[] =
{
  &arg_out_handle,
  NULL
};

static GDBusMethodInfo compose_email_info =
{
  -1,
  "ComposeEmail",
  in_args,
  out_args,
  NULL  /* annotations */
};

static GDBusMethodInfo *method_info[] =
{
  &compose_email_info,
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

  f->xdg_email_path = g_strdup (g_getenv ("XDG_EMAIL"));

  if (f->xdg_email_path == NULL)
    f->xdg_email_path = g_strdup (BINDIR "/xdg-email");

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

  f->xdg_email = g_subprocess_launcher_spawn (launcher, &error,
                                              f->xdg_email_path,
                                              "--help",
                                              NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->xdg_email);

  g_subprocess_communicate_utf8 (f->xdg_email, NULL, NULL, &stdout_buf,
                                 &stderr_buf, &error);
  g_assert_cmpstr (stderr_buf, ==, "");
  g_assert_nonnull (stdout_buf);
  g_test_message ("xdg-open --help: %s", stdout_buf);
  g_assert_true (strstr (stdout_buf, "--version") != NULL);

  g_subprocess_wait_check (f->xdg_email, NULL, &error);
  g_assert_no_error (error);
}

static void
test_minimal (Fixture *f,
              gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  g_autoptr(GVariant) asv = NULL;
  g_autoptr(GVariantDict) dict = NULL;
  GVariant *parameters;
  const gchar *window;
  const gchar *address;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_address,
                                TRUE);

  f->xdg_email = g_subprocess_launcher_spawn (launcher, &error,
                                             f->xdg_email_path,
                                             "me@example.com",
                                             NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->xdg_email);

  while (g_queue_get_length (&f->invocations) < 1)
    g_main_context_iteration (NULL, TRUE);

  g_subprocess_wait_check (f->xdg_email, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (g_queue_get_length (&f->invocations), ==, 1);
  invocation = g_queue_pop_head (&f->invocations);

  g_assert_cmpstr (g_dbus_method_invocation_get_interface_name (invocation),
                   ==, PORTAL_IFACE_NAME);
  g_assert_cmpstr (g_dbus_method_invocation_get_method_name (invocation),
                   ==, "ComposeEmail");

  parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_assert_cmpstr (g_variant_get_type_string (parameters), ==, "(sa{sv})");
  g_variant_get (parameters, "(&s@a{sv})",
                 &window, &asv);
  g_assert_cmpstr (window, ==, "");

  dict = g_variant_dict_new (asv);
  g_assert_true (g_variant_dict_lookup (dict, "address", "&s", &address));
  g_assert_cmpstr (address, ==, "me@example.com");
  g_assert_false (g_variant_dict_contains (dict, "subject"));
  g_assert_false (g_variant_dict_contains (dict, "body"));
  g_assert_false (g_variant_dict_contains (dict, "attachments"));
}

static void
test_maximal (Fixture *f,
              gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  g_autoptr(GVariant) asv = NULL;
  g_autoptr(GVariantDict) dict = NULL;
  g_autoptr(GVariant) attachments = NULL;
  GVariant *parameters;
  const gchar *window;
  const gchar *address;
  const gchar *subject;
  const gchar *body;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_address,
                                TRUE);

  f->xdg_email = g_subprocess_launcher_spawn (launcher, &error,
                                             f->xdg_email_path,
                                             "--subject", "Make Money Fast",
                                             "--body", "Your spam here",
                                             "--attach", "/dev/null",
                                             "me@example.com",
                                             NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->xdg_email);

  while (g_queue_get_length (&f->invocations) < 1)
    g_main_context_iteration (NULL, TRUE);

  g_subprocess_wait_check (f->xdg_email, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (g_queue_get_length (&f->invocations), ==, 1);
  invocation = g_queue_pop_head (&f->invocations);

  g_assert_cmpstr (g_dbus_method_invocation_get_interface_name (invocation),
                   ==, PORTAL_IFACE_NAME);
  g_assert_cmpstr (g_dbus_method_invocation_get_method_name (invocation),
                   ==, "ComposeEmail");

  parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_assert_cmpstr (g_variant_get_type_string (parameters), ==, "(sa{sv})");
  g_variant_get (parameters, "(&s@a{sv})",
                 &window, &asv);
  g_assert_cmpstr (window, ==, "");

  dict = g_variant_dict_new (asv);
  g_assert_true (g_variant_dict_lookup (dict, "address", "&s", &address));
  g_assert_cmpstr (address, ==, "me@example.com");
  g_assert_true (g_variant_dict_lookup (dict, "subject", "&s", &subject));
  g_assert_cmpstr (subject, ==, "Make Money Fast");
  g_assert_true (g_variant_dict_lookup (dict, "body", "&s", &body));
  g_assert_cmpstr (body, ==, "Your spam here");
  /* TODO: Also test that the attachment went through correctly (this
   * doesn't seem to work at the moment) */
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
  g_clear_object (&f->xdg_email);
  g_clear_object (&f->mock_conn);
  g_free (f->dbus_address);
  g_free (f->xdg_email_path);
  alarm (0);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/help", Fixture, NULL, setup, test_help, teardown);
  g_test_add ("/minimal", Fixture, NULL, setup, test_minimal, teardown);
  g_test_add ("/maximal", Fixture, NULL, setup, test_maximal, teardown);

  return g_test_run ();
}
