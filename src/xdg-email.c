/*
 * Copyright © 2017 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Florian Müllner <fmuellner@gnome.org>
 */

#include <gio/gio.h>

#include <glib/gi18n.h>
#include <gio/gunixfdlist.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define PORTAL_BUS_NAME    "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE_NAME  "org.freedesktop.portal.Email"

static char **uris = NULL;
static gboolean show_help = FALSE;
static gboolean show_version = FALSE;

static gboolean use_utf8 = FALSE;
static char *subject = NULL;
static char *body = NULL;
static char *attach = NULL;

static gboolean
warn_ignored_parameter (const char  *option_name,
                        const char  *value,
                        gpointer     data,
                        GError     **error);

static GOptionEntry entries[] = {
  {"utf8", 0, 0, G_OPTION_ARG_NONE, &use_utf8,
   N_("Indicates that all command line options are in utf8"), NULL },
  { "cc", 0, 0, G_OPTION_ARG_CALLBACK, warn_ignored_parameter,
    N_("Specify a recipient to be copied on the e-mail"), N_("address")},
  { "bcc", 0, 0, G_OPTION_ARG_CALLBACK, warn_ignored_parameter,
    N_("Specify a recipient to be blindly copied on the e-mail"), N_("address")},
  { "subject", 0, 0, G_OPTION_ARG_STRING, &subject,
    N_("Specify a subject for the e-mail"), N_("text")},
  { "body", 0, 0, G_OPTION_ARG_STRING, &body,
    N_("Specify a body for the e-mail"), N_("text")},
  { "attach", 0, 0, G_OPTION_ARG_FILENAME, &attach,
    N_("Specify an attachment for the e-mail"), N_("file")},

  /* Compat options with "real" xdg-open */
  { "manual", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &show_help, NULL, NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Show program version"), NULL },

  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &uris, NULL, NULL }
};

static gboolean
warn_ignored_parameter (const char  *option_name,
                        const char  *value,
                        gpointer     data,
                        GError     **error)
{
  g_printerr ("Option %s is not supported, ignoring value \"%s\"\n",
              option_name, value);
  return TRUE;
}

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;
  GDBusConnection *connection;
  GVariantBuilder opt_builder;
  GVariant *parameters;
  GUnixFDList *fd_list = NULL;

  context = g_option_context_new ("[ mailto-uri | address(es) ]");

  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_parse (context, &argc, &argv, &error);

  if (error != NULL)
    {
      g_printerr ("Error parsing commandline options: %s\n", error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.\n", g_get_prgname ());

      g_error_free (error);
      return 1;
    }

  if (show_version)
    {
      g_print ("%s\n", PACKAGE_VERSION);

      return 0;
    }

  if (show_help || uris == NULL || g_strv_length (uris) > 1)
    {
      char *help = g_option_context_get_help (context, TRUE, NULL);
      g_print ("%s\n", help);

      g_free (help);
      return 0;
    }

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);

  if (connection == NULL)
    {
      if (error)
        g_printerr ("Failed to connect to session bus: %s", error->message);
      else
        g_printerr ("Failed to connect to session bus");

      g_clear_pointer (&error, g_error_free);
      return 3;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_variant_builder_add (&opt_builder,
                         "{sv}",
                         "address", g_variant_new_string (*uris));

  if (subject != NULL)
    g_variant_builder_add (&opt_builder,
                           "{sv}",
                           "subject", g_variant_new_string (subject));

  if (body != NULL)
    g_variant_builder_add (&opt_builder,
                           "{sv}",
                           "body", g_variant_new_string (body));

  if (attach != NULL)
    {
      GFile *file = g_file_new_for_commandline_arg (attach);
      char *path;
      int fd;

      if (!g_file_is_native (file))
        {
          g_printerr ("Only native files can be used as attachments");
          g_object_unref (file);
          return 2;
        }

      path = g_file_get_path (file);
      fd = open (path, O_PATH | O_CLOEXEC);
      if (fd == -1)
        {
          g_printerr ("Failed to open '%s': %s", path, g_strerror (errno));
          return 2;
        }

      fd_list = g_unix_fd_list_new_from_array (&fd, 1);
      fd = -1;

      g_variant_builder_add (&opt_builder,
                             "{sv}",
                             "attachments", g_variant_new_parsed ("[0]"));
    }

  parameters = g_variant_new ("(s@a{sv})",
                              "",
                              g_variant_builder_end (&opt_builder));

  g_dbus_connection_call_with_unix_fd_list_sync (connection,
                                                 PORTAL_BUS_NAME,
                                                 PORTAL_OBJECT_PATH,
                                                 PORTAL_IFACE_NAME,
                                                 "ComposeEmail",
                                                 parameters,
                                                 NULL,
                                                 G_DBUS_CALL_FLAGS_NONE,
                                                 -1,
                                                 fd_list,
                                                 NULL,
                                                 NULL,
                                                 &error);

  if (error)
    {
      g_printerr ("Failed to call portal: %s\n", error->message);

      g_object_unref (connection);
      g_error_free (error);

      return 4;
    }

  g_object_unref (connection);

  return 0;
}
