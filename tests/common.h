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

#pragma once

#include <gio/gio.h>

/* GDBus interface info contains padding for future expansion, silence
 * warnings about this */
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define DBUS_SERVICE_DBUS "org.freedesktop.DBus"
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"
#define DBUS_IFACE_DBUS DBUS_SERVICE_DBUS
#define DBUS_NAME_FLAG_DO_NOT_QUEUE 4
#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1

void setup_dbus_daemon (GSubprocess **dbus_daemon,
                        gchar       **dbus_address);
void own_name_sync (GDBusConnection *conn,
                    const char *name);
void store_result_cb (GObject *source, GAsyncResult *res, gpointer data);

#ifndef g_assert_no_errno
#define g_assert_no_errno(expr) \
  g_assert_cmpstr ((expr) >= 0 ? NULL : g_strerror (errno), ==, NULL)
#endif
