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

#include "common.h"

#include <string.h>

#include <gio/gio.h>

#include "backport-autoptr.h"

void
setup_dbus_daemon (GSubprocess **dbus_daemon,
                   gchar       **dbus_address)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  GInputStream *address_pipe;
  gchar address_buffer[4096] = { 0 };
  g_autofree gchar *escaped = NULL;
  char *newline;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  *dbus_daemon = g_subprocess_launcher_spawn (launcher, &error,
                                              "dbus-daemon",
                                              "--session",
                                              "--print-address=1",
                                              "--nofork",
                                              NULL);
  g_assert_no_error (error);
  g_assert_nonnull (*dbus_daemon);

  address_pipe = g_subprocess_get_stdout_pipe (*dbus_daemon);

  /* Crash if it takes too long to get the address */
  alarm (30);

  while (strchr (address_buffer, '\n') == NULL)
    {
      if (strlen (address_buffer) >= sizeof (address_buffer) - 1)
        g_error ("Read %" G_GSIZE_FORMAT " bytes from dbus-daemon with "
                 "no newline",
                 sizeof (address_buffer) - 1);

      g_input_stream_read (address_pipe,
                           address_buffer + strlen (address_buffer),
                           sizeof (address_buffer) - strlen (address_buffer),
                           NULL, &error);
      g_assert_no_error (error);
    }

  /* Disable alarm */
  alarm (0);

  newline = strchr (address_buffer, '\n');
  g_assert_nonnull (newline);
  *newline = '\0';
  *dbus_address = g_strdup (address_buffer);
}

void
own_name_sync (GDBusConnection *conn,
               const char *name)
{
  g_autoptr(GVariant) variant = NULL;
  g_autoptr(GError) error = NULL;
  guint32 result;

  /* We don't use g_bus_own_name() here because it's easier to be
   * synchronous */
  variant = g_dbus_connection_call_sync (conn,
                                         DBUS_SERVICE_DBUS,
                                         DBUS_PATH_DBUS,
                                         DBUS_IFACE_DBUS,
                                         "RequestName",
                                         g_variant_new ("(su)",
                                                        name,
                                                        DBUS_NAME_FLAG_DO_NOT_QUEUE),
                                         G_VARIANT_TYPE ("(u)"),
                                         G_DBUS_CALL_FLAGS_NONE, -1,
                                         NULL, &error);
  g_assert_no_error (error);
  g_variant_get (variant, "(u)", &result);
  g_assert_cmpuint (result, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
}

/* A GAsyncReadyCallback that stores @res via a `GAsyncResult **`. */
void
store_result_cb (G_GNUC_UNUSED GObject *source,
                 GAsyncResult *res,
                 gpointer data)
{
  GAsyncResult **res_p = data;

  g_assert_nonnull (res_p);
  g_assert_null (*res_p);
  g_assert_true (G_IS_ASYNC_RESULT (res));
  *res_p = g_object_ref (res);
}
