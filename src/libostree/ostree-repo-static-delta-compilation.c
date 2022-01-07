/*
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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
#include <stdlib.h>
#include <gio/gunixoutputstream.h>
#include <gio/gmemoryoutputstream.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-lzma-compressor.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-diff.h"
#include "ostree-rollsum.h"
#include "otutil.h"
#include "libglnx.h"
#include "ostree-varint.h"
#include "bsdiff/bsdiff.h"
#include "ostree-autocleanups.h"
#include "ostree-sign.h"

#define CONTENT_SIZE_SIMILARITY_THRESHOLD_PERCENT (30)

typedef enum {
  DELTAOPT_FLAG_NONE = (1 << 0),
  DELTAOPT_FLAG_DISABLE_BSDIFF = (1 << 1),
  DELTAOPT_FLAG_VERBOSE = (1 << 2)
} DeltaOpts;

typedef struct {
  guint64 compressed_size;
  guint64 uncompressed_size;
  GPtrArray *objects;
  GString *payload;
  GString *operations;
  GHashTable *mode_set; /* GVariant(uuu) -> offset */
  GPtrArray *modes;
  GHashTable *xattr_set; /* GVariant(ayay) -> offset */
  GPtrArray *xattrs;
  GLnxTmpfile part_tmpf;
  GVariant *header;
} OstreeStaticDeltaPartBuilder;

typedef struct {
  GPtrArray *parts;
  GPtrArray *fallback_objects;
  guint64 loose_compressed_size;
  guint64 min_fallback_size_bytes;
  guint64 max_bsdiff_size_bytes;
  guint64 max_chunk_size_bytes;
  guint64 rollsum_size;
  guint n_rollsum;
  guint n_bsdiff;
  guint n_fallback;
  gboolean swap_endian;
  int parts_dfd;
  DeltaOpts delta_opts;
} OstreeStaticDeltaBuilder;

/* Get an input stream for a GVariant */
static GInputStream *
variant_to_inputstream (GVariant             *variant)
{
  GMemoryInputStream *ret = (GMemoryInputStream*)
    g_memory_input_stream_new_from_data (g_variant_get_data (variant),
                                         g_variant_get_size (variant),
                                         NULL);
  g_object_set_data_full ((GObject*)ret, "ot-variant-data",
                          g_variant_ref (variant), (GDestroyNotify) g_variant_unref);
  return (GInputStream*)ret;
}

static GBytes *
objtype_checksum_array_new (GPtrArray *objects)
{
  guint i;
  GByteArray *ret = g_byte_array_new ();

  for (i = 0; i < objects->len; i++)
    {
      GVariant *serialized_key = objects->pdata[i];
      OstreeObjectType objtype;
      const char *checksum;
      guint8 csum[OSTREE_SHA256_DIGEST_LEN];
      guint8 objtype_v;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);
      objtype_v = (guint8) objtype;

      ostree_checksum_inplace_to_bytes (checksum, csum);

      g_byte_array_append (ret, &objtype_v, 1);
      g_byte_array_append (ret, csum, sizeof (csum));
    }
  return g_byte_array_free_to_bytes (ret);
}

static void
ostree_static_delta_part_builder_unref (OstreeStaticDeltaPartBuilder *part_builder)
{
  if (part_builder->objects)
    g_ptr_array_unref (part_builder->objects);
  if (part_builder->payload)
    g_string_free (part_builder->payload, TRUE);
  if (part_builder->operations)
    g_string_free (part_builder->operations, TRUE);
  g_hash_table_unref (part_builder->mode_set);
  g_ptr_array_unref (part_builder->modes);
  g_hash_table_unref (part_builder->xattr_set);
  g_ptr_array_unref (part_builder->xattrs);
  glnx_tmpfile_clear (&part_builder->part_tmpf);
  if (part_builder->header)
    g_variant_unref (part_builder->header);
  g_free (part_builder);
}

static guint
mode_chunk_hash (const void *vp)
{
  GVariant *v = (GVariant*)vp;
  guint uid, gid, mode;
  g_variant_get (v, "(uuu)", &uid, &gid, &mode);
  return uid + gid + mode;
}

static gboolean
mode_chunk_equals (const void *one, const void *two)
{
  GVariant *v1 = (GVariant*)one;
  GVariant *v2 = (GVariant*)two;
  guint uid1, gid1, mode1;
  guint uid2, gid2, mode2;

  g_variant_get (v1, "(uuu)", &uid1, &gid1, &mode1);
  g_variant_get (v2, "(uuu)", &uid2, &gid2, &mode2);

  return uid1 == uid2 && gid1 == gid2 && mode1 == mode2;
}

static guint
bufhash (const void *b, gsize len)
{
  const signed char *p, *e;
  guint32 h = 5381;

  for (p = (signed char *)b, e = (signed char *)b + len; p != e; p++)
    h = (h << 5) + h + *p;

  return h;
}

static guint
xattr_chunk_hash (const void *vp)
{
  GVariant *v = (GVariant*)vp;
  gsize n = g_variant_n_children (v);
  guint i;
  guint32 h = 5381;

  for (i = 0; i < n; i++)
    {
      const guint8* name;
      const guint8* value_data;
      g_autoptr(GVariant) value = NULL;
      gsize value_len;

      g_variant_get_child (v, i, "(^&ay@ay)",
                           &name, &value);
      value_data = g_variant_get_fixed_array (value, &value_len, 1);

      h += g_str_hash (name);
      h += bufhash (value_data, value_len);
    }

  return h;
}

static gboolean
xattr_chunk_equals (const void *one, const void *two)
{
  GVariant *v1 = (GVariant*)one;
  GVariant *v2 = (GVariant*)two;
  gsize l1 = g_variant_get_size (v1);
  gsize l2 = g_variant_get_size (v2);

  if (l1 != l2)
    return FALSE;

  if (l1 == 0)
    return l2 == 0;

  return memcmp (g_variant_get_data (v1), g_variant_get_data (v2), l1) == 0;
}

static gboolean
finish_part (OstreeStaticDeltaBuilder *builder, GError **error)
{
  OstreeStaticDeltaPartBuilder *part_builder = builder->parts->pdata[builder->parts->len - 1];
  g_autofree guchar *part_checksum = NULL;
  g_autoptr(GBytes) objtype_checksum_array = NULL;
  g_autoptr(GBytes) checksum_bytes = NULL;
  g_autoptr(GOutputStream) part_temp_outstream = NULL;
  g_autoptr(GInputStream) part_in = NULL;
  g_autoptr(GInputStream) part_payload_in = NULL;
  g_autoptr(GMemoryOutputStream) part_payload_out = NULL;
  g_autoptr(GConverterOutputStream) part_payload_compressor = NULL;
  g_autoptr(GConverter) compressor = NULL;
  g_autoptr(GVariant) delta_part_content = NULL;
  g_autoptr(GVariant) delta_part = NULL;
  g_autoptr(GVariant) delta_part_header = NULL;
  g_auto(GVariantBuilder) mode_builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_auto(GVariantBuilder) xattr_builder = OT_VARIANT_BUILDER_INITIALIZER;
  guint8 compression_type_char;

  g_variant_builder_init (&mode_builder, G_VARIANT_TYPE ("a(uuu)"));
  g_variant_builder_init (&xattr_builder, G_VARIANT_TYPE ("aa(ayay)"));
  guint j;

  for (j = 0; j < part_builder->modes->len; j++)
    g_variant_builder_add_value (&mode_builder, part_builder->modes->pdata[j]);

  for (j = 0; j < part_builder->xattrs->len; j++)
    g_variant_builder_add_value (&xattr_builder, part_builder->xattrs->pdata[j]);

  {
    g_autoptr(GBytes) payload_b = g_string_free_to_bytes (g_steal_pointer (&part_builder->payload));
    g_autoptr(GBytes) operations_b = g_string_free_to_bytes (g_steal_pointer (&part_builder->operations));

    delta_part_content = g_variant_new ("(a(uuu)aa(ayay)@ay@ay)",
                                        &mode_builder, &xattr_builder,
                                        ot_gvariant_new_ay_bytes (payload_b),
                                        ot_gvariant_new_ay_bytes (operations_b));
    g_variant_ref_sink (delta_part_content);
  }

  /* Hardcode xz for now */
  compressor = (GConverter*)_ostree_lzma_compressor_new (NULL);
  compression_type_char = 'x';
  part_payload_in = variant_to_inputstream (delta_part_content);
  part_payload_out = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  part_payload_compressor = (GConverterOutputStream*)g_converter_output_stream_new ((GOutputStream*)part_payload_out, compressor);

  {
    gssize n_bytes_written = g_output_stream_splice ((GOutputStream*)part_payload_compressor, part_payload_in,
                                                     G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                                     NULL, error);
    if (n_bytes_written < 0)
      return FALSE;
  }

  g_clear_pointer (&delta_part_content, g_variant_unref);

  { g_autoptr(GBytes) payload = g_memory_output_stream_steal_as_bytes (part_payload_out);
    delta_part = g_variant_ref_sink (g_variant_new ("(y@ay)",
                                                    compression_type_char,
                                                    ot_gvariant_new_ay_bytes (payload)));
  }

  if (!glnx_open_tmpfile_linkable_at (builder->parts_dfd, ".", O_RDWR | O_CLOEXEC,
                                      &part_builder->part_tmpf, error))
    return FALSE;

  part_temp_outstream = g_unix_output_stream_new (part_builder->part_tmpf.fd, FALSE);

  part_in = variant_to_inputstream (delta_part);
  if (!ot_gio_splice_get_checksum (part_temp_outstream, part_in,
                                   &part_checksum,
                                   NULL, error))
    return FALSE;

  checksum_bytes = g_bytes_new (part_checksum, OSTREE_SHA256_DIGEST_LEN);
  objtype_checksum_array = objtype_checksum_array_new (part_builder->objects);
  delta_part_header = g_variant_new ("(u@aytt@ay)",
                                     maybe_swap_endian_u32 (builder->swap_endian, OSTREE_DELTAPART_VERSION),
                                     ot_gvariant_new_ay_bytes (checksum_bytes),
                                     maybe_swap_endian_u64 (builder->swap_endian, (guint64) g_variant_get_size (delta_part)),
                                     maybe_swap_endian_u64 (builder->swap_endian, part_builder->uncompressed_size),
                                     ot_gvariant_new_ay_bytes (objtype_checksum_array));
  g_variant_ref_sink (delta_part_header);

  part_builder->header = g_variant_ref (delta_part_header);
  part_builder->compressed_size = g_variant_get_size (delta_part);

  if (builder->delta_opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("part %u n:%u compressed:%" G_GUINT64_FORMAT " uncompressed:%" G_GUINT64_FORMAT "\n",
                  builder->parts->len, part_builder->objects->len,
                  part_builder->compressed_size,
                  part_builder->uncompressed_size);
    }

  return TRUE;
}

static OstreeStaticDeltaPartBuilder *
allocate_part (OstreeStaticDeltaBuilder *builder, GError **error)
{
  if (builder->parts->len > 0)
    {
      if (!finish_part (builder, error))
        return NULL;
    }

  OstreeStaticDeltaPartBuilder *part = g_new0 (OstreeStaticDeltaPartBuilder, 1);
  part->objects = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  part->payload = g_string_new (NULL);
  part->operations = g_string_new (NULL);
  part->uncompressed_size = 0;
  part->mode_set = g_hash_table_new_full (mode_chunk_hash, mode_chunk_equals,
                                          (GDestroyNotify)g_variant_unref, NULL);
  part->modes = g_ptr_array_new ();
  part->xattr_set = g_hash_table_new_full (xattr_chunk_hash, xattr_chunk_equals,
                                           (GDestroyNotify)g_variant_unref, NULL);
  part->xattrs = g_ptr_array_new ();
  g_ptr_array_add (builder->parts, part);
  return part;
}

static gsize
allocate_part_buffer_space (OstreeStaticDeltaPartBuilder  *current_part,
                            guint                          len)
{
  gsize empty_space;
  gsize old_len;

  old_len = current_part->payload->len;
  empty_space = current_part->payload->allocated_len - current_part->payload->len;

  if (empty_space < len)
    {
      gsize origlen;
      origlen = current_part->payload->len;
      g_string_set_size (current_part->payload, current_part->payload->allocated_len + (len - empty_space));
      current_part->payload->len = origlen;
    }

  return old_len;
}

static gsize
write_unique_variant_chunk (OstreeStaticDeltaPartBuilder *current_part,
                            GHashTable                   *hash,
                            GPtrArray                    *ordered,
                            GVariant                     *key)
{
  gpointer target_offsetp;
  gsize offset;

  if (g_hash_table_lookup_extended (hash, key, NULL, &target_offsetp))
    return GPOINTER_TO_UINT (target_offsetp);

  offset = ordered->len;
  target_offsetp = GUINT_TO_POINTER (offset);
  g_hash_table_insert (hash, g_variant_ref (key), target_offsetp);
  g_ptr_array_add (ordered, key);

  return offset;
}

static gboolean
splice_stream_to_payload (OstreeStaticDeltaPartBuilder  *current_part,
                          GInputStream                  *istream,
                          GCancellable                  *cancellable,
                          GError                       **error)
{
  while (TRUE)
    {
      const guint readlen = 4096;
      allocate_part_buffer_space (current_part, readlen);

      gsize bytes_read;
      if (!g_input_stream_read_all (istream,
                                    current_part->payload->str + current_part->payload->len,
                                    readlen,
                                    &bytes_read,
                                    cancellable, error))
        return FALSE;
      if (bytes_read == 0)
        break;

      current_part->payload->len += bytes_read;
    }

  return TRUE;
}

static void
write_content_mode_xattrs (OstreeRepo                       *repo,
                           OstreeStaticDeltaPartBuilder     *current_part,
                           GFileInfo                        *content_finfo,
                           GVariant                         *content_xattrs,
                           gsize                            *out_mode_offset,
                           gsize                            *out_xattr_offset)
{
  guint32 uid =
    g_file_info_get_attribute_uint32 (content_finfo, "unix::uid");
  guint32 gid =
    g_file_info_get_attribute_uint32 (content_finfo, "unix::gid");
  guint32 mode =
    g_file_info_get_attribute_uint32 (content_finfo, "unix::mode");
  g_autoptr(GVariant) modev
    = g_variant_ref_sink (g_variant_new ("(uuu)",
                                         GUINT32_TO_BE (uid),
                                         GUINT32_TO_BE (gid),
                                         GUINT32_TO_BE (mode)));

  *out_mode_offset = write_unique_variant_chunk (current_part,
                                                 current_part->mode_set,
                                                 current_part->modes,
                                                 modev);
  *out_xattr_offset = write_unique_variant_chunk (current_part,
                                                  current_part->xattr_set,
                                                  current_part->xattrs,
                                                  content_xattrs);
}

static gboolean
process_one_object (OstreeRepo                       *repo,
                    OstreeStaticDeltaBuilder         *builder,
                    OstreeStaticDeltaPartBuilder    **current_part_val,
                    const char                       *checksum,
                    OstreeObjectType                  objtype,
                    GCancellable                     *cancellable,
                    GError                          **error)
{
  OstreeStaticDeltaPartBuilder *current_part = *current_part_val;
  g_autoptr(GFileInfo) content_finfo = NULL;
  g_autoptr(GVariant) content_xattrs = NULL;
  guint64 content_size;
  g_autoptr(GInputStream) content_stream = NULL;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (!ostree_repo_load_object_stream (repo, objtype, checksum,
                                           &content_stream, &content_size,
                                           cancellable, error))
        return FALSE;
    }
  else
    {
      if (!ostree_repo_load_file (repo, checksum, &content_stream,
                                  &content_finfo, &content_xattrs,
                                  cancellable, error))
        return FALSE;
      content_size = g_file_info_get_size (content_finfo);
    }

  /* Check to see if this delta is maximum size */
  if (current_part->objects->len > 0 &&
      current_part->payload->len + content_size > builder->max_chunk_size_bytes)
    {
      current_part = allocate_part (builder, error);
      if (current_part == NULL)
        return FALSE;
      *current_part_val = current_part;
    }

  guint64 compressed_size;
  if (!ostree_repo_query_object_storage_size (repo, objtype, checksum,
                                              &compressed_size,
                                              cancellable, error))
    return FALSE;
  builder->loose_compressed_size += compressed_size;

  current_part->uncompressed_size += content_size;

  g_ptr_array_add (current_part->objects, ostree_object_name_serialize (checksum, objtype));

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      gsize object_payload_start;

      object_payload_start = current_part->payload->len;

      if (!splice_stream_to_payload (current_part, content_stream,
                                     cancellable, error))
        return FALSE;

      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE);
      _ostree_write_varuint64 (current_part->operations, content_size);
      _ostree_write_varuint64 (current_part->operations, object_payload_start);
    }
  else
    {
      gsize mode_offset, xattr_offset, content_offset;
      guint32 mode = g_file_info_get_attribute_uint32 (content_finfo, "unix::mode");

      write_content_mode_xattrs (repo, current_part, content_finfo, content_xattrs,
                                 &mode_offset, &xattr_offset);

      if (S_ISLNK (mode))
        {
          g_assert (content_stream == NULL);
          const char *target = g_file_info_get_symlink_target (content_finfo);
          content_stream =
            g_memory_input_stream_new_from_data (target, strlen (target), NULL);
          content_size = strlen (target);
        }
      else
        {
          g_assert (S_ISREG (mode));
        }

      content_offset = current_part->payload->len;
      if (!splice_stream_to_payload (current_part, content_stream,
                                     cancellable, error))
        return FALSE;

      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE);
      _ostree_write_varuint64 (current_part->operations, mode_offset);
      _ostree_write_varuint64 (current_part->operations, xattr_offset);
      _ostree_write_varuint64 (current_part->operations, content_size);
      _ostree_write_varuint64 (current_part->operations, content_offset);
    }

  return TRUE;
}

typedef struct {
  char *from_checksum;
} ContentBsdiff;

typedef struct {
  char *from_checksum;
  OstreeRollsumMatches *matches;
} ContentRollsum;

static void
content_rollsums_free (ContentRollsum  *rollsum)
{
  g_free (rollsum->from_checksum);
  _ostree_rollsum_matches_free (rollsum->matches);
  g_free (rollsum);
}

static void
content_bsdiffs_free (ContentBsdiff  *bsdiff)
{
  g_free (bsdiff->from_checksum);
  g_free (bsdiff);
}

/* Load a content object, uncompressing it to an unlinked tmpfile
   that's mmap()'d and suitable for seeking.
 */
static gboolean
get_unpacked_unlinked_content (OstreeRepo       *repo,
                               const char       *checksum,
                               GBytes          **out_content,
                               GCancellable     *cancellable,
                               GError          **error)
{
  g_autoptr(GInputStream) istream = NULL;

  if (!ostree_repo_load_file (repo, checksum, &istream, NULL, NULL,
                              cancellable, error))
    return FALSE;

  *out_content = ot_map_anonymous_tmpfile_from_content (istream, cancellable, error);
  if (!*out_content)
    return FALSE;
  return TRUE;
}

static gboolean
try_content_bsdiff (OstreeRepo                       *repo,
                    const char                       *from,
                    const char                       *to,
                    ContentBsdiff                    **out_bsdiff,
                    guint64                          max_bsdiff_size_bytes,
                    GCancellable                     *cancellable,
                    GError                           **error)
{


  g_autoptr(GFileInfo) from_finfo = NULL;
  if (!ostree_repo_load_file (repo, from, NULL, &from_finfo, NULL,
                              cancellable, error))
    return FALSE;
  g_autoptr(GFileInfo) to_finfo = NULL;
  if (!ostree_repo_load_file (repo, to, NULL, &to_finfo, NULL,
                              cancellable, error))
    return FALSE;

  *out_bsdiff = NULL;

  /* Ignore this if it's too large */
  if (g_file_info_get_size (to_finfo) + g_file_info_get_size (from_finfo) > max_bsdiff_size_bytes)
    return TRUE;

  ContentBsdiff *ret_bsdiff = g_new0 (ContentBsdiff, 1);
  ret_bsdiff->from_checksum = g_strdup (from);

  ot_transfer_out_value (out_bsdiff, &ret_bsdiff);
  return TRUE;
}

static gboolean
try_content_rollsum (OstreeRepo                       *repo,
                     DeltaOpts                        opts,
                     const char                       *from,
                     const char                       *to,
                     ContentRollsum                  **out_rollsum,
                     GCancellable                     *cancellable,
                     GError                          **error)
{
  *out_rollsum = NULL;

  /* Load the content objects, splice them to uncompressed temporary files that
   * we can just mmap() and seek around in conveniently.
   */
  g_autoptr(GBytes) tmp_from = NULL;
  if (!get_unpacked_unlinked_content (repo, from, &tmp_from, cancellable, error))
    return FALSE;
  g_autoptr(GBytes) tmp_to = NULL;
  if (!get_unpacked_unlinked_content (repo, to, &tmp_to, cancellable, error))
    return FALSE;

  g_autoptr(OstreeRollsumMatches) matches = _ostree_compute_rollsum_matches (tmp_from, tmp_to);

  const guint match_ratio = (matches->bufmatches*100)/matches->total;

  /* Only proceed if the file contains (arbitrary) more than 50% of
   * the previous chunks.
   */
  if (match_ratio < 50)
    return TRUE;

  if (opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("rollsum for %s -> %s; crcs=%u bufs=%u total=%u matchsize=%llu\n",
                  from, to, matches->crcmatches,
                  matches->bufmatches,
                  matches->total, (unsigned long long)matches->match_size);
    }

  ContentRollsum *ret_rollsum = g_new0 (ContentRollsum, 1);
  ret_rollsum->from_checksum = g_strdup (from);
  ret_rollsum->matches = g_steal_pointer (&matches);
  ot_transfer_out_value (out_rollsum, &ret_rollsum);
  return TRUE;
}

struct bzdiff_opaque_s
{
  GOutputStream *out;
  GCancellable *cancellable;
  GError **error;
};

static int
bzdiff_write (struct bsdiff_stream* stream, const void* buffer, int size)
{
  struct bzdiff_opaque_s *op = stream->opaque;
  if (!g_output_stream_write (op->out,
                              buffer,
                              size,
                              op->cancellable,
                              op->error))
    return -1;

  return 0;
}

static void
append_payload_chunk_and_write (OstreeStaticDeltaPartBuilder    *current_part,
                                const guint8                    *buf,
                                guint64                          offset)
{
  guint64 payload_start;

  payload_start = current_part->payload->len;
  g_string_append_len (current_part->payload, (char*)buf, offset);
  g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_WRITE);
  _ostree_write_varuint64 (current_part->operations, offset);
  _ostree_write_varuint64 (current_part->operations, payload_start);
}

static gboolean
process_one_rollsum (OstreeRepo                       *repo,
                     OstreeStaticDeltaBuilder         *builder,
                     OstreeStaticDeltaPartBuilder    **current_part_val,
                     const char                       *to_checksum,
                     ContentRollsum                   *rollsum,
                     GCancellable                     *cancellable,
                     GError                          **error)
{
  OstreeStaticDeltaPartBuilder *current_part = *current_part_val;

  /* Check to see if this delta has gone over maximum size */
  if (current_part->objects->len > 0 &&
      current_part->payload->len > builder->max_chunk_size_bytes)
    {
      current_part = allocate_part (builder, error);
      if (current_part == NULL)
        return FALSE;
      *current_part_val = current_part;
    }

  g_autoptr(GBytes) tmp_to = NULL;
  if (!get_unpacked_unlinked_content (repo, to_checksum, &tmp_to,
                                      cancellable, error))
    return FALSE;

  gsize tmp_to_len;
  const guint8 *tmp_to_buf = g_bytes_get_data (tmp_to, &tmp_to_len);

  g_autoptr(GFileInfo) content_finfo = NULL;
  g_autoptr(GVariant) content_xattrs = NULL;
  if (!ostree_repo_load_file (repo, to_checksum, NULL,
                              &content_finfo, &content_xattrs,
                              cancellable, error))
    return FALSE;
  guint64 content_size = g_file_info_get_size (content_finfo);
  g_assert_cmpint (tmp_to_len, ==, content_size);

  current_part->uncompressed_size += content_size;

  g_ptr_array_add (current_part->objects, ostree_object_name_serialize (to_checksum, OSTREE_OBJECT_TYPE_FILE));

  { gsize mode_offset, xattr_offset, from_csum_offset;
    gboolean reading_payload = TRUE;
    guchar source_csum[OSTREE_SHA256_DIGEST_LEN];
    guint i;

    write_content_mode_xattrs (repo, current_part, content_finfo, content_xattrs,
                               &mode_offset, &xattr_offset);

    /* Write the origin checksum */
    ostree_checksum_inplace_to_bytes (rollsum->from_checksum, source_csum);
    from_csum_offset = current_part->payload->len;
    g_string_append_len (current_part->payload, (char*)source_csum, sizeof (source_csum));

    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN);
    _ostree_write_varuint64 (current_part->operations, mode_offset);
    _ostree_write_varuint64 (current_part->operations, xattr_offset);
    _ostree_write_varuint64 (current_part->operations, content_size);

    { guint64 writing_offset = 0;
      guint64 offset = 0, to_start = 0, from_start = 0;
      GPtrArray *matchlist = rollsum->matches->matches;

      g_assert (matchlist->len > 0);
      for (i = 0; i < matchlist->len; i++)
        {
          GVariant *match = matchlist->pdata[i];
          guint32 crc;

          g_variant_get (match, "(uttt)", &crc, &offset, &to_start, &from_start);

          const guint64 prefix = to_start - writing_offset;

          if (prefix > 0)
            {
              if (!reading_payload)
                {
                  g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE);
                  reading_payload = TRUE;
                }

              g_assert_cmpint (writing_offset + prefix, <=, tmp_to_len);
              append_payload_chunk_and_write (current_part, tmp_to_buf + writing_offset, prefix);
              writing_offset += prefix;
            }

          if (reading_payload)
            {
              g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE);
              _ostree_write_varuint64 (current_part->operations, from_csum_offset);
              reading_payload = FALSE;
            }

          g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_WRITE);
          _ostree_write_varuint64 (current_part->operations, offset);
          _ostree_write_varuint64 (current_part->operations, from_start);
          writing_offset += offset;
        }

      if (!reading_payload)
        g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE);

      const guint64 remainder = tmp_to_len - writing_offset;
      if (remainder > 0)
        append_payload_chunk_and_write (current_part, tmp_to_buf + writing_offset, remainder);
      writing_offset += remainder;
      g_assert_cmpint (writing_offset, ==, tmp_to_len);
      g_assert_cmpint (writing_offset, ==, content_size);
    }


    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_CLOSE);
  }

  return TRUE;
}

static gboolean
process_one_bsdiff (OstreeRepo                       *repo,
                    OstreeStaticDeltaBuilder         *builder,
                    OstreeStaticDeltaPartBuilder    **current_part_val,
                    const char                       *to_checksum,
                    ContentBsdiff                   *bsdiff_content,
                    GCancellable                     *cancellable,
                    GError                          **error)
{
  OstreeStaticDeltaPartBuilder *current_part = *current_part_val;

  /* Check to see if this delta has gone over maximum size */
  if (current_part->objects->len > 0 &&
      current_part->payload->len > builder->max_chunk_size_bytes)
    {
      current_part = allocate_part (builder, error);
      if (current_part == NULL)
        return FALSE;
      *current_part_val = current_part;
    }

  g_autoptr(GBytes) tmp_from = NULL;
  if (!get_unpacked_unlinked_content (repo, bsdiff_content->from_checksum, &tmp_from,
                                      cancellable, error))
    return FALSE;
  g_autoptr(GBytes) tmp_to = NULL;
  if (!get_unpacked_unlinked_content (repo, to_checksum, &tmp_to,
                                      cancellable, error))
    return FALSE;

  gsize tmp_to_len;
  const guint8 *tmp_to_buf = g_bytes_get_data (tmp_to, &tmp_to_len);
  gsize tmp_from_len;
  const guint8 *tmp_from_buf = g_bytes_get_data (tmp_from, &tmp_from_len);

  g_autoptr(GFileInfo) content_finfo = NULL;
  g_autoptr(GVariant) content_xattrs = NULL;
  if (!ostree_repo_load_file (repo, to_checksum, NULL,
                              &content_finfo, &content_xattrs,
                              cancellable, error))
    return FALSE;
  const guint64 content_size = g_file_info_get_size (content_finfo);
  g_assert_cmpint (tmp_to_len, ==, content_size);

  current_part->uncompressed_size += content_size;

  g_ptr_array_add (current_part->objects, ostree_object_name_serialize (to_checksum, OSTREE_OBJECT_TYPE_FILE));

  { gsize mode_offset, xattr_offset;
    guchar source_csum[OSTREE_SHA256_DIGEST_LEN];

    write_content_mode_xattrs (repo, current_part, content_finfo, content_xattrs,
                               &mode_offset, &xattr_offset);

    /* Write the origin checksum */
    ostree_checksum_inplace_to_bytes (bsdiff_content->from_checksum, source_csum);

    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE);
    _ostree_write_varuint64 (current_part->operations, current_part->payload->len);
    g_string_append_len (current_part->payload, (char*)source_csum, sizeof (source_csum));

    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN);
    _ostree_write_varuint64 (current_part->operations, mode_offset);
    _ostree_write_varuint64 (current_part->operations, xattr_offset);
    _ostree_write_varuint64 (current_part->operations, content_size);

    {
      struct bsdiff_stream stream;
      struct bzdiff_opaque_s op;
      const gchar *payload;
      gssize payload_size;
      g_autoptr(GOutputStream) out = g_memory_output_stream_new_resizable ();
      stream.malloc = malloc;
      stream.free = free;
      stream.write = bzdiff_write;
      op.out = out;
      op.cancellable = cancellable;
      op.error = error;
      stream.opaque = &op;
      if (bsdiff (tmp_from_buf, tmp_from_len, tmp_to_buf, tmp_to_len, &stream) < 0)
        return glnx_throw (error, "bsdiff generation failed");

      payload = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (out));
      payload_size = g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (out));

      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_BSPATCH);
      _ostree_write_varuint64 (current_part->operations, current_part->payload->len);
      _ostree_write_varuint64 (current_part->operations, payload_size);

      /* A bit too verbose to print by default...but leaving this #if 0'd out to
       * use later. One approach I've been thinking about is to optionally
       * output e.g. a JSON file as we build the deltas. Alternatively, we could
       * try to reverse engineer things more in the "show" path, but that gets
       * hard/messy as it's quite optimized for execution now.
       */
#if 0
      g_printerr ("bspatch %s [%llu] → %s [%llu] bsdiff:%llu (%f)\n",
                  bsdiff_content->from_checksum, (unsigned long long)tmp_from_len,
                  to_checksum, (unsigned long long)tmp_to_len,
                  (unsigned long long)payload_size,
                  ((double)payload_size)/tmp_to_len);
#endif

      g_string_append_len (current_part->payload, payload, payload_size);
    }
    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_CLOSE);
  }

  g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE);

  return TRUE;
}

static gboolean
check_object_world_readable (OstreeRepo   *repo,
                             const char   *checksum,
                             gboolean     *out_readable,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GFileInfo) finfo = NULL;
  guint32 mode;

  if (!ostree_repo_load_file (repo, checksum, NULL, &finfo, NULL,
                              cancellable, error))
    return FALSE;

  mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
  *out_readable = (mode & S_IROTH);
  return TRUE;
}

static gboolean
generate_delta_lowlatency (OstreeRepo                       *repo,
                           const char                       *from,
                           const char                       *to,
                           DeltaOpts                         opts,
                           OstreeStaticDeltaBuilder         *builder,
                           GCancellable                     *cancellable,
                           GError                          **error)
{
  GHashTableIter hashiter;
  gpointer key, value;
  OstreeStaticDeltaPartBuilder *current_part = NULL;
  g_autoptr(GFile) root_from = NULL;
  g_autoptr(GVariant) from_commit = NULL;
  g_autoptr(GFile) root_to = NULL;
  g_autoptr(GVariant) to_commit = NULL;
  g_autoptr(GHashTable) to_reachable_objects = NULL;
  g_autoptr(GHashTable) from_reachable_objects = NULL;
  g_autoptr(GHashTable) new_reachable_metadata = NULL;
  g_autoptr(GHashTable) new_reachable_regfile_content = NULL;
  g_autoptr(GHashTable) new_reachable_symlink_content = NULL;
  g_autoptr(GHashTable) modified_regfile_content = NULL;
  g_autoptr(GHashTable) rollsum_optimized_content_objects = NULL;
  g_autoptr(GHashTable) bsdiff_optimized_content_objects = NULL;

  if (from != NULL)
    {
      if (!ostree_repo_read_commit (repo, from, &root_from, NULL,
                                    cancellable, error))
        return FALSE;

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, from,
                                     &from_commit, error))
        return FALSE;

      if (!ostree_repo_traverse_commit (repo, from, 0, &from_reachable_objects,
                                        cancellable, error))
        return FALSE;
    }

  if (!ostree_repo_read_commit (repo, to, &root_to, NULL,
                                cancellable, error))
    return FALSE;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, to,
                                 &to_commit, error))
    return FALSE;

  if (!ostree_repo_traverse_commit (repo, to, 0, &to_reachable_objects,
                                    cancellable, error))
    return FALSE;

  new_reachable_metadata = ostree_repo_traverse_new_reachable ();
  new_reachable_regfile_content = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  new_reachable_symlink_content = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);

  g_hash_table_iter_init (&hashiter, to_reachable_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      if (from_reachable_objects && g_hash_table_contains (from_reachable_objects, serialized_key))
        continue;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        g_hash_table_add (new_reachable_metadata, g_variant_ref (serialized_key));
      else
        {
          g_autoptr(GFileInfo) finfo = NULL;
          GFileType ftype;

          if (!ostree_repo_load_file (repo, checksum, NULL, &finfo, NULL,
                                      cancellable, error))
            return FALSE;

          ftype = g_file_info_get_file_type (finfo);
          if (ftype == G_FILE_TYPE_REGULAR)
            g_hash_table_add (new_reachable_regfile_content, g_strdup (checksum));
          else if (ftype == G_FILE_TYPE_SYMBOLIC_LINK)
            g_hash_table_add (new_reachable_symlink_content, g_strdup (checksum));
          else
            g_assert_not_reached ();
        }
    }

  if (from_commit)
    {
      if (!_ostree_delta_compute_similar_objects (repo, from_commit, to_commit,
                                                  new_reachable_regfile_content,
                                                  CONTENT_SIZE_SIMILARITY_THRESHOLD_PERCENT,
                                                  &modified_regfile_content,
                                                  cancellable, error))
        return FALSE;
    }
  else
    modified_regfile_content = g_hash_table_new (g_str_hash, g_str_equal);

  if (opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("modified: %u\n", g_hash_table_size (modified_regfile_content));
      g_printerr ("new reachable: metadata=%u content regular=%u symlink=%u\n",
                  g_hash_table_size (new_reachable_metadata),
                  g_hash_table_size (new_reachable_regfile_content),
                  g_hash_table_size (new_reachable_symlink_content));
    }

  /* We already ship the to commit in the superblock, don't ship it twice */
  { g_autoptr(GVariant) commit = ostree_object_name_serialize (to, OSTREE_OBJECT_TYPE_COMMIT);
    g_hash_table_remove (new_reachable_metadata, commit);
  }

  rollsum_optimized_content_objects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                             g_free,
                                                             (GDestroyNotify) content_rollsums_free);

  bsdiff_optimized_content_objects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            g_free,
                                                            (GDestroyNotify) content_bsdiffs_free);

  g_hash_table_iter_init (&hashiter, modified_regfile_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *to_checksum = key;
      const char *from_checksum = value;
      ContentRollsum *rollsum;
      ContentBsdiff *bsdiff;
      gboolean from_world_readable = FALSE;

      /* We only want to include in the delta objects that we are sure will
       * be readable by the client when applying the delta, regardless its
       * access privileges, so that we don't run into permissions problems
       * when the client is trying to update a bare-user repository with a
       * bare repository defined as its parent.
       */
      if (!check_object_world_readable (repo, from_checksum, &from_world_readable, cancellable, error))
        return FALSE;
      if (!from_world_readable)
        continue;

      if (!try_content_rollsum (repo, opts, from_checksum, to_checksum,
                                &rollsum, cancellable, error))
        return FALSE;

      if (rollsum)
        {
          g_hash_table_insert (rollsum_optimized_content_objects, g_strdup (to_checksum), rollsum);
          builder->rollsum_size += rollsum->matches->match_size;
          continue;
        }

      if (!(opts & DELTAOPT_FLAG_DISABLE_BSDIFF))
        {
          if (!try_content_bsdiff (repo, from_checksum, to_checksum,
                                   &bsdiff, builder->max_bsdiff_size_bytes,
                                   cancellable, error))
            return FALSE;

          if (bsdiff)
            g_hash_table_insert (bsdiff_optimized_content_objects, g_strdup (to_checksum), bsdiff);
        }
    }

  if (opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("rollsum for %u/%u modified\n",
                  g_hash_table_size (rollsum_optimized_content_objects),
                  g_hash_table_size (modified_regfile_content));
    }

  current_part = allocate_part (builder, error);
  if (current_part == NULL)
    return FALSE;

  /* Pack the metadata first */
  g_hash_table_iter_init (&hashiter, new_reachable_metadata);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!process_one_object (repo, builder, &current_part,
                               checksum, objtype,
                               cancellable, error))
        return FALSE;
    }

  /* Now do rollsummed objects */

  g_hash_table_iter_init (&hashiter, rollsum_optimized_content_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;
      ContentRollsum *rollsum = value;

      if (!process_one_rollsum (repo, builder, &current_part,
                                checksum, rollsum,
                                cancellable, error))
        return FALSE;

      builder->n_rollsum++;
    }

  /* Now do bsdiff'ed objects */

  const guint n_bsdiff = g_hash_table_size (bsdiff_optimized_content_objects);
  if (n_bsdiff > 0)
    {
      const guint mod = n_bsdiff / 10;
      g_hash_table_iter_init (&hashiter, bsdiff_optimized_content_objects);
      while (g_hash_table_iter_next (&hashiter, &key, &value))
        {
          const char *checksum = key;
          ContentBsdiff *bsdiff = value;

          if (opts & DELTAOPT_FLAG_VERBOSE &&
              (mod == 0 || builder->n_bsdiff % mod == 0))
            g_printerr ("processing bsdiff: [%u/%u]\n", builder->n_bsdiff, n_bsdiff);

          if (!process_one_bsdiff (repo, builder, &current_part,
                                   checksum, bsdiff,
                                   cancellable, error))
            return FALSE;

          builder->n_bsdiff++;
        }
    }

  /* Scan for large objects, so we can fall back to plain HTTP-based
   * fetch.
   */
  g_hash_table_iter_init (&hashiter, new_reachable_regfile_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;
      guint64 uncompressed_size;
      gboolean fallback = FALSE;

      /* Skip content objects we rollsum'd or bsdiff'ed */
      if (g_hash_table_contains (rollsum_optimized_content_objects, checksum) ||
          g_hash_table_contains (bsdiff_optimized_content_objects, checksum))
        continue;

      if (!ostree_repo_load_object_stream (repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                           NULL, &uncompressed_size,
                                           cancellable, error))
        return FALSE;
      if (builder->min_fallback_size_bytes > 0 &&
          uncompressed_size > builder->min_fallback_size_bytes)
        fallback = TRUE;

      if (fallback)
        {
          g_autofree char *size = g_format_size (uncompressed_size);

          if (opts & DELTAOPT_FLAG_VERBOSE)
            g_printerr ("fallback for %s (%s)\n", checksum, size);

          g_ptr_array_add (builder->fallback_objects,
                           ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_FILE));
          g_hash_table_iter_remove (&hashiter);
          builder->n_fallback++;
        }
    }

  /* Now non-rollsummed or bsdiff'ed regular file content */
  g_hash_table_iter_init (&hashiter, new_reachable_regfile_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;

      /* Skip content objects we rollsum'd */
      if (g_hash_table_contains (rollsum_optimized_content_objects, checksum) ||
          g_hash_table_contains (bsdiff_optimized_content_objects, checksum))
        continue;

      if (!process_one_object (repo, builder, &current_part,
                               checksum, OSTREE_OBJECT_TYPE_FILE,
                               cancellable, error))
        return FALSE;
    }

  /* Now symlinks */
  g_hash_table_iter_init (&hashiter, new_reachable_symlink_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;

      if (!process_one_object (repo, builder, &current_part,
                               checksum, OSTREE_OBJECT_TYPE_FILE,
                               cancellable, error))
        return FALSE;
    }

  if (!finish_part (builder, error))
    return FALSE;

  return TRUE;
}

static gboolean
get_fallback_headers (OstreeRepo               *self,
                      OstreeStaticDeltaBuilder *builder,
                      GVariant                **out_headers,
                      GCancellable             *cancellable,
                      GError                  **error)
{
  g_autoptr(GVariantBuilder) fallback_builder =
    g_variant_builder_new (G_VARIANT_TYPE ("a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT));

  for (guint i = 0; i < builder->fallback_objects->len; i++)
    {
      GVariant *serialized = builder->fallback_objects->pdata[i];
      const char *checksum;
      OstreeObjectType objtype;
      guint64 compressed_size;
      guint64 uncompressed_size;

      ostree_object_name_deserialize (serialized, &checksum, &objtype);

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (!ostree_repo_load_object_stream (self, objtype, checksum,
                                               NULL, &uncompressed_size,
                                               cancellable, error))
            return FALSE;
          compressed_size = uncompressed_size;
        }
      else
        {
          if (!ostree_repo_query_object_storage_size (self, OSTREE_OBJECT_TYPE_FILE,
                                                      checksum,
                                                      &compressed_size,
                                                      cancellable, error))
            return FALSE;

          g_autoptr(GFileInfo) file_info = NULL;
          if (!ostree_repo_load_file (self, checksum,
                                      NULL, &file_info, NULL,
                                      cancellable, error))
            return FALSE;

          uncompressed_size = g_file_info_get_size (file_info);
        }

      g_variant_builder_add_value (fallback_builder,
                                   g_variant_new ("(y@aytt)",
                                                  objtype,
                                                  ostree_checksum_to_bytes_v (checksum),
                                                  maybe_swap_endian_u64 (builder->swap_endian, compressed_size),
                                                  maybe_swap_endian_u64 (builder->swap_endian, uncompressed_size)));
    }

  g_autoptr(GVariant) ret_headers = g_variant_ref_sink (g_variant_builder_end (fallback_builder));
  ot_transfer_out_value (out_headers, &ret_headers);
  return TRUE;
}

/**
 * ostree_repo_static_delta_generate:
 * @self: Repo
 * @opt: High level optimization choice
 * @from: (nullable): ASCII SHA256 checksum of origin, or %NULL
 * @to: ASCII SHA256 checksum of target
 * @metadata: (nullable): Optional metadata
 * @params: (nullable): Parameters, see below
 * @cancellable: Cancellable
 * @error: Error
 *
 * Generate a lookaside "static delta" from @from (%NULL means
 * from-empty) which can generate the objects in @to.  This delta is
 * an optimization over fetching individual objects, and can be
 * conveniently stored and applied offline.
 *
 * The @params argument should be an a{sv}.  The following attributes
 * are known:
 *   - min-fallback-size: u: Minimum uncompressed size in megabytes to use fallback, 0 to disable fallbacks
 *   - max-chunk-size: u: Maximum size in megabytes of a delta part
 *   - max-bsdiff-size: u: Maximum size in megabytes to consider bsdiff compression
 *   for input files
 *   - compression: y: Compression type: 0=none, x=lzma, g=gzip
 *   - bsdiff-enabled: b: Enable bsdiff compression.  Default TRUE.
 *   - inline-parts: b: Put part data in header, to get a single file delta.  Default FALSE.
 *   - verbose: b: Print diagnostic messages.  Default FALSE.
 *   - endianness: b: Deltas use host byte order by default; this option allows choosing (G_BIG_ENDIAN or G_LITTLE_ENDIAN)
 *   - filename: ay: Save delta superblock to this filename, and parts in the same directory.  Default saves to repository.
 *   - sign-name: ay: Signature type to use.
 *   - sign-key-ids: as: Array of keys used to sign delta superblock.
 */
gboolean
ostree_repo_static_delta_generate (OstreeRepo                   *self,
                                   OstreeStaticDeltaGenerateOpt  opt,
                                   const char                   *from,
                                   const char                   *to,
                                   GVariant                     *metadata,
                                   GVariant                     *params,
                                   GCancellable                 *cancellable,
                                   GError                      **error)
{
  OstreeStaticDeltaBuilder builder = { 0, };
  guint i;
  guint min_fallback_size;
  guint max_bsdiff_size;
  guint max_chunk_size;
  DeltaOpts delta_opts = DELTAOPT_FLAG_NONE;
  guint64 total_compressed_size = 0;
  guint64 total_uncompressed_size = 0;
  g_autoptr(GVariantBuilder) part_headers = NULL;
  g_autoptr(GPtrArray) part_temp_paths = NULL;
  g_autoptr(GVariant) to_commit = NULL;
  const char *opt_filename;
  g_autofree char *descriptor_name = NULL;
  glnx_autofd int descriptor_dfd = -1;
  g_autoptr(GVariant) fallback_headers = NULL;
  g_autoptr(GVariant) detached = NULL;
  gboolean inline_parts;
  guint endianness = G_BYTE_ORDER;
  g_autoptr(GPtrArray) builder_parts = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_static_delta_part_builder_unref);
  g_autoptr(GPtrArray) builder_fallback_objects = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  g_auto(GLnxTmpfile) descriptor_tmpf = { 0, };
  g_autoptr(OtVariantBuilder) descriptor_builder = NULL;
  const char *opt_sign_name;
  const char **opt_key_ids;

  if (!g_variant_lookup (params, "min-fallback-size", "u", &min_fallback_size))
    min_fallback_size = 4;
  builder.min_fallback_size_bytes = ((guint64)min_fallback_size) * 1000 * 1000;

  if (!g_variant_lookup (params, "max-bsdiff-size", "u", &max_bsdiff_size))
    max_bsdiff_size = 128;
  builder.max_bsdiff_size_bytes = ((guint64)max_bsdiff_size) * 1000 * 1000;
  if (!g_variant_lookup (params, "max-chunk-size", "u", &max_chunk_size))
    max_chunk_size = 32;
  builder.max_chunk_size_bytes = ((guint64)max_chunk_size) * 1000 * 1000;

  (void) g_variant_lookup (params, "endianness", "u", &endianness);
  g_return_val_if_fail (endianness == G_BIG_ENDIAN || endianness == G_LITTLE_ENDIAN, FALSE);

  builder.swap_endian = endianness != G_BYTE_ORDER;
  builder.parts = builder_parts;
  builder.fallback_objects = builder_fallback_objects;

  { gboolean use_bsdiff;
    if (!g_variant_lookup (params, "bsdiff-enabled", "b", &use_bsdiff))
      use_bsdiff = TRUE;
    if (!use_bsdiff)
      delta_opts |= DELTAOPT_FLAG_DISABLE_BSDIFF;
  }

  { gboolean verbose;
    if (!g_variant_lookup (params, "verbose", "b", &verbose))
      verbose = FALSE;
    if (verbose)
      delta_opts |= DELTAOPT_FLAG_VERBOSE;
  }

  if (!g_variant_lookup (params, "inline-parts", "b", &inline_parts))
    inline_parts = FALSE;

  if (!g_variant_lookup (params, "filename", "^&ay", &opt_filename))
    opt_filename = NULL;

  if (!g_variant_lookup (params, "sign-name", "^&ay", &opt_sign_name))
    opt_sign_name = NULL;

  if (!g_variant_lookup (params, "sign-key-ids", "^a&s", &opt_key_ids))
    opt_key_ids = NULL;

  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, to,
                                 &to_commit, error))
    return FALSE;

  builder.delta_opts = delta_opts;

  if (opt_filename)
    {
      g_autofree char *dnbuf = g_strdup (opt_filename);
      const char *dn = dirname (dnbuf);
      descriptor_name = g_strdup (glnx_basename (opt_filename));

      if (!glnx_opendirat (AT_FDCWD, dn, TRUE, &descriptor_dfd, error))
        return FALSE;
    }
  else
    {
      g_autofree char *descriptor_relpath = _ostree_get_relative_static_delta_superblock_path (from, to);
      g_autofree char *dnbuf = g_strdup (descriptor_relpath);
      const char *dn = dirname (dnbuf);

      if (!glnx_shutil_mkdir_p_at (self->repo_dir_fd, dn, DEFAULT_DIRECTORY_MODE, cancellable, error))
        return FALSE;
      if (!glnx_opendirat (self->repo_dir_fd, dn, TRUE, &descriptor_dfd, error))
        return FALSE;

      descriptor_name = g_strdup (basename (descriptor_relpath));
    }
  builder.parts_dfd = descriptor_dfd;

  /* Ignore optimization flags */
  if (!generate_delta_lowlatency (self, from, to, delta_opts, &builder,
                                  cancellable, error))
    return FALSE;

  if (!glnx_open_tmpfile_linkable_at (descriptor_dfd, ".", O_RDWR | O_CLOEXEC,
                                      &descriptor_tmpf, error))
    return FALSE;

  descriptor_builder = ot_variant_builder_new (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT), descriptor_tmpf.fd);

  /* Open the metadata dict */
  if (!ot_variant_builder_open (descriptor_builder, G_VARIANT_TYPE ("a{sv}"), error))
    return FALSE;

  /* NOTE: Add user-supplied metadata first.  This is used by at least
   * flatpak as a way to provide MIME content sniffing, since the
   * metadata appears first in the file.
   */
  if (metadata != NULL)
    {
      GVariantIter iter;
      GVariant *item;

      g_variant_iter_init (&iter, metadata);
      while ((item = g_variant_iter_next_value (&iter)))
        {
          if (!ot_variant_builder_add_value (descriptor_builder, item, error))
            return FALSE;
          g_variant_unref (item);
        }
    }

  { guint8 endianness_char;

    switch (endianness)
      {
      case G_LITTLE_ENDIAN:
        endianness_char = 'l';
        break;
      case G_BIG_ENDIAN:
        endianness_char = 'B';
        break;
      default:
        g_assert_not_reached ();
      }
    if (!ot_variant_builder_add (descriptor_builder, error, "{sv}", "ostree.endianness", g_variant_new_byte (endianness_char)))
      return FALSE;
  }

  part_headers = g_variant_builder_new (G_VARIANT_TYPE ("a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT));
  part_temp_paths = g_ptr_array_new_with_free_func ((GDestroyNotify)glnx_tmpfile_clear);
  for (i = 0; i < builder.parts->len; i++)
    {
      OstreeStaticDeltaPartBuilder *part_builder = builder.parts->pdata[i];

      if (inline_parts)
        {
          g_autofree char *part_relpath = _ostree_get_relative_static_delta_part_path (from, to, i);

          lseek (part_builder->part_tmpf.fd, 0, SEEK_SET);

          if (!ot_variant_builder_open (descriptor_builder, G_VARIANT_TYPE ("{sv}"), error) ||
              !ot_variant_builder_add (descriptor_builder, error, "s", part_relpath) ||
              !ot_variant_builder_open (descriptor_builder, G_VARIANT_TYPE ("v"), error) ||
              !ot_variant_builder_add_from_fd (descriptor_builder, G_VARIANT_TYPE ("(yay)"), part_builder->part_tmpf.fd, part_builder->compressed_size, error) ||
              !ot_variant_builder_close (descriptor_builder, error) ||
              !ot_variant_builder_close (descriptor_builder, error))
            return FALSE;
        }
      else
        {
          g_autofree char *partstr = g_strdup_printf ("%u", i);

          if (fchmod (part_builder->part_tmpf.fd, 0644) < 0)
            return glnx_throw_errno_prefix (error, "fchmod");

          if (!glnx_link_tmpfile_at (&part_builder->part_tmpf, GLNX_LINK_TMPFILE_REPLACE,
                                     descriptor_dfd, partstr, error))
            return FALSE;
        }

      g_variant_builder_add_value (part_headers, part_builder->header);

      total_compressed_size += part_builder->compressed_size;
      total_uncompressed_size += part_builder->uncompressed_size;
    }

  if (!get_fallback_headers (self, &builder, &fallback_headers,
                             cancellable, error))
    return FALSE;

  if (!ostree_repo_read_commit_detached_metadata (self, to, &detached, cancellable, error))
    return FALSE;

  if (detached)
    {
      g_autofree char *detached_key = _ostree_get_relative_static_delta_path (from, to, "commitmeta");
      if (!ot_variant_builder_add (descriptor_builder, error, "{sv}", detached_key, detached))
        return FALSE;
    }

  /* Close metadata dict */
  if (!ot_variant_builder_close (descriptor_builder, error))
    return FALSE;

  /* Generate OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */
  {
    GDateTime *now = g_date_time_new_now_utc ();
    /* floating */ GVariant *from_csum_v =
      from ? ostree_checksum_to_bytes_v (from) : ot_gvariant_new_bytearray ((guchar *)"", 0);
    /* floating */ GVariant *to_csum_v =
      ostree_checksum_to_bytes_v (to);


    if (!ot_variant_builder_add (descriptor_builder, error, "t",
                                 GUINT64_TO_BE (g_date_time_to_unix (now))) ||
        !ot_variant_builder_add_value (descriptor_builder,
                                       from_csum_v, error) ||
        !ot_variant_builder_add_value (descriptor_builder,
                                       to_csum_v, error) ||
        !ot_variant_builder_add_value (descriptor_builder,
                                       to_commit, error) ||
        !ot_variant_builder_add_value (descriptor_builder,
                                       ot_gvariant_new_bytearray ((guchar*)"", 0), error) ||
        !ot_variant_builder_add_value (descriptor_builder,
                                       g_variant_builder_end (part_headers), error) ||
        !ot_variant_builder_add_value (descriptor_builder,
                                       fallback_headers, error))
      return FALSE;

    if (!ot_variant_builder_end (descriptor_builder, error))
      return FALSE;

    g_date_time_unref (now);
  }

  if (delta_opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("uncompressed=%" G_GUINT64_FORMAT " compressed=%" G_GUINT64_FORMAT " loose=%" G_GUINT64_FORMAT "\n",
                  total_uncompressed_size,
                  total_compressed_size,
                  builder.loose_compressed_size);
      g_printerr ("rollsum=%u objects, %" G_GUINT64_FORMAT " bytes\n",
                  builder.n_rollsum,
                  builder.rollsum_size);
      g_printerr ("bsdiff=%u objects\n", builder.n_bsdiff);
    }

  if (opt_sign_name != NULL && opt_key_ids != NULL)
    {
      g_autoptr(GBytes) tmpdata = NULL;
      g_autoptr(OstreeSign) sign = NULL;
      const gchar *signature_key = NULL;
      g_autoptr(GVariantBuilder) signature_builder = NULL;
      g_auto(GLnxTmpfile) descriptor_sign_tmpf = { 0, };
      g_autoptr(OtVariantBuilder) descriptor_sign_builder = NULL;

      lseek (descriptor_tmpf.fd, 0, SEEK_SET);
      tmpdata = glnx_fd_readall_bytes (descriptor_tmpf.fd, cancellable, error);
      if (!tmpdata)
        return FALSE;

      sign = ostree_sign_get_by_name (opt_sign_name, error);
      if (sign == NULL)
        return FALSE;

      signature_key = ostree_sign_metadata_key (sign);
      const gchar *signature_format = ostree_sign_metadata_format (sign);

      signature_builder = g_variant_builder_new (G_VARIANT_TYPE (signature_format));

      for (const char **iter = opt_key_ids; iter && *iter; iter++)
        {
          const char *keyid = *iter;
          g_autoptr(GVariant) secret_key = NULL;
          g_autoptr(GBytes) signature_bytes = NULL;

          secret_key = g_variant_new_string (keyid);
          if (!ostree_sign_set_sk (sign, secret_key, error))
              return FALSE;

          if (!ostree_sign_data (sign, tmpdata, &signature_bytes,
                                 NULL, error))
            return FALSE;

          g_variant_builder_add (signature_builder, "@ay", ot_gvariant_new_ay_bytes (signature_bytes));
        }

      if (!glnx_open_tmpfile_linkable_at (descriptor_dfd, ".", O_WRONLY | O_CLOEXEC,
                                          &descriptor_sign_tmpf, error))
        return FALSE;

      descriptor_sign_builder = ot_variant_builder_new (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SIGNED_FORMAT),
                                                        descriptor_sign_tmpf.fd);

      if (!ot_variant_builder_add (descriptor_sign_builder, error, "t",
                                   GUINT64_TO_BE (OSTREE_STATIC_DELTA_SIGNED_MAGIC)))
        return FALSE;
      if (!ot_variant_builder_add (descriptor_sign_builder, error, "@ay", ot_gvariant_new_ay_bytes (tmpdata)))
        return FALSE;
      if (!ot_variant_builder_open (descriptor_sign_builder, G_VARIANT_TYPE ("a{sv}"), error))
        return FALSE;
      if (!ot_variant_builder_add (descriptor_sign_builder, error, "{sv}",
                                   signature_key, g_variant_builder_end(signature_builder)))
        return FALSE;
      if (!ot_variant_builder_close (descriptor_sign_builder, error))
        return FALSE;

      if (!ot_variant_builder_end (descriptor_sign_builder, error))
        return FALSE;

      if (fchmod (descriptor_sign_tmpf.fd, 0644) < 0)
        return glnx_throw_errno_prefix (error, "fchmod");

      if (!glnx_link_tmpfile_at (&descriptor_sign_tmpf, GLNX_LINK_TMPFILE_REPLACE,
                                 descriptor_dfd, descriptor_name, error))
        return FALSE;
    }
  else
    {
      if (fchmod (descriptor_tmpf.fd, 0644) < 0)
        return glnx_throw_errno_prefix (error, "fchmod");

      if (!glnx_link_tmpfile_at (&descriptor_tmpf, GLNX_LINK_TMPFILE_REPLACE,
                                 descriptor_dfd, descriptor_name, error))
        return FALSE;
    }

  return TRUE;
}
