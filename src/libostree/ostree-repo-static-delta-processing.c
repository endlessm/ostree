/*
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

#include <string.h>

#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gio/gfiledescriptorbased.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-lzma-decompressor.h"
#include "otutil.h"
#include "ostree-varint.h"
#include "bsdiff/bspatch.h"

/* This should really always be true, but hey, let's just assert it */
G_STATIC_ASSERT (sizeof (guint) >= sizeof (guint32));

typedef struct {
  gboolean        stats_only;
  OstreeRepo     *repo;
  guint           checksum_index;
  const guint8   *checksums;
  guint           n_checksums;

  const guint8   *opdata;
  guint           oplen;

  GVariant       *mode_dict;
  GVariant       *xattr_dict;

  gboolean        object_start;
  gboolean        caught_error;
  GError        **async_error;

  OstreeObjectType output_objtype;
  guint64          content_size;
  char             checksum[OSTREE_SHA256_STRING_LEN+1];
  OstreeRepoBareContent content_out;
  char             *read_source_object;
  int               read_source_fd;
  gboolean        have_obj;
  guint32         uid;
  guint32         gid;
  guint32         mode;
  GVariant       *xattrs;

  const guint8   *output_target;
  const guint8   *input_target_csum;

  const guint8   *payload_data;
  guint64         payload_size;
} StaticDeltaExecutionState;

typedef struct {
  StaticDeltaExecutionState *state;
  char checksum[OSTREE_SHA256_STRING_LEN+1];
} StaticDeltaContentWrite;

typedef gboolean (*DispatchOpFunc) (OstreeRepo                 *repo,
                                    StaticDeltaExecutionState  *state,
                                    GCancellable               *cancellable,
                                    GError                    **error);

#define OPPROTO(name) \
  static gboolean dispatch_##name (OstreeRepo                 *repo, \
                                   StaticDeltaExecutionState  *state, \
                                   GCancellable               *cancellable, \
                                   GError                    **error);

OPPROTO(open_splice_and_close)
OPPROTO(open)
OPPROTO(write)
OPPROTO(set_read_source)
OPPROTO(unset_read_source)
OPPROTO(close)
OPPROTO(bspatch)
#undef OPPROTO

static void
static_delta_execution_state_init (StaticDeltaExecutionState  *state)
{
  state->read_source_fd = -1;
}

static gboolean
read_varuint64 (StaticDeltaExecutionState  *state,
                guint64                    *out_value,
                GError                    **error)
{
  gsize bytes_read;
  if (!_ostree_read_varuint64 (state->opdata, state->oplen, out_value, &bytes_read))
    {
      return glnx_throw (error, "%s", "Unexpected EOF reading varint");
    }
  state->opdata += bytes_read;
  state->oplen -= bytes_read;
  return TRUE;
}

static gboolean
open_output_target (StaticDeltaExecutionState   *state,
                    GCancellable                *cancellable,
                    GError                     **error)
{
  g_assert (state->checksums != NULL);
  g_assert (state->output_target == NULL);
  g_assert (state->checksum_index < state->n_checksums);

  guint8 *objcsum = (guint8*)state->checksums + (state->checksum_index * OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN);

  if (G_UNLIKELY(!ostree_validate_structureof_objtype (*objcsum, error)))
    return FALSE;

  state->output_objtype = (OstreeObjectType) *objcsum;
  state->output_target = objcsum + 1;

  ostree_checksum_inplace_from_bytes (state->output_target, state->checksum);

  return TRUE;
}

static guint
delta_opcode_index (OstreeStaticDeltaOpCode op)
{
  switch (op)
    {
    case OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE:
      return 0;
    case OSTREE_STATIC_DELTA_OP_OPEN:
      return 1;
    case OSTREE_STATIC_DELTA_OP_WRITE:
      return 2;
    case OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE:
      return 3;
    case OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE:
      return 4;
    case OSTREE_STATIC_DELTA_OP_CLOSE:
      return 5;
    case OSTREE_STATIC_DELTA_OP_BSPATCH:
      return 6;
    default:
      g_assert_not_reached ();
    }
}

gboolean
_ostree_static_delta_part_execute (OstreeRepo      *repo,
                                   GVariant        *objects,
                                   GVariant        *part,
                                   gboolean         stats_only,
                                   OstreeDeltaExecuteStats *stats,
                                   GCancellable    *cancellable,
                                   GError         **error)
{
  gboolean ret = FALSE;
  guint8 *checksums_data;
  g_autoptr(GVariant) mode_dict = NULL;
  g_autoptr(GVariant) xattr_dict = NULL;
  g_autoptr(GVariant) payload = NULL;
  g_autoptr(GVariant) ops = NULL;
  StaticDeltaExecutionState statedata = { 0, };
  StaticDeltaExecutionState *state = &statedata;
  guint n_executed = 0;

  static_delta_execution_state_init (&statedata);

  state->repo = repo;
  state->async_error = error;
  state->stats_only = stats_only;

  if (!_ostree_static_delta_parse_checksum_array (objects,
                                                  &checksums_data,
                                                  &state->n_checksums,
                                                  error))
    goto out;

  /* Skip processing for empty delta part */
  if (state->n_checksums == 0)
    {
      ret = TRUE;
      goto out;
    }

  state->checksums = checksums_data;

  g_variant_get (part, "(@a(uuu)@aa(ayay)@ay@ay)",
                 &mode_dict,
                 &xattr_dict,
                 &payload, &ops);

  state->mode_dict = mode_dict;
  state->xattr_dict = xattr_dict;

  state->payload_data = g_variant_get_data (payload);
  state->payload_size = g_variant_get_size (payload);

  state->oplen = g_variant_n_children (ops);
  state->opdata = g_variant_get_data (ops);

  while (state->oplen > 0)
    {
      guint8 opcode;

      opcode = state->opdata[0];
      state->oplen--;
      state->opdata++;

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      switch (opcode)
        {
        case OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE:
          if (!dispatch_open_splice_and_close (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_OPEN:
          if (!dispatch_open (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_WRITE:
          if (!dispatch_write (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE:
          if (!dispatch_set_read_source (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE:
          if (!dispatch_unset_read_source (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_CLOSE:
          if (!dispatch_close (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_BSPATCH:
          if (!dispatch_bspatch (repo, state, cancellable, error))
            goto out;
          break;
        default:
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Unknown opcode %u at offset %u", opcode, n_executed);
          goto out;
        }

      n_executed++;
      if (stats)
        stats->n_ops_executed[delta_opcode_index(opcode)]++;
    }

  if (state->caught_error)
    goto out;

  ret = TRUE;
 out:
  _ostree_repo_bare_content_cleanup (&state->content_out);
  return ret;
}

typedef struct {
  OstreeRepo *repo;
  GVariant *header;
  GVariant *part;
  GCancellable *cancellable;
} StaticDeltaPartExecuteAsyncData;

static void
static_delta_part_execute_async_data_free (gpointer user_data)
{
  StaticDeltaPartExecuteAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_variant_unref (data->header);
  g_variant_unref (data->part);
  g_clear_object (&data->cancellable);
  g_free (data);
}

static void
static_delta_part_execute_thread (GTask               *task,
                                  GObject             *object,
                                  gpointer             datap,
                                  GCancellable        *cancellable)
{
  GError *error = NULL;
  StaticDeltaPartExecuteAsyncData *data = datap;

  if (!_ostree_static_delta_part_execute (data->repo,
                                          data->header,
                                          data->part,
                                          FALSE, NULL,
                                          cancellable, &error))
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

void
_ostree_static_delta_part_execute_async (OstreeRepo      *repo,
                                         GVariant        *header,
                                         GVariant        *part,
                                         GCancellable    *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer         user_data)
{
  g_autoptr(GTask) task = NULL;
  StaticDeltaPartExecuteAsyncData *asyncdata;

  asyncdata = g_new0 (StaticDeltaPartExecuteAsyncData, 1);
  asyncdata->repo = g_object_ref (repo);
  asyncdata->header = g_variant_ref (header);
  asyncdata->part = g_variant_ref (part);
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  task = g_task_new (G_OBJECT (repo), cancellable, callback, user_data);
  g_task_set_task_data (task, asyncdata, (GDestroyNotify)static_delta_part_execute_async_data_free);
  g_task_set_source_tag (task, _ostree_static_delta_part_execute_async);
  g_task_run_in_thread (task, (GTaskThreadFunc)static_delta_part_execute_thread);
}

gboolean
_ostree_static_delta_part_execute_finish (OstreeRepo      *repo,
                                          GAsyncResult    *result,
                                          GError         **error)
{
  g_return_val_if_fail (OSTREE_IS_REPO (repo), FALSE);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (g_task_is_valid (result, repo), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_static_delta_part_execute_async), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static gboolean
validate_ofs (StaticDeltaExecutionState  *state,
              guint64                     offset,
              guint64                     length,
              GError                    **error)
{
  if (G_UNLIKELY (offset + length < offset ||
                  offset + length > state->payload_size))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid offset/length %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT,
                   offset, length);
      return FALSE;
    }
  return TRUE;
}

static gboolean
do_content_open_generic (OstreeRepo                 *repo,
                         StaticDeltaExecutionState  *state,
                         GCancellable               *cancellable,
                         GError                    **error)
{
  guint64 mode_offset;
  guint64 xattr_offset;

  if (!read_varuint64 (state, &mode_offset, error))
    return FALSE;
  if (!read_varuint64 (state, &xattr_offset, error))
    return FALSE;

  g_autoptr(GVariant) modev = g_variant_get_child_value (state->mode_dict, mode_offset);
  guint32 uid, gid, mode;
  g_variant_get (modev, "(uuu)", &uid, &gid, &mode);
  state->uid = GUINT32_FROM_BE (uid);
  state->gid = GUINT32_FROM_BE (gid);
  state->mode = GUINT32_FROM_BE (mode);

  state->xattrs = g_variant_get_child_value (state->xattr_dict, xattr_offset);

  return TRUE;
}

struct bzpatch_opaque_s
{
  StaticDeltaExecutionState  *state;
  guint64 offset, length;
};

static int
bspatch_read (const struct bspatch_stream* stream, void* buffer, int length)
{
  struct bzpatch_opaque_s *opaque = stream->opaque;

  g_assert (length <= opaque->length);
  g_assert (opaque->offset + length <= opaque->state->payload_size);

  memcpy (buffer, opaque->state->payload_data + opaque->offset, length);
  opaque->offset += length;
  opaque->length -= length;
  return 0;
}

static gboolean
dispatch_bspatch (OstreeRepo                 *repo,
                  StaticDeltaExecutionState  *state,
                  GCancellable               *cancellable,
                  GError                    **error)
{
  guint64 offset, length;

  if (!read_varuint64 (state, &offset, error))
    return FALSE;
  if (!read_varuint64 (state, &length, error))
    return FALSE;

  if (state->stats_only)
    return TRUE; /* Early return */

  if (!state->have_obj)
    {
      g_autoptr(GMappedFile) input_mfile = g_mapped_file_new_from_fd (state->read_source_fd, FALSE, error);
      if (!input_mfile)
        return FALSE;

      g_autofree guchar *buf = g_malloc0 (state->content_size);

      struct bzpatch_opaque_s opaque;
      opaque.state = state;
      opaque.offset = offset;
      opaque.length = length;
      struct bspatch_stream stream;
      stream.read = bspatch_read;
      stream.opaque = &opaque;
      if (bspatch ((const guint8*)g_mapped_file_get_contents (input_mfile),
                   g_mapped_file_get_length (input_mfile),
                   buf,
                   state->content_size,
                   &stream) < 0)
        return glnx_throw (error, "bsdiff patch failed");

      if (!_ostree_repo_bare_content_write (repo, &state->content_out,
                                            buf, state->content_size,
                                            cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
dispatch_open_splice_and_close (OstreeRepo                 *repo,
                                StaticDeltaExecutionState  *state,
                                GCancellable               *cancellable,
                                GError                    **error)
{
  gboolean ret = FALSE;

  if (!open_output_target (state, cancellable, error))
    goto out;

  if (OSTREE_OBJECT_TYPE_IS_META (state->output_objtype))
    {
      g_autoptr(GVariant) metadata = NULL;
      guint64 offset;
      guint64 length;

      if (!read_varuint64 (state, &length, error))
        goto out;
      if (!read_varuint64 (state, &offset, error))
        goto out;
      if (!validate_ofs (state, offset, length, error))
        goto out;

      if (state->stats_only)
        {
          ret = TRUE;
          goto out;
        }

      /* Unfortunately we need a copy because GVariant wants pointer-alignment
       * and we didn't guarantee that in static deltas. We can do so in the
       * future.
       */
      g_autoptr(GBytes) metadata_copy = g_bytes_new (state->payload_data + offset, length);
      metadata = g_variant_new_from_bytes (ostree_metadata_variant_type (state->output_objtype),
                                           metadata_copy, FALSE);

      {
        g_autofree guchar *actual_csum = NULL;

        if (!ostree_repo_write_metadata (state->repo, state->output_objtype,
                                         state->checksum,
                                         metadata, &actual_csum,
                                         cancellable,
                                         error))
          goto out;
      }
    }
  else
    {
      guint64 content_offset;
      guint64 objlen;
      g_autoptr(GInputStream) object_input = NULL;
      g_autoptr(GInputStream) memin = NULL;

      if (!do_content_open_generic (repo, state, cancellable, error))
        goto out;

      if (!read_varuint64 (state, &state->content_size, error))
        goto out;
      if (!read_varuint64 (state, &content_offset, error))
        goto out;
      if (!validate_ofs (state, content_offset, state->content_size, error))
        goto out;

      if (state->stats_only)
        {
          ret = TRUE;
          goto out;
        }

      /* Fast path for regular files to bare repositories */
      if (S_ISREG (state->mode) &&
          _ostree_repo_mode_is_bare (repo->mode))
        {
          if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_FILE, state->checksum,
                                       &state->have_obj, cancellable, error))
            goto out;

          if (!state->have_obj)
            {
              if (!_ostree_repo_bare_content_open (repo, state->checksum,
                                                   state->content_size,
                                                   state->uid, state->gid, state->mode,
                                                   state->xattrs,
                                                   &state->content_out,
                                                   cancellable, error))
                goto out;

              if (!_ostree_repo_bare_content_write (repo, &state->content_out,
                                                    state->payload_data + content_offset,
                                                    state->content_size,
                                                    cancellable, error))
                goto out;
            }
        }
      else
        {
          /* Slower path, for symlinks and unpacking deltas into archive */
          g_autoptr(GFileInfo) finfo =
            _ostree_mode_uidgid_to_gfileinfo (state->mode, state->uid, state->gid);

          if (S_ISLNK (state->mode))
            {
              g_autofree char *nulterminated_target =
                g_strndup ((char*)state->payload_data + content_offset, state->content_size);
              g_file_info_set_symlink_target (finfo, nulterminated_target);
            }
          else
            {
              g_assert (S_ISREG (state->mode));
              g_file_info_set_size (finfo, state->content_size);
              memin = g_memory_input_stream_new_from_data (state->payload_data + content_offset, state->content_size, NULL);
            }

          if (!ostree_raw_file_to_content_stream (memin, finfo, state->xattrs,
                                                  &object_input, &objlen,
                                                  cancellable, error))
            goto out;

          {
            g_autofree guchar *actual_csum = NULL;
            if (!ostree_repo_write_content (state->repo,
                                            state->checksum,
                                            object_input,
                                            objlen,
                                            &actual_csum,
                                            cancellable,
                                            error))
              goto out;
          }
        }
    }

  if (!dispatch_close (repo, state, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (state->stats_only)
    (void) dispatch_close (repo, state, cancellable, NULL);
  if (!ret)
    g_prefix_error (error, "opcode open-splice-and-close: ");
  return ret;
}

static gboolean
dispatch_open (OstreeRepo                 *repo,
               StaticDeltaExecutionState  *state,
               GCancellable               *cancellable,
               GError                    **error)
{
  GLNX_AUTO_PREFIX_ERROR("opcode open", error);

  g_assert (state->output_target == NULL);
  /* FIXME - lift this restriction */
  if (!state->stats_only)
    {
      g_assert (repo->mode == OSTREE_REPO_MODE_BARE ||
                repo->mode == OSTREE_REPO_MODE_BARE_USER ||
                repo->mode == OSTREE_REPO_MODE_BARE_USER_ONLY);
    }

  if (!open_output_target (state, cancellable, error))
    return FALSE;

  if (!do_content_open_generic (repo, state, cancellable, error))
    return FALSE;

  if (!read_varuint64 (state, &state->content_size, error))
    return FALSE;

  if (state->stats_only)
    return TRUE; /* Early return */

  if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_FILE, state->checksum,
                               &state->have_obj, cancellable, error))
    return FALSE;

  if (!state->have_obj)
    {
      if (!_ostree_repo_bare_content_open (repo, state->checksum,
                                           state->content_size,
                                           state->uid, state->gid, state->mode,
                                           state->xattrs,
                                           &state->content_out,
                                           cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
dispatch_write (OstreeRepo                 *repo,
               StaticDeltaExecutionState  *state,
               GCancellable               *cancellable,
               GError                    **error)
{
  GLNX_AUTO_PREFIX_ERROR("opcode write", error);
  guint64 content_size;
  guint64 content_offset;

  if (!read_varuint64 (state, &content_size, error))
    return FALSE;
  if (!read_varuint64 (state, &content_offset, error))
    return FALSE;

  if (state->stats_only)
    return TRUE; /* Early return */

  if (!state->have_obj)
    {
      if (state->read_source_fd != -1)
        {
          while (content_size > 0)
            {
              char buf[4096];
              gssize bytes_read;

              do
                bytes_read = pread (state->read_source_fd, buf, MIN(sizeof(buf), content_size), content_offset);
              while (G_UNLIKELY (bytes_read == -1 && errno == EINTR));
              if (bytes_read == -1)
                return glnx_throw_errno_prefix (error, "read");
              if (G_UNLIKELY (bytes_read == 0))
                return glnx_throw (error, "Unexpected EOF reading object %s", state->read_source_object);

              if (!_ostree_repo_bare_content_write (repo, &state->content_out,
                                                    (guint8*)buf, bytes_read,
                                                    cancellable, error))
                return FALSE;

              content_size -= bytes_read;
              content_offset += bytes_read;
            }
        }
      else
        {
          if (!validate_ofs (state, content_offset, content_size, error))
            return FALSE;

          if (!_ostree_repo_bare_content_write (repo, &state->content_out,
                                                state->payload_data + content_offset, content_size,
                                                cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
dispatch_set_read_source (OstreeRepo                 *repo,
                          StaticDeltaExecutionState  *state,
                          GCancellable               *cancellable,
                          GError                    **error)
{
  GLNX_AUTO_PREFIX_ERROR("opcode set-read-source", error);
  guint64 source_offset;

  glnx_close_fd (&state->read_source_fd);

  if (!read_varuint64 (state, &source_offset, error))
    return FALSE;
  if (!validate_ofs (state, source_offset, 32, error))
    return FALSE;

  if (state->stats_only)
    return TRUE; /* Early return */

  g_free (state->read_source_object);
  state->read_source_object = ostree_checksum_from_bytes (state->payload_data + source_offset);

  if (!_ostree_repo_load_file_bare (repo, state->read_source_object,
                                    &state->read_source_fd,
                                    NULL, NULL, NULL,
                                    cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
dispatch_unset_read_source (OstreeRepo                 *repo,
                            StaticDeltaExecutionState  *state,
                            GCancellable               *cancellable,
                            GError                    **error)
{
  GLNX_AUTO_PREFIX_ERROR("opcode unset-read-source", error);

  if (state->stats_only)
    return TRUE; /* Early return */

  glnx_close_fd (&state->read_source_fd);
  g_clear_pointer (&state->read_source_object, g_free);

  return TRUE;
}

static gboolean
dispatch_close (OstreeRepo                 *repo,
                StaticDeltaExecutionState  *state,
                GCancellable               *cancellable,
                GError                    **error)
{
  GLNX_AUTO_PREFIX_ERROR("opcode close", error);

  if (state->content_out.initialized)
    {
      char actual_checksum[OSTREE_SHA256_STRING_LEN+1];
      if (!_ostree_repo_bare_content_commit (repo, &state->content_out, actual_checksum,
                                             sizeof (actual_checksum),
                                             cancellable, error))
        return FALSE;

      g_assert_cmpstr (state->checksum, ==, actual_checksum);
    }

  if (!dispatch_unset_read_source (repo, state, cancellable, error))
    return FALSE;

  g_clear_pointer (&state->xattrs, g_variant_unref);
  _ostree_repo_bare_content_cleanup (&state->content_out);

  state->checksum_index++;
  state->output_target = NULL;

  return TRUE;
}
