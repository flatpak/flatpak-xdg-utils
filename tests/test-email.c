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
#define PORTAL_IFACE_NAME_OPENURI "org.freedesktop.portal.OpenURI"

typedef struct
{
  guint32 iface_version;
} Config;

typedef struct
{
  const Config *config;
  GSubprocess *dbus_daemon;
  gchar *dbus_address;
  GSubprocess *xdg_email;
  gchar *xdg_email_path;
  GDBusConnection *mock_conn;
  guint mock_object;
  guint mock_openuri_object;
  GQueue invocations;
} Fixture;

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

  if (g_strcmp0 (interface_name, "org.freedesktop.portal.Email") == 0 &&
      g_strcmp0 (method_name, "ComposeEmail") == 0)
    {
      /* OK */
    }
  else if (g_strcmp0 (interface_name, "org.freedesktop.portal.OpenURI") == 0 &&
           g_strcmp0 (method_name, "OpenURI") == 0)
    {
      /* OK */
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Not ComposeEmail or OpenURI");
      return;
    }

  g_queue_push_tail (&f->invocations, g_object_ref (invocation));
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", "/foo"));
}

GVariant *
mock_getter (GDBusConnection *conn G_GNUC_UNUSED,
             const gchar *sender G_GNUC_UNUSED,
             const gchar *object_path G_GNUC_UNUSED,
             const gchar *interface_name,
             const gchar *property_name,
             GError **error,
             gpointer user_data)
{
  Fixture *f = user_data;

  g_test_message ("Get property: %s.%s", interface_name, property_name);

  if (g_strcmp0 (interface_name, "org.freedesktop.portal.Email") == 0 &&
      g_strcmp0 (property_name, "version") == 0)
    {
      return g_variant_new_uint32 (f->config->iface_version);
    }
  else
    {
#if GLIB_CHECK_VERSION(2, 42, 0)
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                   "Unknown property");
#else
      /* This is the closest we can do in older versions */
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                   "Unknown property");
#endif
      return NULL;
    }
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

static GDBusArgInfo arg_uri =
{
  -1,
  "uri",
  "s",
  NULL  /* annotations */
};

static GDBusArgInfo *openuri_in_args[] =
{
  &arg_parent_window,
  &arg_uri,
  &arg_options,
  NULL
};

static GDBusMethodInfo openuri_info =
{
  -1,
  "OpenURI",
  openuri_in_args,
  out_args,
  NULL  /* annotations */
};

static GDBusMethodInfo *openuri_method_info[] =
{
  &openuri_info,
  NULL
};

static GDBusPropertyInfo prop_version_info =
{
  .ref_count = -1,
  .name = "version",
  .signature = "u",
  .flags = G_DBUS_PROPERTY_INFO_FLAGS_READABLE,
  .annotations = NULL
};

static GDBusPropertyInfo *props_info[] =
{
  &prop_version_info,
  NULL
};

static GDBusInterfaceInfo iface_info =
{
  -1,
  PORTAL_IFACE_NAME,
  method_info,
  NULL, /* signals */
  props_info,
  NULL  /* annotations */
};

static GDBusInterfaceInfo v0_iface_info =
{
  .ref_count = -1,
  .name = PORTAL_IFACE_NAME,
  .methods = method_info,
  .signals = NULL,
  .properties = NULL,
  .annotations = NULL
};

static GDBusInterfaceInfo openuri_iface_info =
{
  .ref_count = -1,
  .name = PORTAL_IFACE_NAME_OPENURI,
  .methods = openuri_method_info,
  .signals = NULL,
  .properties = NULL,
  .annotations = NULL
};

static const GDBusInterfaceVTable vtable =
{
  mock_method_call,
  mock_getter,
  NULL  /* set */
};

static void
setup (Fixture *f,
       gconstpointer context)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceInfo *chosen_iface_info;

  f->config = context;

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

  if (f->config->iface_version == 0)
    chosen_iface_info = &v0_iface_info;
  else
    chosen_iface_info = &iface_info;

  f->mock_object = g_dbus_connection_register_object (f->mock_conn,
                                                      PORTAL_OBJECT_PATH,
                                                      chosen_iface_info,
                                                      &vtable,
                                                      f,
                                                      NULL,
                                                      &error);
  g_assert_no_error (error);
  g_assert_cmpuint (f->mock_object, !=, 0);

  f->mock_openuri_object = g_dbus_connection_register_object (f->mock_conn,
                                                              PORTAL_OBJECT_PATH,
                                                              &openuri_iface_info,
                                                              &vtable,
                                                              f,
                                                              NULL,
                                                              &error);
  g_assert_no_error (error);
  g_assert_cmpuint (f->mock_openuri_object, !=, 0);

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
  const gchar **addresses;
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

  if (f->config->iface_version >= 3)
    {
      g_assert_true (g_variant_dict_lookup (dict, "addresses", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "me@example.com");
      g_assert_cmpstr (addresses[1], ==, NULL);
      g_free (addresses);
    }
  else
    {
      g_assert_true (g_variant_dict_lookup (dict, "address", "&s", &address));
      g_assert_cmpstr (address, ==, "me@example.com");
    }

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
  const gchar **addresses;
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
                                             "--cc", "us@example.com",
                                             "--cc", "them@example.com",
                                             "--bcc", "hidden@example.com",
                                             "--bcc", "secret@example.com",
                                             "me@example.com",
                                             "you@example.com",
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

  if (f->config->iface_version >= 3)
    {
      g_assert_true (g_variant_dict_lookup (dict, "addresses", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "me@example.com");
      g_assert_cmpstr (addresses[1], ==, "you@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);

      g_assert_true (g_variant_dict_lookup (dict, "cc", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "us@example.com");
      g_assert_cmpstr (addresses[1], ==, "them@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);

      g_assert_true (g_variant_dict_lookup (dict, "bcc", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "hidden@example.com");
      g_assert_cmpstr (addresses[1], ==, "secret@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);
    }
  else
    {
      /* all addresses except the first are ignored */
      g_assert_true (g_variant_dict_lookup (dict, "address", "&s", &address));
      g_assert_cmpstr (address, ==, "me@example.com");
    }

  g_assert_true (g_variant_dict_lookup (dict, "subject", "&s", &subject));
  g_assert_cmpstr (subject, ==, "Make Money Fast");
  g_assert_true (g_variant_dict_lookup (dict, "body", "&s", &body));
  g_assert_cmpstr (body, ==, "Your spam here");
  /* TODO: Also test that the attachment went through correctly (this
   * doesn't seem to work at the moment) */
}

static void
test_mailto_none (Fixture *f,
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
                                              "mailto:?cc=one@example.com&bcc=two@example.com",
                                              "mailto:?none-here-either=true",
                                              NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->xdg_email);

  g_subprocess_communicate_utf8 (f->xdg_email, NULL, NULL, &stdout_buf,
                                 &stderr_buf, &error);
  g_test_message ("%s", stderr_buf);
  g_assert_nonnull (strstr (stderr_buf, "No valid addresses found"));

  g_subprocess_wait_check (f->xdg_email, NULL, &error);
  g_assert_error (error, G_SPAWN_EXIT_ERROR, 1);
}

static void
test_mailto_single (Fixture *f,
                    gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  g_autoptr(GVariant) asv = NULL;
  GVariant *parameters;
  const gchar *window;
  const gchar *uri;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_address,
                                TRUE);

  f->xdg_email = g_subprocess_launcher_spawn (launcher, &error,
                                              f->xdg_email_path,
                                              /* Deliberarely not RFC 6068
                                               * compliant, to check that
                                               * we pass it through without
                                               * parsing or understanding it */
                                              "MailTo:?you-are-not-expected-to-understand-this",
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
                   ==, PORTAL_IFACE_NAME_OPENURI);
  g_assert_cmpstr (g_dbus_method_invocation_get_method_name (invocation),
                   ==, "OpenURI");

  parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_assert_cmpstr (g_variant_get_type_string (parameters), ==, "(ssa{sv})");
  g_variant_get (parameters, "(&s&s@a{sv})",
                 &window, &uri, &asv);
  g_assert_cmpstr (window, ==, "");
  g_assert_cmpstr (uri, ==, "MailTo:?you-are-not-expected-to-understand-this");
}

static void
test_mailto_multiple (Fixture *f,
                      gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  g_autoptr(GVariant) asv = NULL;
  g_autoptr(GVariantDict) dict = NULL;
  GVariant *parameters;
  const gchar *window;
  const gchar **addresses;
  const gchar *address;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_address,
                                TRUE);

  f->xdg_email = g_subprocess_launcher_spawn (launcher, &error,
                                              f->xdg_email_path,
                                              "mailto:me@example.com",
                                              "mailto:you@example.com",
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

  if (f->config->iface_version >= 3)
    {
      g_assert_true (g_variant_dict_lookup (dict, "addresses", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "me@example.com");
      g_assert_cmpstr (addresses[1], ==, "you@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);
    }
  else
    {
      g_assert_true (g_variant_dict_lookup (dict, "address", "&s", &address));
      g_assert_cmpstr (address, ==, "me@example.com");
    }

  g_assert_false (g_variant_dict_contains (dict, "subject"));
  g_assert_false (g_variant_dict_contains (dict, "body"));
  g_assert_false (g_variant_dict_contains (dict, "attachments"));
}

static void
test_mailto_complex (Fixture *f,
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
  const gchar **addresses;
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
                                              "mailto:nobody@example.com",
                                              (
                                                "mailto:"
                                                  "me@example.com"
                                                  ","
                                                  "you@example.com"
                                                "?"
                                                  "subject=Make%20Money%20Fast"
                                                "&"
                                                  "body=Your%20spam%20here"
                                                "&"
                                                  "cc="
                                                    "us@example.com"
                                                    ","
                                                    "them@example.com"
                                                "&"
                                                  "Bcc="
                                                    "hidden@example.com"
                                                    ","
                                                    "secret@example.com"
                                                "&"
                                                  "Precedence=bulk"
                                                "&"
                                                  "X-Mailer=xdg-email"
                                              ),
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

  if (f->config->iface_version >= 3)
    {
      g_assert_true (g_variant_dict_lookup (dict, "addresses", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "nobody@example.com");
      g_assert_cmpstr (addresses[1], ==, "me@example.com");
      g_assert_cmpstr (addresses[2], ==, "you@example.com");
      g_assert_cmpstr (addresses[3], ==, NULL);
      g_free (addresses);

      g_assert_true (g_variant_dict_lookup (dict, "cc", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "us@example.com");
      g_assert_cmpstr (addresses[1], ==, "them@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);

      g_assert_true (g_variant_dict_lookup (dict, "bcc", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "hidden@example.com");
      g_assert_cmpstr (addresses[1], ==, "secret@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);
    }
  else
    {
      /* all addresses except the first are ignored */
      g_assert_true (g_variant_dict_lookup (dict, "address", "&s", &address));
      g_assert_cmpstr (address, ==, "nobody@example.com");
    }

  g_assert_true (g_variant_dict_lookup (dict, "subject", "&s", &subject));
  g_assert_cmpstr (subject, ==, "Make Money Fast");
  g_assert_true (g_variant_dict_lookup (dict, "body", "&s", &body));
  g_assert_cmpstr (body, ==, "Your spam here");
}

static void
test_mailto_combined (Fixture *f,
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
  const gchar **addresses;
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
                                              "--cc", "us@example.com",
                                              "--bcc", "hidden@example.com",
                                              "--subject", "ignored",
                                              "--body", "ignored",
                                              "me@example.com",
                                              (
                                                "mailto:"
                                                  "you@example.com"
                                                "?"
                                                  "Precedence=bulk"
                                                "&"
                                                  "X-Mailer=xdg-email"
                                                "&"
                                                  "subject=Make%20Money%20Fast"
                                                "&"
                                                  "body=Your%20spam%20here"
                                                "&"
                                                  "cc="
                                                    "them@example.com"
                                                "&"
                                                  "Bcc="
                                                    "secret@example.com"
                                              ),
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

  if (f->config->iface_version >= 3)
    {
      g_assert_true (g_variant_dict_lookup (dict, "addresses", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "me@example.com");
      g_assert_cmpstr (addresses[1], ==, "you@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);

      g_assert_true (g_variant_dict_lookup (dict, "cc", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "us@example.com");
      g_assert_cmpstr (addresses[1], ==, "them@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);

      g_assert_true (g_variant_dict_lookup (dict, "bcc", "^a&s", &addresses));
      g_assert_cmpstr (addresses[0], ==, "hidden@example.com");
      g_assert_cmpstr (addresses[1], ==, "secret@example.com");
      g_assert_cmpstr (addresses[2], ==, NULL);
      g_free (addresses);
    }
  else
    {
      /* all addresses except the first are ignored */
      g_assert_true (g_variant_dict_lookup (dict, "address", "&s", &address));
      g_assert_cmpstr (address, ==, "me@example.com");
    }

  g_assert_true (g_variant_dict_lookup (dict, "subject", "&s", &subject));
  g_assert_cmpstr (subject, ==, "Make Money Fast");
  g_assert_true (g_variant_dict_lookup (dict, "body", "&s", &body));
  g_assert_cmpstr (body, ==, "Your spam here");
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

static const Config v0 =
{
  .iface_version = 0,
};

static const Config v1 =
{
  .iface_version = 1,
};

static const Config v3 =
{
  .iface_version = 3,
};

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/help", Fixture, &v0, setup, test_help, teardown);
  g_test_add ("/minimal/v0", Fixture, &v0, setup, test_minimal, teardown);
  g_test_add ("/minimal/v1", Fixture, &v1, setup, test_minimal, teardown);
  g_test_add ("/minimal/v3", Fixture, &v3, setup, test_minimal, teardown);
  g_test_add ("/maximal/v0", Fixture, &v0, setup, test_maximal, teardown);
  g_test_add ("/maximal/v1", Fixture, &v1, setup, test_maximal, teardown);
  g_test_add ("/maximal/v3", Fixture, &v3, setup, test_maximal, teardown);
  g_test_add ("/mailto/none", Fixture, &v0, setup, test_mailto_none, teardown);
  g_test_add ("/mailto/single", Fixture, &v3, setup, test_mailto_single, teardown);
  g_test_add ("/mailto/multiple", Fixture, &v3, setup, test_mailto_multiple, teardown);
  g_test_add ("/mailto/complex", Fixture, &v3, setup, test_mailto_complex, teardown);
  g_test_add ("/mailto/combined", Fixture, &v3, setup, test_mailto_combined, teardown);

  return g_test_run ();
}
