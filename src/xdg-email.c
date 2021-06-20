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

#include "backport-autoptr.h"

#define PORTAL_BUS_NAME    "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE_NAME  "org.freedesktop.portal.Email"
#define PORTAL_IFACE_NAME_OPENURI "org.freedesktop.portal.OpenURI"

static char **addresses = NULL;
static char **opt_cc = NULL;
static char **opt_bcc = NULL;
static gboolean show_help = FALSE;
static gboolean show_version = FALSE;

static gboolean use_utf8 = FALSE;
static char *subject = NULL;
static char *body = NULL;
static char *attach = NULL;

static GOptionEntry entries[] = {
  {"utf8", 0, 0, G_OPTION_ARG_NONE, &use_utf8,
   N_("Indicates that all command line options are in utf8"), NULL },
  { "cc", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cc,
    N_("Specify a recipient to be copied on the e-mail"), N_("address")},
  { "bcc", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_bcc,
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

  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &addresses, NULL, NULL },
  { NULL, 0, 0, 0, NULL, NULL, NULL }
};

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;
  GDBusConnection *connection;
  GVariantBuilder opt_builder;
  GVariant *parameters;
  GUnixFDList *fd_list = NULL;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GVariant) v = NULL;
  guint32 version = 0;
  g_autoptr(GPtrArray) to = NULL;
  g_autoptr(GPtrArray) cc = NULL;
  g_autoptr(GPtrArray) bcc = NULL;
  gsize i;
  const char *single_uri = NULL;

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

  if (show_help || addresses == NULL || addresses[0] == NULL)
    {
      char *help = g_option_context_get_help (context, TRUE, NULL);
      g_print ("%s\n", help);

      g_free (help);
      return 0;
    }

  /* If there is only one argument and it is a mailto: URI, behave like
   * xdg-open instead, allowing the full generality of RFC 6068 mailto:
   * URLs */
  if ((opt_cc == NULL || *opt_cc == NULL) &&
      (opt_bcc == NULL || *opt_bcc == NULL) &&
      subject == NULL && body == NULL && attach == NULL &&
      addresses[1] == NULL &&
      g_ascii_strncasecmp (addresses[0], "mailto:", strlen ("mailto:")) == 0)
    {
      single_uri = addresses[0];
    }
  else
    {
      to = g_ptr_array_new_full (g_strv_length (addresses), g_free);
      cc = g_ptr_array_new_full (opt_cc == NULL ? 0 : g_strv_length (opt_cc), g_free);
      bcc = g_ptr_array_new_full (opt_bcc == NULL ? 0 : g_strv_length (opt_bcc), g_free);

      for (i = 0; opt_cc != NULL && opt_cc[i] != NULL; i++)
        g_ptr_array_add (cc, g_strdup (opt_cc[i]));

      for (i = 0; opt_bcc != NULL && opt_bcc[i] != NULL; i++)
        g_ptr_array_add (bcc, g_strdup (opt_bcc[i]));

      for (i = 0; addresses[i] != NULL; i++)
        {
          if (g_ascii_strncasecmp (addresses[i], "mailto:",
                                   strlen ("mailto:")) == 0)
            {
              g_autofree gchar *rest = g_strdup (addresses[i] + strlen ("mailto:"));
              char *token;
              char *question_mark = strchr (rest, '?');
              char *saveptr = NULL;

              if (question_mark != NULL)
                *question_mark = '\0';

              /* The part before any '?' is a comma-separated list of URI-escaped
               * email addresses, but may be empty */
              if (rest[0] != '\0')
                {
                  for (token = strtok_r (rest, ",", &saveptr);
                       token != NULL;
                       token = strtok_r (NULL, ",", &saveptr))
                    {
                      g_autofree gchar *addr = g_uri_unescape_string (token, NULL);

                      if (addr != NULL)
                        g_ptr_array_add (to, g_steal_pointer (&addr));
                      else
                        g_warning ("Invalid URI-escaped email address: %s", token);
                    }
                }

              if (question_mark == NULL)
                continue;

              /* The part after '?' (if any) is an &-separated list of header
               * field/value pairs */
              for (token = strtok_r (question_mark + 1, "&", &saveptr);
                   token != NULL;
                   token = strtok_r (NULL, "&", &saveptr))
                {
                  g_autofree gchar *value = NULL;
                  char *equals = strchr (token, '=');
                  const char *header;

                  if (equals == NULL)
                    {
                      g_warning ("No '=' found in %s", token);
                      continue;
                    }

                  *equals = '\0';
                  header = token;
                  value = g_uri_unescape_string (equals + 1, NULL);

                  if (value == NULL)
                    {
                      g_warning ("Invalid URI-escaped value for '%s': %s",
                                 header, value);
                      continue;
                    }

                  if (g_ascii_strcasecmp (header, "to") == 0)
                    {
                      char *a, *saveptr2 = NULL;

                      for (a = strtok_r (value, ",", &saveptr2);
                           a != NULL;
                           a = strtok_r (NULL, ",", &saveptr2))
                        g_ptr_array_add (to, g_strdup (a));
                    }
                  else if (g_ascii_strcasecmp (header, "cc") == 0)
                    {
                      char *a, *saveptr2 = NULL;

                      for (a = strtok_r (value, ",", &saveptr2);
                           a != NULL;
                           a = strtok_r (NULL, ",", &saveptr2))
                        g_ptr_array_add (cc, g_strdup (a));
                    }
                  else if (g_ascii_strcasecmp (header, "bcc") == 0)
                    {
                      char *a, *saveptr2 = NULL;

                      for (a = strtok_r (value, ",", &saveptr2);
                           a != NULL;
                           a = strtok_r (NULL, ",", &saveptr2))
                        g_ptr_array_add (bcc, g_strdup (a));
                    }
                  else if (g_ascii_strcasecmp (header, "subject") == 0)
                    {
                      g_clear_pointer (&subject, g_free);
                      subject = g_steal_pointer (&value);
                    }
                  else if (g_ascii_strcasecmp (header, "body") == 0)
                    {
                      g_clear_pointer (&body, g_free);
                      body = g_steal_pointer (&value);
                    }
                  else
                    {
                      g_debug ("Ignoring unknown header field in mailto: URI: %s",
                               header);
                    }
                }
            }
          else
            {
              g_ptr_array_add (to, g_strdup (addresses[i]));
            }
        }
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

  if (single_uri != NULL)
    {
      ret = g_dbus_connection_call_sync (connection,
                                         PORTAL_BUS_NAME,
                                         PORTAL_OBJECT_PATH,
                                         PORTAL_IFACE_NAME_OPENURI,
                                         "OpenURI",
                                         g_variant_new ("(ss@a{sv})",
                                                        "", single_uri,
                                                        g_variant_builder_end (&opt_builder)),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

      if (ret == NULL)
        {
          g_printerr ("Failed to call portal: %s\n", error->message);

          g_object_unref (connection);
          g_error_free (error);
          return 4;
        }

      g_object_unref (connection);
      return 0;
    }

  ret = g_dbus_connection_call_sync (connection,
                                     PORTAL_BUS_NAME,
                                     PORTAL_OBJECT_PATH,
                                     "org.freedesktop.DBus.Properties",
                                     "Get",
                                     g_variant_new ("(ss)", "org.freedesktop.portal.Email", "version"),
                                     G_VARIANT_TYPE ("(v)"),
                                     0,
                                     G_MAXINT,
                                     NULL,
                                     NULL);
  if (ret != NULL)
    {
      g_variant_get (ret, "(v)", &v);

      if (g_variant_is_of_type (v, G_VARIANT_TYPE ("u")))
        g_variant_get (v, "u", &version);
      else
        g_warning ("o.fd.portal.Email.version had unexpected type %s",
                   g_variant_get_type_string (v));
    }

  if (version >= 3)
    {
      g_variant_builder_add (&opt_builder,
                             "{sv}",
                             "addresses",
                             g_variant_new_strv ((const char * const *) to->pdata,
                                                 to->len));

      if (cc->len > 0)
        g_variant_builder_add (&opt_builder,
                               "{sv}",
                               "cc", g_variant_new_strv ((const char * const *) cc->pdata,
                                                         cc->len));

      if (bcc->len > 0)
        g_variant_builder_add (&opt_builder,
                               "{sv}",
                               "bcc", g_variant_new_strv ((const char * const *) bcc->pdata,
                                                          bcc->len));
    }
  else
    {
      if (to->len == 0)
        {
          g_printerr ("xdg-email: No valid addresses found\n");
          return 1;
        }

      g_variant_builder_add (&opt_builder,
                             "{sv}",
                             "address", g_variant_new_string (g_ptr_array_index (to, 0)));
    }

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
                             "attachment_fds", g_variant_new_parsed ("@ah [0]"));
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
