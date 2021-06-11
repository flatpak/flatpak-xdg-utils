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
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "backport-autoptr.h"

/* Change to #if 1 to check backwards-compatibility code paths */
#if 0
#undef GLIB_CHECK_VERSION
#define GLIB_CHECK_VERSION(x, y, z) (0)
#endif

typedef enum {
  FLATPAK_SPAWN_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_SPAWN_FLAGS_LATEST_VERSION = 1 << 1,
  FLATPAK_SPAWN_FLAGS_SANDBOX = 1 << 2,
  FLATPAK_SPAWN_FLAGS_NO_NETWORK = 1 << 3,
  FLATPAK_SPAWN_FLAGS_WATCH_BUS = 1 << 4, /* Since 1.2 */
  FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS = 1 << 5, /* Since 1.6, optional */
  FLATPAK_SPAWN_FLAGS_NOTIFY_START = 1 << 6,
  FLATPAK_SPAWN_FLAGS_SHARE_PIDS = 1 << 7,
  FLATPAK_SPAWN_FLAGS_EMPTY_APP = 1 << 8,
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

/* The same flag is reused: this feature is available under the same
 * circumstances */
#define FLATPAK_SPAWN_SUPPORT_FLAGS_SHARE_PIDS FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS

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
  guint32 wait_status = 0;

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(uu)")))
    return;

  g_variant_get (parameters, "(uu)", &client_pid, &wait_status);
  g_debug ("child exited %d: %d", client_pid, wait_status);

  if (child_pid == client_pid)
    {
      int exit_code;

      if (WIFEXITED (wait_status))
        {
          exit_code = WEXITSTATUS (wait_status);
        }
      else if (WIFSIGNALED (wait_status))
        {
          /* Smush the signal into an unsigned byte, as the shell does. This is
           * not quite right from the perspective of whatever ran flatpak-spawn
           * — it will get WIFEXITED() not WIFSIGNALED() — but the
           *  alternative is to disconnect all signal() handlers then send this
           *  signal to ourselves and hope it kills us.
           */
          exit_code = 128 + WTERMSIG (wait_status);
        }
      else
        {
          /* wait(3p) claims that if the waitpid() call that returned the exit
           * code specified neither WUNTRACED nor WIFSIGNALED, then exactly one
           * of WIFEXITED() or WIFSIGNALED() will be true.
           */
          g_warning ("wait status %d is neither WIFEXITED() nor WIFSIGNALED()",
                     wait_status);
          /* EX_SOFTWARE "internal software error" from sysexits.h, for want of
           * a better code.
           */
          exit_code = 70;
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

static void
forward_signal (int sig)
{
  g_autoptr(GVariant) reply = NULL;
  gboolean to_process_group = FALSE;
  g_autoptr(GError) error = NULL;

  if (child_pid == 0)
    {
      /* We are not monitoring a child yet, so let the signal act on
       * this main process instead */
      if (sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU)
        {
          raise (SIGSTOP);
        }
      else if (sig != SIGCONT)
        {
          sigset_t mask;

          sigemptyset (&mask);
          sigaddset (&mask, sig);
          /* Unblock it, so that it will be delivered properly this time.
           * Use pthread_sigmask instead of sigprocmask because the latter
           * has unspecified behaviour in a multi-threaded process. */
          pthread_sigmask (SIG_UNBLOCK, &mask, NULL);
          raise (sig);
        }

      return;
    }

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
}

static gboolean
forward_signal_handler (
#if GLIB_CHECK_VERSION (2, 36, 0)
                        int sfd,
#else
                        GIOChannel *source,
#endif
                        G_GNUC_UNUSED GIOCondition condition,
                        G_GNUC_UNUSED gpointer data)
{
  struct signalfd_siginfo info;
  ssize_t size;

#if !GLIB_CHECK_VERSION (2, 36, 0)
  int sfd;

  sfd = g_io_channel_unix_get_fd (source);
  g_return_val_if_fail (sfd >= 0, G_SOURCE_CONTINUE);
#endif

  size = read (sfd, &info, sizeof (info));

  if (size < 0)
    {
      if (errno != EINTR && errno != EAGAIN)
        g_warning ("Unable to read struct signalfd_siginfo: %s",
                   g_strerror (errno));
    }
  else if (size != sizeof (info))
    {
      g_warning ("Expected struct signalfd_siginfo of size %"
                 G_GSIZE_FORMAT ", got %" G_GSSIZE_FORMAT,
                 sizeof (info), size);
    }
  else
    {
      forward_signal (info.ssi_signo);
    }

  return G_SOURCE_CONTINUE;
}

static guint
forward_signals (void)
{
  static int forward[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCONT, SIGTSTP, SIGUSR1, SIGUSR2
  };
  sigset_t mask;
  guint i;
  int sfd;

  sigemptyset (&mask);

  for (i = 0; i < G_N_ELEMENTS (forward); i++)
    sigaddset (&mask, forward[i]);

  sfd = signalfd (-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

  if (sfd < 0)
    {
      g_warning ("Unable to watch signals: %s", g_strerror (errno));
      return 0;
    }

  /*
   * We have to block the signals, for two reasons:
   * - If we didn't, most of them would kill our process.
   *   Listening for a signal with a signalfd does not prevent the signal's
   *   default disposition from being acted on.
   * - Reading from a signalfd only returns information about the signals
   *   that are still pending for the process. If we ignored them instead
   *   of blocking them, they would no longer be pending by the time the
   *   main loop wakes up and reads from the signalfd.
   */
  pthread_sigmask (SIG_BLOCK, &mask, NULL);

#if GLIB_CHECK_VERSION (2, 36, 0)
  return g_unix_fd_add (sfd, G_IO_IN, forward_signal_handler, NULL);
#else
  GIOChannel *channel = g_io_channel_unix_new (sfd);
  guint ret;

  /* Disable text recoding, treat it as a bytestream */
  g_io_channel_set_encoding (channel, NULL, NULL);
  ret = g_io_add_watch (channel, G_IO_IN, forward_signal_handler, NULL);
  g_io_channel_unref (channel);
  return ret;
#endif
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
      opt_sandbox_flags |= FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_A11Y;
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

/*
 * @str: A path
 * @prefix: A possible prefix
 *
 * The same as flatpak_has_path_prefix(), but instead of a boolean,
 * return the part of @str after @prefix (non-%NULL but possibly empty)
 * if @str has prefix @prefix, or %NULL if it does not.
 *
 * Returns: (nullable) (transfer none): the part of @str after @prefix,
 *  or %NULL if @str is not below @prefix
 */
static const char *
get_path_after (const char *str,
                const char *prefix)
{
  while (TRUE)
    {
      /* Skip consecutive slashes to reach next path
         element */
      while (*str == '/')
        str++;
      while (*prefix == '/')
        prefix++;

      /* No more prefix path elements? Done! */
      if (*prefix == 0)
        return str;

      /* Compare path element */
      while (*prefix != 0 && *prefix != '/')
        {
          if (*str != *prefix)
            return NULL;
          str++;
          prefix++;
        }

      /* Matched prefix path element,
         must be entire str path element */
      if (*str != '/' && *str != 0)
        return NULL;
    }
}

static gint32
path_to_handle (GUnixFDList *fd_list,
                const char *path,
                const char *home_realpath,
                const char *flatpak_id,
                GError **error)
{
  int path_fd = open (path, O_PATH|O_CLOEXEC|O_NOFOLLOW|O_RDONLY);
  int saved_errno;
  gint32 handle;

  if (path_fd < 0)
    {
      saved_errno = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Failed to open %s to expose in sandbox: %s",
                   path, g_strerror (saved_errno));
      return -1;
    }

  if (home_realpath != NULL && flatpak_id != NULL)
    {
      g_autofree char *real = NULL;
      const char *after = NULL;

      real = realpath (path, NULL);

      if (real != NULL)
        after = get_path_after (real, home_realpath);

      if (after != NULL)
        {
          g_autofree char *var_path = NULL;
          int var_fd = -1;
          struct stat path_buf;
          struct stat var_buf;

          /* @after is possibly "", but that's OK: if @path is exactly $HOME,
           * we want to check whether it's the same file as
           * ~/.var/app/$FLATPAK_ID, with no suffix */
          var_path = g_build_filename (home_realpath, ".var", "app", flatpak_id,
                                       after, NULL);

          var_fd = open (var_path, O_PATH|O_CLOEXEC|O_NOFOLLOW|O_RDONLY);

          if (var_fd >= 0 &&
              fstat (path_fd, &path_buf) == 0 &&
              fstat (var_fd, &var_buf) == 0 &&
              path_buf.st_dev == var_buf.st_dev &&
              path_buf.st_ino == var_buf.st_ino)
            {
              close (path_fd);
              path_fd = var_fd;
              var_fd = -1;
            }
          else
            {
              close (var_fd);
            }
        }
    }


  handle = g_unix_fd_list_append (fd_list, path_fd, error);

  if (handle < 0)
    {
      g_prefix_error (error, "Failed to add fd to list for %s: ", path);
      return -1;
    }

  /* The GUnixFdList keeps a duplicate, so we should release the original */
  close (path_fd);
  return handle;
}

static gboolean
add_paths_to_variant (GVariantBuilder *builder,
                      GUnixFDList *fd_list,
                      const GStrv paths,
                      const char *home_realpath,
                      const char *flatpak_id,
                      gboolean ignore_errors)
{
  g_autoptr(GError) error = NULL;

  if (!paths)
    return TRUE;

  for (gsize i = 0; paths[i] != NULL; i++)
    {
      gint32 handle = path_to_handle (fd_list, paths[i], home_realpath,
                                      flatpak_id,
                                      ignore_errors ? NULL : &error);

      if (handle < 0)
        {
          if (ignore_errors)
            continue;

          g_printerr ("%s\n", error->message);
          return FALSE;
        }

      g_variant_builder_add (builder, "h", handle);
    }

  return TRUE;
}

static GHashTable *opt_env = NULL;
static GHashTable *opt_unsetenv = NULL;

static gboolean
opt_env_cb (G_GNUC_UNUSED const char *option_name,
            const gchar *value,
            G_GNUC_UNUSED gpointer data,
            GError **error)
{
  g_auto(GStrv) split = g_strsplit (value, "=", 2);

  g_assert (opt_env != NULL);
  g_assert (opt_unsetenv != NULL);

  if (split == NULL ||
      split[0] == NULL ||
      split[0][0] == 0 ||
      split[1] == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "Invalid env format %s", value);
      return FALSE;
    }

  g_hash_table_remove (opt_unsetenv, split[0]);
  g_hash_table_replace (opt_env,
                        g_steal_pointer (&split[0]),
                        g_steal_pointer (&split[1]));
  return TRUE;
}

static gboolean
opt_unset_env_cb (G_GNUC_UNUSED const char *option_name,
                  const gchar *value,
                  G_GNUC_UNUSED gpointer data,
                  G_GNUC_UNUSED GError **error)
{
  g_assert (opt_env != NULL);
  g_assert (opt_unsetenv != NULL);

  g_hash_table_remove (opt_env, value);
  g_hash_table_add (opt_unsetenv, g_strdup (value));
  return TRUE;
}

static gboolean
option_env_fd_cb (G_GNUC_UNUSED const gchar *option_name,
                  const gchar *value,
                  G_GNUC_UNUSED gpointer data,
                  GError **error)
{
  g_autofree gchar *proc_filename = NULL;
  g_autofree gchar *env_block = NULL;
  gsize remaining;
  const char *p;
  guint64 fd;
  gchar *endptr;

  g_assert (opt_env != NULL);
  g_assert (opt_unsetenv != NULL);

  fd = g_ascii_strtoull (value, &endptr, 10);

  if (endptr == NULL || *endptr != '\0' || fd > G_MAXINT)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "Not a valid file descriptor: %s", value);
      return FALSE;
    }

  proc_filename = g_strdup_printf ("/proc/self/fd/%d", (int) fd);

  if (!g_file_get_contents (proc_filename, &env_block, &remaining, error))
    return FALSE;

  p = env_block;

  while (remaining > 0)
    {
      g_autofree gchar *var = NULL;
      g_autofree gchar *val = NULL;
      size_t len = strnlen (p, remaining);
      const char *equals;

      g_assert (len <= remaining);

      equals = memchr (p, '=', len);

      if (equals == NULL || equals == p)
        {
          g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                       "Environment variable must be given in the form VARIABLE=VALUE, not %.*s",
                       (int) len, p);
          return FALSE;
        }

      var = g_strndup (p, equals - p);
      val = g_strndup (equals + 1, len - (equals - p) - 1);
      g_hash_table_remove (opt_unsetenv, var);
      g_hash_table_replace (opt_env,
                            g_steal_pointer (&var),
                            g_steal_pointer (&val));

      p += len;
      remaining -= len;

      if (remaining > 0)
        {
          g_assert (*p == '\0');
          p += 1;
          remaining -= 1;
        }
    }

  if (fd >= 3)
    close (fd);

  return TRUE;
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
  gboolean opt_watch_bus = FALSE;
  gboolean opt_expose_pids = FALSE;
  gboolean opt_share_pids = FALSE;
  gboolean opt_latest_version = FALSE;
  gboolean opt_sandbox = FALSE;
  gboolean opt_no_network = FALSE;
  char **opt_sandbox_expose = NULL;
  char **opt_sandbox_expose_ro = NULL;
  char **opt_sandbox_expose_path = NULL;
  char **opt_sandbox_expose_path_ro = NULL;
  char **opt_sandbox_expose_path_try = NULL;
  char **opt_sandbox_expose_path_ro_try = NULL;
  char *opt_directory = NULL;
  char *opt_app_path = NULL;
  char *opt_usr_path = NULL;
  g_autofree char *cwd = NULL;
  g_autofree char *home_realpath = NULL;
  const char *flatpak_id = NULL;
  GVariantBuilder options_builder;
  const GOptionEntry options[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output", NULL },
    { "forward-fd", 0, 0, G_OPTION_ARG_STRING_ARRAY, &forward_fds,  "Forward file descriptor", "FD" },
    { "clear-env", 0, 0, G_OPTION_ARG_NONE, &opt_clear_env,  "Run with clean environment", NULL },
    { "watch-bus", 0, 0, G_OPTION_ARG_NONE, &opt_watch_bus,  "Make the spawned command exit if we do", NULL },
    { "expose-pids", 0, 0, G_OPTION_ARG_NONE, &opt_expose_pids, "Expose sandbox pid in calling sandbox", NULL },
    { "share-pids", 0, 0, G_OPTION_ARG_NONE, &opt_share_pids, "Use same pid namespace as calling sandbox", NULL },
    { "env", 0, 0, G_OPTION_ARG_CALLBACK, &opt_env_cb, "Set environment variable", "VAR=VALUE" },
    { "unset-env", 0, 0, G_OPTION_ARG_CALLBACK, &opt_unset_env_cb, "Unset environment variable", "VAR=VALUE" },
    { "env-fd", 0, 0, G_OPTION_ARG_CALLBACK, &option_env_fd_cb, "Read environment variables in env -0 format from FD", "FD" },
    { "latest-version", 0, 0, G_OPTION_ARG_NONE, &opt_latest_version,  "Run latest version", NULL },
    { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandbox,  "Run sandboxed", NULL },
    { "no-network", 0, 0, G_OPTION_ARG_NONE, &opt_no_network,  "Run without network access", NULL },
    { "sandbox-expose", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sandbox_expose, "Expose access to named file", "NAME" },
    { "sandbox-expose-ro", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sandbox_expose_ro, "Expose readonly access to named file", "NAME" },
    { "sandbox-expose-path", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sandbox_expose_path, "Expose access to path", "PATH" },
    { "sandbox-expose-path-ro", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sandbox_expose_path_ro, "Expose readonly access to path", "PATH" },
    { "sandbox-expose-path-try", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sandbox_expose_path_try, "Expose access to path if it exists", "PATH" },
    { "sandbox-expose-path-ro-try", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sandbox_expose_path_ro_try, "Expose readonly access to path if it exists", "PATH" },
    { "sandbox-flag", 0, 0, G_OPTION_ARG_CALLBACK, sandbox_flag_callback, "Enable sandbox flag", "FLAG" },
    { "host", 0, 0, G_OPTION_ARG_NONE, &opt_host, "Start the command on the host", NULL },
    { "directory", 0, 0, G_OPTION_ARG_FILENAME, &opt_directory, "Working directory in which to run the command", "DIR" },
    { "app-path", 0, 0, G_OPTION_ARG_FILENAME, &opt_app_path, "Replace runtime's /app with DIR or empty", "DIR|\"\"" },
    { "usr-path", 0, 0, G_OPTION_ARG_FILENAME, &opt_usr_path, "Replace runtime's /usr with DIR", "DIR" },
    { NULL }
  };
  guint signal_source = 0;
  GHashTableIter iter;
  gpointer key, value;

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  child_argv = g_ptr_array_new ();

  cwd = g_get_current_dir ();

  i = 1;
  while (i < argc && argv[i][0] == '-')
    i++;

  opt_argc = i;
  opt_env = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  opt_unsetenv = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

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

  /* We have to block the signals we want to forward before we start any
   * other thread, and in particular the GDBus worker thread, because
   * the signal mask is per-thread. We need all threads to have the same
   * mask, otherwise a thread that doesn't have the mask will receive
   * process-directed signals, causing the whole process to exit. */
  signal_source = forward_signals ();

  if (signal_source == 0)
    return 1;

  flatpak_id = g_getenv ("FLATPAK_ID");

  if (flatpak_id != NULL)
    home_realpath = realpath (g_get_home_dir (), NULL);

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
      /* The GUnixFdList keeps a duplicate, so we should release the original */
      close (fd);
      g_variant_builder_add (fd_builder, "{uh}", fd, handle);
    }

  g_hash_table_iter_init (&iter, opt_env);

  while (g_hash_table_iter_next (&iter, &key, &value))
    g_variant_builder_add (env_builder, "{ss}", key, value);

  g_clear_pointer (&opt_env, g_hash_table_unref);

  spawn_flags = 0;

  if (opt_clear_env)
    spawn_flags |= opt_host ? FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV : FLATPAK_SPAWN_FLAGS_CLEAR_ENV;

  if (opt_watch_bus)
    spawn_flags |= opt_host ? FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS : FLATPAK_SPAWN_FLAGS_WATCH_BUS;

  if (opt_share_pids)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --share-pids\n");
          return 1;
        }

      check_portal_version ("share-pids", 5);
      check_portal_supports ("share-pids", FLATPAK_SPAWN_SUPPORT_FLAGS_SHARE_PIDS);

      spawn_flags |= FLATPAK_SPAWN_FLAGS_SHARE_PIDS;
    }
  else if (opt_expose_pids)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --expose-pids\n");
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
          g_printerr ("--host not compatible with --latest-version\n");
          return 1;
        }
      spawn_flags |= FLATPAK_SPAWN_FLAGS_LATEST_VERSION;
    }

  if (opt_sandbox)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox\n");
          return 1;
        }
      spawn_flags |= FLATPAK_SPAWN_FLAGS_SANDBOX;
    }

  if (opt_no_network)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --no-network\n");
          return 1;
        }
      spawn_flags |= FLATPAK_SPAWN_FLAGS_NO_NETWORK;
    }

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE ("a{sv}"));

  if (g_hash_table_size (opt_unsetenv) > 0)
    {
      g_hash_table_iter_init (&iter, opt_unsetenv);

      /* The host portal doesn't support options, so we always have to do
       * this the hard way. The subsandbox portal supports unset-env in
       * versions >= 5. */
      if (opt_host ? FALSE : (get_portal_version () >= 5))
        {
          GVariantBuilder strv_builder;

          g_variant_builder_init (&strv_builder, G_VARIANT_TYPE_STRING_ARRAY);

          while (g_hash_table_iter_next (&iter, &key, NULL))
            g_variant_builder_add (&strv_builder, "s", key);

          g_variant_builder_add (&options_builder, "{s@v}", "unset-env",
                                 g_variant_new_variant (g_variant_builder_end (&strv_builder)));
        }
      else
        {
          /* env(1) will do the wrong thing if argv[0] contains an equals
           * sign, so we might need to prepend this incantation - and
           * because we're prepending, we need to do it backwards.
           * More legibly, we're replacing MY=COMMAND ARGS with:
           *
           *     /usr/bin/env -u VAR -u VAR2 /bin/sh -euc 'exec "$@"' sh MY=COMMAND ARGS
           *
           * This is a standard trick for dealing with env(1). */
          g_assert (child_argv->len >= 1);

          if (strchr (g_ptr_array_index (child_argv, 0), '=') != NULL)
            {
              g_ptr_array_insert (child_argv, 0, g_strdup ("sh"));  /* argv[0] */
              g_ptr_array_insert (child_argv, 0, g_strdup ("exec \"$@\""));
              g_ptr_array_insert (child_argv, 0, g_strdup ("-euc"));
              g_ptr_array_insert (child_argv, 0, g_strdup ("/bin/sh"));
            }

          while (g_hash_table_iter_next (&iter, &key, NULL))
            {
              /* Again, yes, this is backwards: we're prepending. */
              g_ptr_array_insert (child_argv, 0, g_strdup (key));
              g_ptr_array_insert (child_argv, 0, g_strdup ("-u"));
            }

          g_ptr_array_insert (child_argv, 0, g_strdup ("/usr/bin/env"));
        }
    }

  g_clear_pointer (&opt_unsetenv, g_hash_table_unref);

  if (opt_sandbox_expose)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose\n");
          return 1;
        }
      g_variant_builder_add (&options_builder, "{s@v}", "sandbox-expose",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *)opt_sandbox_expose, -1)));
    }

  if (opt_sandbox_expose_ro)
    {
      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose-ro\n");
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

  if (opt_sandbox_expose_path || opt_sandbox_expose_path_try)
    {
      g_autoptr(GVariantBuilder) expose_fd_builder = g_variant_builder_new (G_VARIANT_TYPE ("ah"));

      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose-path\n");
          return 1;
        }

      check_portal_version ("sandbox-expose-path", 3);

      if (!add_paths_to_variant (expose_fd_builder, fd_list, opt_sandbox_expose_path, home_realpath, flatpak_id, FALSE)
          || !add_paths_to_variant (expose_fd_builder, fd_list, opt_sandbox_expose_path_try, home_realpath, flatpak_id, TRUE))
        return 1;

      g_variant_builder_add (&options_builder, "{s@v}", "sandbox-expose-fd",
                             g_variant_new_variant (g_variant_builder_end (g_steal_pointer (&expose_fd_builder))));
    }

  if (opt_sandbox_expose_path_ro || opt_sandbox_expose_path_ro_try)
    {
      g_autoptr(GVariantBuilder) expose_fd_builder = g_variant_builder_new (G_VARIANT_TYPE ("ah"));

      if (opt_host)
        {
          g_printerr ("--host not compatible with --sandbox-expose-path-ro\n");
          return 1;
        }

      check_portal_version ("sandbox-expose-path-ro", 3);

      if (!add_paths_to_variant (expose_fd_builder, fd_list, opt_sandbox_expose_path_ro, home_realpath, flatpak_id, FALSE)
          || !add_paths_to_variant (expose_fd_builder, fd_list, opt_sandbox_expose_path_ro_try, home_realpath, flatpak_id, TRUE))
        return 1;

      g_variant_builder_add (&options_builder, "{s@v}", "sandbox-expose-fd-ro",
                             g_variant_new_variant (g_variant_builder_end (g_steal_pointer (&expose_fd_builder))));
    }

  if (opt_app_path != NULL)
    {
      gint32 handle;

      g_debug ("Using \"%s\" as /app instead of runtime", opt_app_path);

      if (opt_host)
        {
          g_printerr ("--host not compatible with --app-path\n");
          return 1;
        }

      check_portal_version ("app-path", 6);

      if (opt_app_path[0] == '\0')
        {
          /* Empty path is special-cased to mean an empty directory */
          spawn_flags |= FLATPAK_SPAWN_FLAGS_EMPTY_APP;
        }
      else
        {
          handle = path_to_handle (fd_list, opt_app_path, home_realpath,
                                   flatpak_id, &error);

          if (handle < 0)
            {
              g_printerr ("%s\n", error->message);
              return 1;
            }

          g_variant_builder_add (&options_builder, "{s@v}", "app-fd",
                                 g_variant_new_variant (g_variant_new_handle (handle)));
        }
    }

  if (opt_usr_path != NULL)
    {
      gint32 handle;

      g_debug ("Using %s as /usr instead of runtime", opt_usr_path);

      if (opt_host)
        {
          g_printerr ("--host not compatible with --usr-path\n");
          return 1;
        }

      check_portal_version ("usr-path", 6);

      handle = path_to_handle (fd_list, opt_usr_path, home_realpath,
                               flatpak_id, &error);

      if (handle < 0)
        {
          g_printerr ("%s\n", error->message);
          return 1;
        }

      g_variant_builder_add (&options_builder, "{s@v}", "usr-fd",
                             g_variant_new_variant (g_variant_new_handle (handle)));
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

  /* Release our reference to the fds, so that only the copy we sent over
   * D-Bus remains open */
  g_clear_object (&fd_list);

  loop = g_main_loop_new (NULL, FALSE);

  g_signal_connect (session_bus, "closed", G_CALLBACK (session_bus_closed_cb), loop);

  g_main_loop_run (loop);

  if (signal_source != 0)
    g_source_remove (signal_source);

  return 0;
}
