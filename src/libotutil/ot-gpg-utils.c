/*
 * Copyright (C) 2015 Red Hat, Inc.
 * Copyright (C) 2022 Igalia S.L.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ot-gpg-utils.h"

#include <stdlib.h>

#include <gio/gunixoutputstream.h>
#include "libglnx.h"
#include "zbase32.h"

/* Like glnx_throw_errno_prefix, but takes @gpg_error */
gboolean
ot_gpgme_throw (gpgme_error_t gpg_error, GError **error,
                const char *fmt, ...)
{
  if (error == NULL)
    return FALSE;

  GIOErrorEnum errcode;
  char errbuf[1024];

  /* XXX This list is incomplete.  Add cases as needed. */
  switch (gpgme_err_code (gpg_error))
    {
      /* special case - shouldn't be here */
      case GPG_ERR_NO_ERROR:
        g_assert_not_reached ();

      /* special case - abort on out-of-memory */
      case GPG_ERR_ENOMEM:
        (void) gpg_strerror_r (gpg_error, errbuf, sizeof (errbuf));
        errbuf[sizeof(errbuf)-1] = '\0';
        g_error ("%s: %s",
                 gpgme_strsource (gpg_error),
                 errbuf);

      case GPG_ERR_INV_VALUE:
        errcode = G_IO_ERROR_INVALID_ARGUMENT;
        break;

      default:
        errcode = G_IO_ERROR_FAILED;
        break;
    }

  (void) gpg_strerror_r (gpg_error, errbuf, sizeof (errbuf));
  errbuf[sizeof(errbuf)-1] = '\0';
  g_set_error (error, G_IO_ERROR, errcode, "%s: %s",
               gpgme_strsource (gpg_error),
               errbuf);
  va_list args;
  va_start (args, fmt);
  glnx_real_set_prefix_error_va (*error, fmt, args);
  va_end (args);

  return FALSE;
}

gboolean
ot_gpgme_ctx_tmp_home_dir (gpgme_ctx_t     gpgme_ctx,
                           char          **out_tmp_home_dir,
                           GOutputStream **out_pubring_stream,
                           GCancellable   *cancellable,
                           GError        **error)
{
  g_autofree char *tmp_home_dir = NULL;
  gpgme_error_t gpg_error;
  gboolean ret = FALSE;

  g_return_val_if_fail (gpgme_ctx != NULL, FALSE);

  /* GPGME has no API for using multiple keyrings (aka, gpg --keyring),
   * so we create a temporary directory and tell GPGME to use it as the
   * home directory.  Then (optionally) create a pubring.gpg file there
   * and hand the caller an open output stream to concatenate necessary
   * keyring files. */

  tmp_home_dir = g_build_filename (g_get_tmp_dir (), "ostree-gpg-XXXXXX", NULL);

  if (mkdtemp (tmp_home_dir) == NULL)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* Not documented, but gpgme_ctx_set_engine_info() accepts NULL for
   * the executable file name, which leaves the old setting unchanged. */
  gpg_error = gpgme_ctx_set_engine_info (gpgme_ctx,
                                         GPGME_PROTOCOL_OpenPGP,
                                         NULL, tmp_home_dir);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "gpgme_ctx_set_engine_info");
      goto out;
    }

  if (out_pubring_stream != NULL)
    {
      GFileOutputStream *pubring_stream;
      g_autoptr(GFile) pubring_file = NULL;
      g_autofree char *pubring_path = NULL;

      pubring_path = g_build_filename (tmp_home_dir, "pubring.gpg", NULL);
      pubring_file = g_file_new_for_path (pubring_path);

      pubring_stream = g_file_create (pubring_file,
                                      G_FILE_CREATE_NONE,
                                      cancellable, error);
      if (pubring_stream == NULL)
        goto out;

      /* Sneaky cast from GFileOutputStream to GOutputStream. */
      *out_pubring_stream = (GOutputStream *)g_steal_pointer (&pubring_stream);
    }

  if (out_tmp_home_dir != NULL)
    *out_tmp_home_dir = g_steal_pointer (&tmp_home_dir);

  ret = TRUE;

out:
  if (!ret)
    {
      /* Clean up our mess on error. */
      (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmp_home_dir, NULL, NULL);
    }

  return ret;
}

/**** The functions below are based on seahorse-gpgme-data.c ****/

static void
set_errno_from_gio_error (GError *error)
{
  /* This is the reverse of g_io_error_from_errno() */

  g_return_if_fail (error != NULL);

  switch (error->code)
    {
      case G_IO_ERROR_FAILED:
        errno = EIO;
        break;
      case G_IO_ERROR_NOT_FOUND:
        errno = ENOENT;
        break;
      case G_IO_ERROR_EXISTS:
        errno = EEXIST;
        break;
      case G_IO_ERROR_IS_DIRECTORY:
        errno = EISDIR;
        break;
      case G_IO_ERROR_NOT_DIRECTORY:
        errno = ENOTDIR;
        break;
      case G_IO_ERROR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
      case G_IO_ERROR_NOT_REGULAR_FILE:
      case G_IO_ERROR_NOT_SYMBOLIC_LINK:
      case G_IO_ERROR_NOT_MOUNTABLE_FILE:
        errno = EBADF;
        break;
      case G_IO_ERROR_FILENAME_TOO_LONG:
        errno = ENAMETOOLONG;
        break;
      case G_IO_ERROR_INVALID_FILENAME:
        errno = EINVAL;
        break;
      case G_IO_ERROR_TOO_MANY_LINKS:
        errno = EMLINK;
        break;
      case G_IO_ERROR_NO_SPACE:
        errno = ENOSPC;
        break;
      case G_IO_ERROR_INVALID_ARGUMENT:
        errno = EINVAL;
        break;
      case G_IO_ERROR_PERMISSION_DENIED:
        errno = EPERM;
        break;
      case G_IO_ERROR_NOT_SUPPORTED:
        errno = ENOTSUP;
        break;
      case G_IO_ERROR_NOT_MOUNTED:
        errno = ENOENT;
        break;
      case G_IO_ERROR_ALREADY_MOUNTED:
        errno = EALREADY;
        break;
      case G_IO_ERROR_CLOSED:
        errno = EBADF;
        break;
      case G_IO_ERROR_CANCELLED:
        errno = EINTR;
        break;
      case G_IO_ERROR_PENDING:
        errno = EALREADY;
        break;
      case G_IO_ERROR_READ_ONLY:
        errno = EACCES;
        break;
      case G_IO_ERROR_CANT_CREATE_BACKUP:
        errno = EIO;
        break;
      case G_IO_ERROR_WRONG_ETAG:
        errno = EACCES;
        break;
      case G_IO_ERROR_TIMED_OUT:
        errno = EIO;
        break;
      case G_IO_ERROR_WOULD_RECURSE:
        errno = ELOOP;
        break;
      case G_IO_ERROR_BUSY:
        errno = EBUSY;
        break;
      case G_IO_ERROR_WOULD_BLOCK:
        errno = EWOULDBLOCK;
        break;
      case G_IO_ERROR_HOST_NOT_FOUND:
        errno = EHOSTDOWN;
        break;
      case G_IO_ERROR_WOULD_MERGE:
        errno = EIO;
        break;
      case G_IO_ERROR_FAILED_HANDLED:
        errno = 0;
        break;
      default:
        errno = EIO;
        break;
    }
}

static ssize_t
data_read_cb (void *handle, void *buffer, size_t size)
{
  GInputStream *input_stream = handle;
  gsize bytes_read;
  GError *local_error = NULL;

  g_return_val_if_fail (G_IS_INPUT_STREAM (input_stream), -1);

  if (!g_input_stream_read_all (input_stream, buffer, size,
                                &bytes_read, NULL, &local_error))
    {
      set_errno_from_gio_error (local_error);
      g_clear_error (&local_error);
      bytes_read = -1;
    }

  return bytes_read;
}

static ssize_t
data_write_cb (void *handle, const void *buffer, size_t size)
{
  GOutputStream *output_stream = handle;
  gsize bytes_written;
  GError *local_error = NULL;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (output_stream), -1);

  if (g_output_stream_write_all (output_stream, buffer, size,
                                 &bytes_written, NULL, &local_error))
    {
      (void)g_output_stream_flush (output_stream, NULL, &local_error);
    }

  if (local_error != NULL)
    {
      set_errno_from_gio_error (local_error);
      g_clear_error (&local_error);
      bytes_written = -1;
    }

  return bytes_written;
}

static off_t
data_seek_cb (void *handle, off_t offset, int whence)
{
  GObject *stream = handle;
  GSeekable *seekable;
  GSeekType seek_type = 0;
  off_t position = -1;
  GError *local_error = NULL;

  g_return_val_if_fail (G_IS_INPUT_STREAM (stream) ||
                        G_IS_OUTPUT_STREAM (stream), -1);

  if (!G_IS_SEEKABLE (stream)) {
    errno = EOPNOTSUPP;
    goto out;
  }

  switch (whence)
    {
      case SEEK_SET:
        seek_type = G_SEEK_SET;
        break;
      case SEEK_CUR:
        seek_type = G_SEEK_CUR;
        break;
      case SEEK_END:
        seek_type = G_SEEK_END;
        break;
      default:
        g_assert_not_reached ();
    }

  seekable = G_SEEKABLE (stream);

  if (!g_seekable_seek (seekable, offset, seek_type, NULL, &local_error))
    {
      set_errno_from_gio_error (local_error);
      g_clear_error (&local_error);
      goto out;
    }

  position = g_seekable_tell (seekable);

out:
  return position;
}

static void
data_release_cb (void *handle)
{
  GObject *stream = handle;

  g_return_if_fail (G_IS_INPUT_STREAM (stream) ||
                    G_IS_OUTPUT_STREAM (stream));

  g_object_unref (stream);
}

static struct gpgme_data_cbs data_input_cbs = {
  data_read_cb,
  NULL,
  data_seek_cb,
  data_release_cb
};

static struct gpgme_data_cbs data_output_cbs = {
  NULL,
  data_write_cb,
  data_seek_cb,
  data_release_cb
};

gpgme_data_t
ot_gpgme_data_input (GInputStream *input_stream)
{
  gpgme_data_t data = NULL;
  gpgme_error_t gpg_error;

  g_return_val_if_fail (G_IS_INPUT_STREAM (input_stream), NULL);

  gpg_error = gpgme_data_new_from_cbs (&data, &data_input_cbs, input_stream);

  /* The only possible error is ENOMEM, which we abort on. */
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      g_assert (gpgme_err_code (gpg_error) == GPG_ERR_ENOMEM);
      g_assert_not_reached ();
    }

  g_object_ref (input_stream);

  return data;
}

gpgme_data_t
ot_gpgme_data_output (GOutputStream *output_stream)
{
  gpgme_data_t data = NULL;
  gpgme_error_t gpg_error;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (output_stream), NULL);

  gpg_error = gpgme_data_new_from_cbs (&data, &data_output_cbs, output_stream);

  /* The only possible error is ENOMEM, which we abort on. */
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      g_assert (gpgme_err_code (gpg_error) == GPG_ERR_ENOMEM);
      g_assert_not_reached ();
    }

  g_object_ref (output_stream);

  return data;
}

gpgme_ctx_t
ot_gpgme_new_ctx (const char *homedir,
                  GError    **error)
{
  gpgme_error_t err;
  g_auto(gpgme_ctx_t) context = NULL;

  if ((err = gpgme_new (&context)) != GPG_ERR_NO_ERROR)
    return ot_gpgme_throw (err, error,  "Unable to create gpg context"), NULL;

  if (homedir != NULL)
    {
      gpgme_engine_info_t info;

      info = gpgme_ctx_get_engine_info (context);

      if ((err = gpgme_ctx_set_engine_info (context, info->protocol, NULL, homedir))
          != GPG_ERR_NO_ERROR)
        return ot_gpgme_throw (err, error, "Unable to set gpg homedir to '%s'", homedir), NULL;
    }

  return g_steal_pointer (&context);
}

static gboolean
get_gnupg_version (guint *major,
                   guint *minor,
                   guint *patch)
{
  g_return_val_if_fail (major != NULL, FALSE);
  g_return_val_if_fail (minor != NULL, FALSE);
  g_return_val_if_fail (patch != NULL, FALSE);

  gpgme_engine_info_t info;
  gpgme_error_t err = gpgme_get_engine_info (&info);
  if (err != GPG_ERR_NO_ERROR)
    {
      g_debug ("Failed to get GPGME engine info: %s: %s",
               gpgme_strsource (err), gpgme_strerror (err));
      return FALSE;
    }

  const char *gnupg_version = NULL;
  for (; info != NULL; info = info->next)
    {
      if (info->protocol == GPGME_PROTOCOL_OpenPGP)
        {
          gnupg_version = info->version;
          break;
        }
    }

  if (gnupg_version == NULL)
    {
      g_debug ("Could not determine GnuPG version");
      return FALSE;
    }

  g_auto(GStrv) parts = g_strsplit (gnupg_version, ".", 4);
  if (g_strv_length (parts) < 3)
    {
      g_debug ("Less than 3 components in GnuPG version \"%s\"", gnupg_version);
      return FALSE;
    }

  *major = g_ascii_strtoull (parts[0], NULL, 10);
  *minor = g_ascii_strtoull (parts[1], NULL, 10);
  *patch = g_ascii_strtoull (parts[2], NULL, 10);

  return TRUE;
}

void
ot_gpgme_kill_agent (const char *homedir)
{
  g_return_if_fail (homedir != NULL);

  /* If gnupg is at least 2.1.17, gpg-agent will exit when the homedir
   * is deleted.
   */
  guint gnupg_major = 0, gnupg_minor = 0, gnupg_patch = 0;
  if (get_gnupg_version (&gnupg_major, &gnupg_minor, &gnupg_patch))
    {
      if ((gnupg_major > 2) ||
          (gnupg_major == 2 && gnupg_minor > 1) ||
          (gnupg_major == 2 && gnupg_minor == 1 && gnupg_patch >= 17))
        {
          /* Note early return */
          g_debug ("GnuPG >= 2.1.17, skipping gpg-agent cleanup in %s", homedir);
          return;
        }
    }

  /* Run gpg-connect-agent killagent /bye */
  g_autoptr(GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "gpg-connect-agent");
  g_ptr_array_add (argv, "--homedir");
  g_ptr_array_add (argv, (gpointer)homedir);
  g_ptr_array_add (argv, "killagent");
  g_ptr_array_add (argv, "/bye");
  g_ptr_array_add (argv, NULL);

  g_autoptr(GError) local_error = NULL;
  GSpawnFlags flags = G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL;
  gint proc_status = 0;
  g_autofree gchar *proc_stderr = NULL;
  g_debug ("Killing gpg-agent in %s", homedir);
  if (!g_spawn_sync (NULL, (char **)argv->pdata, NULL, flags, NULL, NULL,
                     NULL, &proc_stderr, &proc_status, &local_error))
    {
      g_debug ("Spawning gpg-connect-agent failed: %s", local_error->message);
      return;
    }
  if (!g_spawn_check_exit_status (proc_status, &local_error))
    {
      /* Dump out stderr on failures */
      g_printerr ("%s", proc_stderr);

      g_debug ("Killing GPG agent with gpg-connect-agent failed: %s",
               local_error->message);
      return;
    }
}

/* Takes the SHA1 checksum of the local component of an email address and
 * returns the zbase32 encoding.
 */
static char *
encode_wkd_local (const char *local)
{
  g_return_val_if_fail (local != NULL, NULL);

  guint8 digest[20] = { 0 };
  gsize len = sizeof (digest);
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA1);
  g_checksum_update (checksum, (const guchar *)local, -1);
  g_checksum_get_digest (checksum, digest, &len);

  char *encoded = zbase32_encode (digest, len);

  /* If the returned string is NULL, then there must have been a memory
   * allocation problem. Just exit immediately like g_malloc.
   */
  if (encoded == NULL)
    g_error ("%s: %s", G_STRLOC, g_strerror (errno));

  return encoded;
}

/* Implementation of OpenPGP Web Key Directory URLs as defined in
 * https://datatracker.ietf.org/doc/html/draft-koch-openpgp-webkey-service
 */
gboolean
ot_gpg_wkd_urls (const char  *email,
                 char       **out_advanced_url,
                 char       **out_direct_url,
                 GError     **error)
{
  g_return_val_if_fail (email != NULL, FALSE);

  g_auto(GStrv) email_parts = g_strsplit (email, "@", -1);
  if (g_strv_length (email_parts) != 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid email address \"%s\"", email);
      return FALSE;
    }

  g_autofree char *local_lowered = g_ascii_strdown (email_parts[0], -1);
  g_autofree char *domain_lowered = g_ascii_strdown (email_parts[1], -1);
  g_autofree char *local_encoded = encode_wkd_local (local_lowered);
  g_autofree char *local_escaped = g_uri_escape_string (email_parts[0], NULL, FALSE);

  g_autofree char *advanced_url = g_strdup_printf ("https://openpgpkey.%s"
                                                   "/.well-known/openpgpkey"
                                                   "/%s/hu/%s?l=%s",
                                                   email_parts[1],
                                                   domain_lowered,
                                                   local_encoded,
                                                   local_escaped);
  g_debug ("GPG UID \"%s\" advanced WKD URL: %s", email, advanced_url);

  g_autofree char *direct_url = g_strdup_printf ("https://%s"
                                                 "/.well-known/openpgpkey"
                                                 "/hu/%s?l=%s",
                                                 email_parts[1],
                                                 local_encoded,
                                                 local_escaped);
  g_debug ("GPG UID \"%s\" direct WKD URL: %s", email, direct_url);

  if (out_advanced_url != NULL)
    *out_advanced_url = g_steal_pointer (&advanced_url);
  if (out_direct_url != NULL)
    *out_direct_url = g_steal_pointer (&direct_url);

  return TRUE;
}
