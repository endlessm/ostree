/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <gio/gfiledescriptorbased.h>
#include "libglnx.h"
#include "ostree.h"
#include "ostree-core-private.h"
#include "ostree-chain-input-stream.h"
#include "otutil.h"

#define ALIGN_VALUE(this, boundary) \
  (( ((unsigned long)(this)) + (((unsigned long)(boundary)) -1)) & (~(((unsigned long)(boundary))-1)))

static gboolean
file_header_parse (GVariant         *metadata,
                   GFileInfo       **out_file_info,
                   GVariant        **out_xattrs,
                   GError          **error);
static gboolean
zlib_file_header_parse (GVariant         *metadata,
                        GFileInfo       **out_file_info,
                        GVariant        **out_xattrs,
                        GError          **error);

/**
 * SECTION:ostree-core
 * @title: Core repository-independent functions
 * @short_description: Create, validate, and convert core data types
 *
 * These functions implement repository-independent algorithms for
 * operating on the core OSTree data formats, such as converting
 * #GFileInfo into a #GVariant.
 *
 * There are 4 types of objects; file, dirmeta, tree, and commit.  The
 * last 3 are metadata, and the file object is the only content object
 * type.
 *
 * All metadata objects are stored as #GVariant (big endian).  The
 * rationale for this is the same as that of the ext{2,3,4} family of
 * filesystems; most developers will be using LE, and so it's better
 * to continually test the BE->LE swap.
 *
 * The file object is a custom format in order to support streaming.
 */

const GVariantType *
ostree_metadata_variant_type (OstreeObjectType objtype)
{
  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_DIR_TREE:
      return OSTREE_TREE_GVARIANT_FORMAT;
    case OSTREE_OBJECT_TYPE_DIR_META:
      return OSTREE_DIRMETA_GVARIANT_FORMAT;
    case OSTREE_OBJECT_TYPE_COMMIT:
      return OSTREE_COMMIT_GVARIANT_FORMAT;
    default:
      g_assert_not_reached ();
    }
}

/**
 * ostree_validate_checksum_string:
 * @sha256: SHA256 hex string
 * @error: Error
 *
 * Use this function to see if input strings are checksums.
 *
 * Returns: %TRUE if @sha256 is a valid checksum string, %FALSE otherwise
 */
gboolean
ostree_validate_checksum_string (const char *sha256,
                                 GError    **error)
{
  return ostree_validate_structureof_checksum_string (sha256, error);
}

#define OSTREE_REF_FRAGMENT_REGEXP "[-._\\w\\d]+"
#define OSTREE_REF_REGEXP "(?:" OSTREE_REF_FRAGMENT_REGEXP "/)*" OSTREE_REF_FRAGMENT_REGEXP

/**
 * ostree_parse_refspec:
 * @refspec: A "refspec" string
 * @out_remote: (out) (allow-none): The remote name, or %NULL if the refspec refs to a local ref
 * @out_ref: (out) (allow-none): Name of ref
 * @error: Error
 *
 * Split a refspec like "gnome-ostree:gnome-ostree/buildmaster" into
 * two parts; @out_remote will be set to "gnome-ostree", and @out_ref
 * will be "gnome-ostree/buildmaster".
 *
 * If @refspec refers to a local ref, @out_remote will be %NULL.
 */
gboolean
ostree_parse_refspec (const char   *refspec,
                      char        **out_remote,
                      char        **out_ref,
                      GError      **error)
{
  gboolean ret = FALSE;
  GMatchInfo *match = NULL;
  g_autofree char *remote = NULL;

  static gsize regex_initialized;
  static GRegex *regex;

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^(" OSTREE_REF_FRAGMENT_REGEXP ":)?(" OSTREE_REF_REGEXP ")$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  if (!g_regex_match (regex, refspec, 0, &match))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid refspec %s", refspec);
      goto out;
    }

  remote = g_match_info_fetch (match, 1);
  if (*remote == '\0')
    {
      g_clear_pointer (&remote, g_free);
    }
  else
    {
      /* Trim the : */
      remote[strlen(remote)-1] = '\0';
    }

  ret = TRUE;

  if (out_remote)
    *out_remote = g_steal_pointer (&remote);
  if (out_ref != NULL)
    *out_ref = g_match_info_fetch (match, 2);
 out:
  if (match)
    g_match_info_unref (match);
  return ret;
}

/**
 * ostree_validate_rev:
 * @rev: A revision string
 * @error: Error
 *
 * Returns: %TRUE if @rev is a valid ref string
 */
gboolean
ostree_validate_rev (const char *rev,
                     GError **error)
{
  gboolean ret = FALSE;
  GMatchInfo *match = NULL;

  static gsize regex_initialized;
  static GRegex *regex;

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^" OSTREE_REF_REGEXP "$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  if (!g_regex_match (regex, rev, 0, &match))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid ref name %s", rev);
      goto out;
    }

  ret = TRUE;
 out:
  if (match)
    g_match_info_unref (match);
  return ret;
}

GVariant *
_ostree_file_header_new (GFileInfo         *file_info,
                         GVariant          *xattrs)
{
  guint32 uid;
  guint32 gid;
  guint32 mode;
  const char *symlink_target;
  GVariant *ret;
  g_autoptr(GVariant) tmp_xattrs = NULL;

  uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
  gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");
  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    symlink_target = g_file_info_get_symlink_target (file_info);
  else
    symlink_target = "";

  if (xattrs == NULL)
    tmp_xattrs = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));

  ret = g_variant_new ("(uuuus@a(ayay))", GUINT32_TO_BE (uid),
                       GUINT32_TO_BE (gid), GUINT32_TO_BE (mode), 0,
                       symlink_target, xattrs ? xattrs : tmp_xattrs);
  g_variant_ref_sink (ret);
  return ret;
}

/*
 * ostree_zlib_file_header_new:
 * @file_info: a #GFileInfo
 * @xattrs: (allow-none): Optional extended attribute array
 *
 * Returns: (transfer full): A new #GVariant containing file header for an archive-z2 repository
 */
GVariant *
_ostree_zlib_file_header_new (GFileInfo         *file_info,
                              GVariant          *xattrs)
{
  guint64 size;
  guint32 uid;
  guint32 gid;
  guint32 mode;
  const char *symlink_target;
  GVariant *ret;
  g_autoptr(GVariant) tmp_xattrs = NULL;

  size = g_file_info_get_size (file_info);
  uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
  gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");
  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    symlink_target = g_file_info_get_symlink_target (file_info);
  else
    symlink_target = "";

  if (xattrs == NULL)
    tmp_xattrs = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));

  ret = g_variant_new ("(tuuuus@a(ayay))",
                       GUINT64_TO_BE (size), GUINT32_TO_BE (uid),
                       GUINT32_TO_BE (gid), GUINT32_TO_BE (mode), 0,
                       symlink_target, xattrs ? xattrs : tmp_xattrs);
  g_variant_ref_sink (ret);
  return ret;
}

static gboolean
write_padding (GOutputStream    *output,
               guint             alignment,
               gsize             offset,
               gsize            *out_bytes_written,
               GChecksum        *checksum,
               GCancellable     *cancellable,
               GError          **error)
{
  gboolean ret = FALSE;
  guint bits;
  guint padding_len;
  guchar padding_nuls[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  if (alignment == 8)
    bits = ((offset) & 7);
  else
    bits = ((offset) & 3);

  if (bits > 0)
    {
      padding_len = alignment - bits;
      if (!ot_gio_write_update_checksum (output, (guchar*)padding_nuls, padding_len,
                                         out_bytes_written, checksum,
                                         cancellable, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

/*
 * _ostree_write_variant_with_size:
 * @output: Stream
 * @variant: A variant
 * @alignment_offset: Used to determine whether or not we should write padding bytes
 * @out_bytes_written: (out): Number of bytes written
 * @checksum: (allow-none): If provided, update with written data
 * @cancellable: Cancellable
 * @error: Error
 *
 * Use this function for serializing a chain of 1 or more variants
 * into a stream; the @alignment_offset parameter is used to ensure
 * that each variant begins on an 8-byte alignment so it can be safely
 * accessed.
 */
gboolean
_ostree_write_variant_with_size (GOutputStream      *output,
                                 GVariant           *variant,
                                 guint64             alignment_offset,
                                 gsize              *out_bytes_written,
                                 GChecksum          *checksum,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  gboolean ret = FALSE;
  guint64 variant_size;
  guint32 variant_size_u32_be;
  gsize bytes_written;
  gsize ret_bytes_written = 0;

  /* Write variant size */
  variant_size = g_variant_get_size (variant);
  g_assert (variant_size < G_MAXUINT32);
  variant_size_u32_be = GUINT32_TO_BE((guint32) variant_size);

  bytes_written = 0;
  if (!ot_gio_write_update_checksum (output, &variant_size_u32_be, 4,
                                     &bytes_written, checksum,
                                     cancellable, error))
    goto out;
  ret_bytes_written += bytes_written;
  alignment_offset += bytes_written;

  bytes_written = 0;
  /* Pad to offset of 8, write variant */
  if (!write_padding (output, 8, alignment_offset, &bytes_written, checksum,
                      cancellable, error))
    goto out;
  ret_bytes_written += bytes_written;

  bytes_written = 0;
  if (!ot_gio_write_update_checksum (output, g_variant_get_data (variant),
                                     variant_size, &bytes_written, checksum,
                                     cancellable, error))
    goto out;
  ret_bytes_written += bytes_written;

  ret = TRUE;
  if (out_bytes_written)
    *out_bytes_written = ret_bytes_written;
 out:
  return ret;
}

/*
 * write_file_header_update_checksum:
 * @out: Stream
 * @variant: A variant, should be a file header
 * @checksum: (allow-none): If provided, update with written data
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write a file header variant to the provided @out stream, optionally
 * updating @checksum.
 */
static gboolean
write_file_header_update_checksum (GOutputStream         *out,
                                   GVariant              *header,
                                   GChecksum             *checksum,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;

  if (!_ostree_write_variant_with_size (out, header, 0, &bytes_written, checksum,
                                        cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/*
 * header_and_input_to_stream:
 * @file_header: A file header
 * @input: File raw content stream
 * @out_input: (out): Serialized object stream
 * @out_header_size: (out): Length of the header
 * @cancellable: Cancellable
 * @error: Error
 *
 * Combines @file_header and @input into a single stream.
 */
static gboolean
header_and_input_to_stream (GVariant           *file_header,
                            GInputStream       *input,
                            GInputStream      **out_input,
                            guint64            *out_header_size,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gpointer header_data;
  gsize header_size;
  g_autoptr(GInputStream) ret_input = NULL;
  g_autoptr(GPtrArray) streams = NULL;
  g_autoptr(GOutputStream) header_out_stream = NULL;
  g_autoptr(GInputStream) header_in_stream = NULL;

  header_out_stream = g_memory_output_stream_new (NULL, 0, g_realloc, g_free);

  if (!_ostree_write_variant_with_size (header_out_stream, file_header, 0, NULL, NULL,
                                        cancellable, error))
    return FALSE;

  if (!g_output_stream_close (header_out_stream, cancellable, error))
    return FALSE;

  header_size = g_memory_output_stream_get_data_size ((GMemoryOutputStream*) header_out_stream);
  header_data = g_memory_output_stream_steal_data ((GMemoryOutputStream*) header_out_stream);
  header_in_stream = g_memory_input_stream_new_from_data (header_data, header_size, g_free);

  streams = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_ptr_array_add (streams, g_object_ref (header_in_stream));
  if (input)
    g_ptr_array_add (streams, g_object_ref (input));

  ret_input = (GInputStream*)ostree_chain_input_stream_new (streams);
  ot_transfer_out_value (out_input, &ret_input);
  if (out_header_size)
    *out_header_size = header_size;

  return TRUE;
}

/**
 * ostree_raw_file_to_archive_z2_stream:
 * @input: File raw content stream
 * @file_info: A file info
 * @xattrs: (allow-none): Optional extended attributes
 * @out_input: (out): Serialized object stream
 * @cancellable: Cancellable
 * @error: Error
 *
 * Convert from a "bare" file representation into an
 * OSTREE_OBJECT_TYPE_FILE stream suitable for ostree pull.
 */
gboolean
ostree_raw_file_to_archive_z2_stream (GInputStream       *input,
                                      GFileInfo          *file_info,
                                      GVariant           *xattrs,
                                      GInputStream      **out_input,
                                      GCancellable       *cancellable,
                                      GError            **error)
{
  g_autoptr(GVariant) file_header = NULL;
  g_autoptr(GInputStream) zlib_input = NULL;

  file_header = _ostree_zlib_file_header_new (file_info, xattrs);
  if (input != NULL)
    {
      g_autoptr(GConverter) zlib_compressor = NULL;

      zlib_compressor = G_CONVERTER (g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 9));
      zlib_input = g_converter_input_stream_new (input, zlib_compressor);
    }
  return header_and_input_to_stream (file_header,
                                     zlib_input,
                                     out_input,
                                     NULL,
                                     cancellable,
                                     error);
}

/**
 * ostree_raw_file_to_content_stream:
 * @input: File raw content stream
 * @file_info: A file info
 * @xattrs: (allow-none): Optional extended attributes
 * @out_input: (out): Serialized object stream
 * @out_length: (out): Length of stream
 * @cancellable: Cancellable
 * @error: Error
 *
 * Convert from a "bare" file representation into an
 * OSTREE_OBJECT_TYPE_FILE stream.  This is a fundamental operation
 * for writing data to an #OstreeRepo.
 */
gboolean
ostree_raw_file_to_content_stream (GInputStream       *input,
                                   GFileInfo          *file_info,
                                   GVariant           *xattrs,
                                   GInputStream      **out_input,
                                   guint64            *out_length,
                                   GCancellable       *cancellable,
                                   GError            **error)
{
  g_autoptr(GVariant) file_header = NULL;
  guint64 header_size;

  file_header = _ostree_file_header_new (file_info, xattrs);
  if (!header_and_input_to_stream (file_header,
                                   input,
                                   out_input,
                                   &header_size,
                                   cancellable,
                                   error))
    return FALSE;
  if (out_length)
    *out_length = header_size + g_file_info_get_size (file_info);
  return TRUE;
}

/**
 * ostree_content_stream_parse:
 * @compressed: Whether or not the stream is zlib-compressed
 * @input: Object content stream
 * @input_length: Length of stream
 * @trusted: If %TRUE, assume the content has been validated
 * @out_input: (out): The raw file content stream
 * @out_file_info: (out): Normal metadata 
 * @out_xattrs: (out): Extended attributes
 * @cancellable: Cancellable
 * @error: Error
 *
 * The reverse of ostree_raw_file_to_content_stream(); this function
 * converts an object content stream back into components.
 */
gboolean
ostree_content_stream_parse (gboolean                compressed,
                             GInputStream           *input,
                             guint64                 input_length,
                             gboolean                trusted,
                             GInputStream          **out_input,
                             GFileInfo             **out_file_info,
                             GVariant              **out_xattrs,
                             GCancellable           *cancellable,
                             GError                **error)
{
  gboolean ret = FALSE;
  guint32 archive_header_size;
  guchar dummy[4];
  gsize bytes_read;
  g_autoptr(GInputStream) ret_input = NULL;
  g_autoptr(GFileInfo) ret_file_info = NULL;
  g_autoptr(GVariant) ret_xattrs = NULL;
  g_autoptr(GVariant) file_header = NULL;
  g_autofree guchar *buf = NULL;

  if (!g_input_stream_read_all (input,
                                &archive_header_size, 4, &bytes_read,
                                cancellable, error))
    goto out;
  archive_header_size = GUINT32_FROM_BE (archive_header_size);
  if (archive_header_size > input_length)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "File header size %u exceeds size %" G_GUINT64_FORMAT,
                   (guint)archive_header_size, input_length);
      goto out;
    }
  if (archive_header_size == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "File header size is zero");
      goto out;
    }

  /* Skip over padding */
  if (!g_input_stream_read_all (input,
                                dummy, 4, &bytes_read,
                                cancellable, error))
    goto out;

  buf = g_malloc (archive_header_size);
  if (!g_input_stream_read_all (input, buf, archive_header_size, &bytes_read,
                                cancellable, error))
    goto out;
  file_header = g_variant_new_from_data (compressed ? _OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT : _OSTREE_FILE_HEADER_GVARIANT_FORMAT,
                                         buf, archive_header_size, trusted,
                                         g_free, buf);
  buf = NULL;

  if (compressed)
    {
      if (!zlib_file_header_parse (file_header,
                                   out_file_info ? &ret_file_info : NULL,
                                   out_xattrs ? &ret_xattrs : NULL,
                                   error))
        goto out;
    }
  else
    {
      if (!file_header_parse (file_header,
                              out_file_info ? &ret_file_info : NULL,
                              out_xattrs ? &ret_xattrs : NULL,
                              error))
        goto out;
      if (ret_file_info)
        g_file_info_set_size (ret_file_info, input_length - archive_header_size - 8);
    }
  
  if (g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR
      && out_input)
    {
      /* Give the input stream at its current position as return value;
       * assuming the caller doesn't seek, this should be fine.  We might
       * want to wrap it though in a non-seekable stream.
       **/
      if (compressed)
        {
          g_autoptr(GConverter) zlib_decomp = (GConverter*)g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW);
          ret_input = g_converter_input_stream_new (input, zlib_decomp);
        }
      else
        ret_input = g_object_ref (input);
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

/**
 * ostree_content_file_parse_at:
 * @compressed: Whether or not the stream is zlib-compressed
 * @parent_dfd: Directory file descriptor
 * @path: Subpath
 * @trusted: If %TRUE, assume the content has been validated
 * @out_input: (out): The raw file content stream
 * @out_file_info: (out): Normal metadata 
 * @out_xattrs: (out): Extended attributes
 * @cancellable: Cancellable
 * @error: Error
 *
 * A thin wrapper for ostree_content_stream_parse(); this function
 * converts an object content stream back into components.
 */
gboolean
ostree_content_file_parse_at (gboolean                compressed,
                              int                     parent_dfd,
                              const char             *path,
                              gboolean                trusted,
                              GInputStream          **out_input,
                              GFileInfo             **out_file_info,
                              GVariant              **out_xattrs,
                              GCancellable           *cancellable,
                              GError                **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  g_autoptr(GInputStream) file_input = NULL;
  g_autoptr(GInputStream) ret_input = NULL;
  g_autoptr(GFileInfo) ret_file_info = NULL;
  g_autoptr(GVariant) ret_xattrs = NULL;

  if (!ot_openat_read_stream (parent_dfd, path, TRUE, &file_input,
                              cancellable, error))
    goto out;
      
  if (!glnx_stream_fstat ((GFileDescriptorBased*)file_input, &stbuf, error))
    goto out;
  
  if (!ostree_content_stream_parse (compressed, file_input, stbuf.st_size, trusted,
                                    out_input ? &ret_input : NULL,
                                    &ret_file_info, &ret_xattrs,
                                    cancellable, error))
    goto out;
      
  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

/**
 * ostree_content_file_parse:
 * @compressed: Whether or not the stream is zlib-compressed
 * @content_path: Path to file containing content
 * @trusted: If %TRUE, assume the content has been validated
 * @out_input: (out): The raw file content stream
 * @out_file_info: (out): Normal metadata 
 * @out_xattrs: (out): Extended attributes
 * @cancellable: Cancellable
 * @error: Error
 *
 * A thin wrapper for ostree_content_stream_parse(); this function
 * converts an object content stream back into components.
 */
gboolean
ostree_content_file_parse (gboolean                compressed,
                           GFile                  *content_path,
                           gboolean                trusted,
                           GInputStream          **out_input,
                           GFileInfo             **out_file_info,
                           GVariant              **out_xattrs,
                           GCancellable           *cancellable,
                           GError                **error)
{
  return ostree_content_file_parse_at (compressed, AT_FDCWD,
                                       gs_file_get_path_cached (content_path),
                                       trusted,
                                       out_input, out_file_info, out_xattrs,
                                       cancellable, error);
}

/**
 * ostree_checksum_file_from_input:
 * @file_info: File information
 * @xattrs: (allow-none): Optional extended attributes
 * @in: (allow-none): File content, should be %NULL for symbolic links
 * @objtype: Object type
 * @out_csum: (out) (array fixed-size=32): Return location for binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Compute the OSTree checksum for a given input.
 */
gboolean
ostree_checksum_file_from_input (GFileInfo        *file_info,
                                 GVariant         *xattrs,
                                 GInputStream     *in,
                                 OstreeObjectType  objtype,
                                 guchar          **out_csum,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  gboolean ret = FALSE;
  g_autofree guchar *ret_csum = NULL;
  GChecksum *checksum = NULL;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (!ot_gio_splice_update_checksum (NULL, in, checksum, cancellable, error))
        goto out;
    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr(GVariant) dirmeta = ostree_create_directory_metadata (file_info, xattrs);
      g_checksum_update (checksum, g_variant_get_data (dirmeta),
                         g_variant_get_size (dirmeta));
      
    }
  else
    {
      g_autoptr(GVariant) file_header = NULL;

      file_header = _ostree_file_header_new (file_info, xattrs);

      if (!write_file_header_update_checksum (NULL, file_header, checksum,
                                              cancellable, error))
        goto out;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          if (!ot_gio_splice_update_checksum (NULL, in, checksum, cancellable, error))
            goto out;
        }
    }

  ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value (out_csum, &ret_csum);
 out:
  g_clear_pointer (&checksum, (GDestroyNotify)g_checksum_free);
  return ret;
}

/**
 * ostree_checksum_file:
 * @f: File path
 * @objtype: Object type
 * @out_csum: (out) (array fixed-size=32): Return location for binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Compute the OSTree checksum for a given file.
 */
gboolean
ostree_checksum_file (GFile            *f,
                      OstreeObjectType  objtype,
                      guchar          **out_csum,
                      GCancellable     *cancellable,
                      GError          **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  g_autofree guchar *ret_csum = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      in = (GInputStream*)g_file_read (f, cancellable, error);
      if (!in)
        goto out;
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      if (!glnx_dfd_name_get_all_xattrs (AT_FDCWD, gs_file_get_path_cached (f),
                                         &xattrs, cancellable, error))
        goto out;
    }

  if (!ostree_checksum_file_from_input (file_info, xattrs, in, objtype,
                                        &ret_csum, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  return ret;
}

typedef struct {
  GFile  *f;
  OstreeObjectType objtype;
  guchar *csum;
} ChecksumFileAsyncData;

static void
checksum_file_async_thread (GSimpleAsyncResult  *res,
                            GObject             *object,
                            GCancellable        *cancellable)
{
  GError *error = NULL;
  ChecksumFileAsyncData *data;
  guchar *csum = NULL;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_checksum_file (data->f, data->objtype, &csum, cancellable, &error))
    g_simple_async_result_take_error (res, error);
  else
    data->csum = csum;
}

static void
checksum_file_async_data_free (gpointer datap)
{
  ChecksumFileAsyncData *data = datap;

  g_object_unref (data->f);
  g_free (data->csum);
  g_free (data);
}
  
/**
 * ostree_checksum_file_async:
 * @f: File path
 * @objtype: Object type
 * @io_priority: Priority for operation, see %G_IO_PRIORITY_DEFAULT
 * @cancellable: Cancellable
 * @callback: Invoked when operation is complete
 * @user_data: Data for @callback
 *
 * Asynchronously compute the OSTree checksum for a given file;
 * complete with ostree_checksum_file_async_finish().
 */
void
ostree_checksum_file_async (GFile                 *f,
                            OstreeObjectType       objtype,
                            int                    io_priority,
                            GCancellable          *cancellable,
                            GAsyncReadyCallback    callback,
                            gpointer               user_data)
{
  GSimpleAsyncResult  *res;
  ChecksumFileAsyncData *data;

  data = g_new0 (ChecksumFileAsyncData, 1);
  data->f = g_object_ref (f);
  data->objtype = objtype;

  res = g_simple_async_result_new (G_OBJECT (f), callback, user_data, ostree_checksum_file_async);
  g_simple_async_result_set_op_res_gpointer (res, data, (GDestroyNotify)checksum_file_async_data_free);
  
  g_simple_async_result_run_in_thread (res, checksum_file_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

/**
 * ostree_checksum_file_async_finish:
 * @f: File path
 * @result: Async result
 * @out_csum: (out) (array fixed-size=32): Return location for binary checksum
 * @error: Error
 *
 * Finish computing the OSTree checksum for a given file; see
 * ostree_checksum_file_async().
 */
gboolean
ostree_checksum_file_async_finish (GFile          *f,
                                   GAsyncResult   *result,
                                   guchar        **out_csum,
                                   GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  ChecksumFileAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_checksum_file_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->csum;
  data->csum = NULL;
  return TRUE;
}

/**
 * ostree_create_directory_metadata:
 * @dir_info: a #GFileInfo containing directory information
 * @xattrs: (allow-none): Optional extended attributes
 *
 * Returns: (transfer full): A new #GVariant containing %OSTREE_OBJECT_TYPE_DIR_META
 */
GVariant *
ostree_create_directory_metadata (GFileInfo    *dir_info,
                                  GVariant     *xattrs)
{
  GVariant *ret_metadata = NULL;

  ret_metadata = g_variant_new ("(uuu@a(ayay))",
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::uid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::gid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::mode")),
                                xattrs ? xattrs : g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));
  g_variant_ref_sink (ret_metadata);

  return ret_metadata;
}

/* Create a randomly-named symbolic link in @tempdir which points to
 * @target.  The filename will be returned in @out_file.
 *
 * The reason this odd function exists is that the repo should only
 * contain objects in their final state.  For bare repositories, we
 * need to first create the symlink, then chown it, and apply all
 * extended attributes, before finally rename()ing it into place.
 *
 * Furthermore for checkouts, we use this to implement union mode
 * where we override existing files via tempfile+rename().
 */
gboolean
_ostree_make_temporary_symlink_at (int             tmp_dirfd,
                                   const char     *target,
                                   char          **out_name,
                                   GCancellable   *cancellable,
                                   GError        **error)
{
  gboolean ret = FALSE;
  char *tmpname = g_strdup ("tmplink.XXXXXX");
  guint i;
  const int max_attempts = 128;

  for (i = 0; i < max_attempts; i++)
    {
      glnx_gen_temp_name (tmpname);
      if (symlinkat (target, tmp_dirfd, tmpname) < 0)
        {
          if (errno == EEXIST)
            continue;
          else
            {
              int errsv = errno;
              g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                   g_strerror (errsv));
              goto out;
            }
        }
      else
        break;
    }
  if (i == max_attempts)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted attempts to open temporary file");
      goto out;
    }

  ret = TRUE;
  if (out_name)
    *out_name = g_steal_pointer (&tmpname);
 out:
  return ret;
}


/**
 * ostree_object_type_to_string:
 * @objtype: an #OstreeObjectType
 *
 * Serialize @objtype to a string; this is used for file extensions.
 */
const char *
ostree_object_type_to_string (OstreeObjectType objtype)
{
  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_FILE:
      return "file";
    case OSTREE_OBJECT_TYPE_DIR_TREE:
      return "dirtree";
    case OSTREE_OBJECT_TYPE_DIR_META:
      return "dirmeta";
    case OSTREE_OBJECT_TYPE_COMMIT:
      return "commit";
    case OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT:
      return "tombstone-commit";
    case OSTREE_OBJECT_TYPE_COMMIT_META:
      return "commitmeta";
    default:
      g_assert_not_reached ();
      return NULL;
    }
}

/**
 * ostree_object_type_from_string:
 * @str: A stringified version of #OstreeObjectType
 *
 * The reverse of ostree_object_type_to_string().
 */
OstreeObjectType
ostree_object_type_from_string (const char *str)
{
  if (!strcmp (str, "file"))
    return OSTREE_OBJECT_TYPE_FILE;
  else if (!strcmp (str, "dirtree"))
    return OSTREE_OBJECT_TYPE_DIR_TREE;
  else if (!strcmp (str, "dirmeta"))
    return OSTREE_OBJECT_TYPE_DIR_META;
  else if (!strcmp (str, "commit"))
    return OSTREE_OBJECT_TYPE_COMMIT;
  else if (!strcmp (str, "tombstone-commit"))
    return OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT;
  else if (!strcmp (str, "commitmeta"))
    return OSTREE_OBJECT_TYPE_COMMIT_META;
  g_assert_not_reached ();
  return 0;
}

/**
 * ostree_object_to_string:
 * @checksum: An ASCII checksum
 * @objtype: Object type
 *
 * Returns: A string containing both @checksum and a stringifed version of @objtype
 */
char *
ostree_object_to_string (const char *checksum,
                         OstreeObjectType objtype)
{
  return g_strconcat (checksum, ".", ostree_object_type_to_string (objtype), NULL);
}

/**
 * ostree_object_from_string:
 * @str: An ASCII checksum
 * @out_checksum: (out) (transfer full): Parsed checksum
 * @out_objtype: (out): Parsed object type
 *
 * Reverse ostree_object_to_string().
 */
void
ostree_object_from_string (const char *str,
                           gchar     **out_checksum,
                           OstreeObjectType *out_objtype)
{
  const char *dot;

  dot = strrchr (str, '.');
  g_assert (dot != NULL);
  *out_checksum = g_strndup (str, dot - str);
  *out_objtype = ostree_object_type_from_string (dot + 1);
}

/**
 * ostree_hash_object_name:
 * @a: A #GVariant containing a serialized object
 *
 * Use this function with #GHashTable and ostree_object_name_serialize().
 */
guint
ostree_hash_object_name (gconstpointer a)
{
  GVariant *variant = (gpointer)a;
  const char *checksum;
  OstreeObjectType objtype;
  gint objtype_int;
  
  ostree_object_name_deserialize (variant, &checksum, &objtype);
  objtype_int = (gint) objtype;
  return g_str_hash (checksum) + g_int_hash (&objtype_int);
}

/**
 * ostree_cmp_checksum_bytes:
 * @a: A binary checksum
 * @b: A binary checksum
 *
 * Compare two binary checksums, using memcmp().
 */
int
ostree_cmp_checksum_bytes (const guchar *a,
                           const guchar *b)
{
  return memcmp (a, b, OSTREE_SHA256_DIGEST_LEN);
}

/**
 * ostree_object_name_serialize:
 * @checksum: An ASCII checksum
 * @objtype: An object type
 *
 * Returns: (transfer floating): A new floating #GVariant containing checksum string and objtype
 */
GVariant *
ostree_object_name_serialize (const char *checksum,
                              OstreeObjectType objtype)
{
  g_assert (objtype >= OSTREE_OBJECT_TYPE_FILE
            && objtype <= OSTREE_OBJECT_TYPE_LAST);
  return g_variant_new ("(su)", checksum, (guint32)objtype);
}

/**
 * ostree_object_name_deserialize:
 * @variant: A #GVariant of type (su)
 * @out_checksum: (out) (transfer none): Pointer into string memory of @variant with checksum
 * @out_objtype: (out): Return object type
 *
 * Reverse ostree_object_name_serialize().  Note that @out_checksum is
 * only valid for the lifetime of @variant, and must not be freed.
 */
void
ostree_object_name_deserialize (GVariant         *variant,
                                const char      **out_checksum,
                                OstreeObjectType *out_objtype)
{
  guint32 objtype_u32;
  g_variant_get (variant, "(&su)", out_checksum, &objtype_u32);
  *out_objtype = (OstreeObjectType)objtype_u32;
}

/**
 * ostree_checksum_b64_inplace_to_bytes: (skip)
 * @checksum: (array fixed-size=32): An binary checksum of length 32
 * @buf: Output location, must be at least 45 bytes in length
 *
 * Overwrite the contents of @buf with stringified version of @csum.
 */
void
ostree_checksum_b64_inplace_to_bytes (const char *checksum,
                                      guchar     *buf)
{
  int state = 0;
  guint save = 0;
  char tmpbuf[44];
  int i;

  for (i = 0; i < 43; i++)
    {
      char c = checksum[i];
      if (c == '_')
        tmpbuf[i] = '/';
      else
        tmpbuf[i] = c;
    }
  tmpbuf[43] = '=';

  g_base64_decode_step (tmpbuf, sizeof (tmpbuf), (guchar *) buf, &state, &save);
}

/**
 * ostree_checksum_inplace_to_bytes:
 * @checksum: a SHA256 string
 * @buf: Output buffer with at least 32 bytes of space
 *
 * Convert @checksum from a string to binary in-place, without
 * allocating memory.  Use this function in hot code paths.
 */
void
ostree_checksum_inplace_to_bytes (const char *checksum,
                                  guchar     *buf)
{
  guint i;
  guint j;

  for (i = 0, j = 0; i < OSTREE_SHA256_DIGEST_LEN; i += 1, j += 2)
    {
      gint big, little;

      g_assert (checksum[j]);
      g_assert (checksum[j+1]);

      big = g_ascii_xdigit_value (checksum[j]);
      little = g_ascii_xdigit_value (checksum[j+1]);

      g_assert (big != -1);
      g_assert (little != -1);

      buf[i] = (big << 4) | little;
    }
}

/**
 * ostree_checksum_to_bytes:
 * @checksum: An ASCII checksum
 *
 * Returns: (transfer full) (array fixed-size=32): Binary checksum from @checksum of length 32; free with g_free().
 */
guchar *
ostree_checksum_to_bytes (const char *checksum)
{
  guchar *ret = g_malloc (OSTREE_SHA256_DIGEST_LEN);
  ostree_checksum_inplace_to_bytes (checksum, ret);
  return ret;
}

/**
 * ostree_checksum_to_bytes_v:
 * @checksum: An ASCII checksum
 *
 * Returns: (transfer full): New #GVariant of type ay with length 32
 */
GVariant *
ostree_checksum_to_bytes_v (const char *checksum)
{
  guchar result[OSTREE_SHA256_DIGEST_LEN];
  ostree_checksum_inplace_to_bytes (checksum, result);
  return ot_gvariant_new_bytearray ((guchar*)result, OSTREE_SHA256_DIGEST_LEN);
}

/**
 * ostree_checksum_b64_to_bytes:
 * @checksum: An ASCII checksum
 *
 * Returns: (transfer full) (array fixed-size=32): Binary version of @checksum.
 */
guchar *
ostree_checksum_b64_to_bytes (const char *checksum)
{
  guchar *ret = g_malloc (32);
  ostree_checksum_b64_inplace_to_bytes (checksum, ret);
  return ret;
}

/**
 * ostree_checksum_inplace_from_bytes: (skip)
 * @csum: (array fixed-size=32): An binary checksum of length 32
 * @buf: Output location, must be at least OSTREE_SHA256_STRING_LEN+1 bytes in length
 *
 * Overwrite the contents of @buf with stringified version of @csum.
 */
void
ostree_checksum_inplace_from_bytes (const guchar *csum,
                                    char         *buf)
{
  static const gchar hexchars[] = "0123456789abcdef";
  guint i, j;

  for (i = 0, j = 0; i < OSTREE_SHA256_DIGEST_LEN; i++, j += 2)
    {
      guchar byte = csum[i];
      buf[j] = hexchars[byte >> 4];
      buf[j+1] = hexchars[byte & 0xF];
    }
  buf[j] = '\0';
}

/**
 * ostree_checksum_b64_inplace_from_bytes: (skip)
 * @csum: (array fixed-size=32): An binary checksum of length 32
 * @buf: Output location, must be at least 44 bytes in length
 *
 * Overwrite the contents of @buf with modified base64 encoding of @csum.
 * The "modified" term refers to the fact that instead of '/', the '_'
 * character is used.
 */
void
ostree_checksum_b64_inplace_from_bytes (const guchar *csum,
                                        char         *buf)
{
  char tmpbuf[44];
  int save = 0;
  int state = 0;
  gsize outlen;
  int i;

  /* At some point, we can optimize this, but for now it's
   * a lot easier to reuse GLib's base64 encoder and postprocess it
   * to replace the '/' with '_'.
   */
  outlen = g_base64_encode_step (csum, OSTREE_SHA256_DIGEST_LEN, FALSE, tmpbuf, &state, &save);
  outlen += g_base64_encode_close (FALSE, tmpbuf+outlen, &state, &save);
  g_assert (outlen == 44);

  for (i = 0; i < sizeof (tmpbuf); i++)
    {
      char c = tmpbuf[i];
      if (c == '=')
        {
          g_assert (i == 43);
          buf[i] = '\0';
        }
      else if (c == '/')
        buf[i] = '_';
      else
        buf[i] = c;
    }
}

/**
 * ostree_checksum_from_bytes:
 * @csum: (array fixed-size=32): An binary checksum of length 32
 *
 * Returns: (transfer full): String form of @csum
 */
char *
ostree_checksum_from_bytes (const guchar *csum)
{
  char *ret = g_malloc (OSTREE_SHA256_STRING_LEN+1);
  ostree_checksum_inplace_from_bytes (csum, ret);
  return ret;
}

/**
 * ostree_checksum_from_bytes_v:
 * @csum_v: #GVariant of type ay
 *
 * Returns: (transfer full): String form of @csum_bytes
 */
char *
ostree_checksum_from_bytes_v (GVariant *csum_v)
{
  return ostree_checksum_from_bytes (ostree_checksum_bytes_peek (csum_v));
}

/**
 * ostree_checksum_b64_from_bytes:
 * @csum: (array fixed-size=32): An binary checksum of length 32
 *
 * Returns: (transfer full): Modified base64 encoding of @csum
 *
 * The "modified" term refers to the fact that instead of '/', the '_'
 * character is used.
 */
char *
ostree_checksum_b64_from_bytes (const guchar *csum)
{
  char *ret = g_malloc (44);
  ostree_checksum_b64_inplace_from_bytes (csum, ret);
  return ret;
}

/**
 * ostree_checksum_bytes_peek:
 * @bytes: #GVariant of type ay
 *
 * Returns: (transfer none) (array fixed-size=32) (element-type guint8): Binary checksum data in @bytes; do not free.  If @bytes does not have the correct length, return %NULL.
 */
const guchar *
ostree_checksum_bytes_peek (GVariant *bytes)
{
  gsize n_elts;
  const guchar *ret;
  ret = g_variant_get_fixed_array (bytes, &n_elts, 1);
  if (G_UNLIKELY (n_elts != OSTREE_SHA256_DIGEST_LEN))
    return NULL;
  return ret;
}

/**
 * ostree_checksum_bytes_peek_validate:
 * @bytes: #GVariant of type ay
 * @error: Errror
 *
 * Like ostree_checksum_bytes_peek(), but also throws @error.
 *
 * Returns: (transfer none) (array fixed-size=32) (element-type guint8): Binary checksum data
 */
const guchar *
ostree_checksum_bytes_peek_validate (GVariant  *bytes,
                                     GError   **error)
{
  const guchar *ret = ostree_checksum_bytes_peek (bytes);
  if (G_UNLIKELY (!ret))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid checksum of length %" G_GUINT64_FORMAT
                   " expected 32", (guint64) g_variant_n_children (bytes));
      return NULL;
    }
  return ret;
}

/*
 * _ostree_loose_path:
 * @buf: Output buffer, must be _OSTREE_LOOSE_PATH_MAX in size
 * @checksum: ASCII checksum
 * @objtype: Object type
 * @mode: Repository mode
 *
 * Overwrite the contents of @buf with relative path for loose
 * object.
 */
void
_ostree_loose_path (char              *buf,
                    const char        *checksum,
                    OstreeObjectType   objtype,
                    OstreeRepoMode     mode)
{
  *buf = checksum[0];
  buf++;
  *buf = checksum[1];
  buf++;
  snprintf (buf, _OSTREE_LOOSE_PATH_MAX - 2, "/%s.%s%s",
            checksum + 2, ostree_object_type_to_string (objtype),
            (!OSTREE_OBJECT_TYPE_IS_META (objtype) && mode == OSTREE_REPO_MODE_ARCHIVE_Z2) ? "z" : "");
}

/**
 * _ostree_header_gfile_info_new:
 * @mode: File mode
 * @uid: File uid
 * @gid: File gid
 *
 * OSTree only stores a subset of file attributes; for example,
 * timestamps are intentionally not stored.  This function creates a
 * #GFileInfo based on the attributes of a `struct stat` that match
 * those file attributes.
 *
 * Returns: (transfer full): A new #GFileInfo mapping a subset of @stbuf.
 */
GFileInfo *
_ostree_header_gfile_info_new (mode_t mode, uid_t uid, gid_t gid)
{
  GFileInfo *ret = g_file_info_new ();
  g_file_info_set_attribute_uint32 (ret, "standard::type", ot_gfile_type_for_mode (mode));
  g_file_info_set_attribute_boolean (ret, "standard::is-symlink", S_ISLNK (mode));
  g_file_info_set_attribute_uint32 (ret, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (ret, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (ret, "unix::mode", mode);
  return ret;
}

/*
 * _ostree_get_relative_object_path:
 * @checksum: ASCII checksum string
 * @type: Object type
 * @compressed: Whether or not the repository object is compressed
 *
 * Returns: (transfer full): Relative path for a loose object
 */
char *
_ostree_get_relative_object_path (const char         *checksum,
                                  OstreeObjectType    type,
                                  gboolean            compressed)
{
  GString *path;

  g_assert (strlen (checksum) == OSTREE_SHA256_STRING_LEN);

  path = g_string_new ("objects/");

  g_string_append_len (path, checksum, 2);
  g_string_append_c (path, '/');
  g_string_append (path, checksum + 2);
  g_string_append_c (path, '.');
  g_string_append (path, ostree_object_type_to_string (type));
  if (!OSTREE_OBJECT_TYPE_IS_META (type) && compressed)
    g_string_append (path, "z");

  return g_string_free (path, FALSE);
}

char *
_ostree_get_relative_static_delta_path (const char *from,
                                        const char *to,
                                        const char *target)
{
  guint8 csum_to[OSTREE_SHA256_DIGEST_LEN];
  char to_b64[44];
  guint8 csum_to_copy[OSTREE_SHA256_DIGEST_LEN];
  GString *ret = g_string_new ("deltas/");

  ostree_checksum_inplace_to_bytes (to, csum_to);
  ostree_checksum_b64_inplace_from_bytes (csum_to, to_b64);
  ostree_checksum_b64_inplace_to_bytes (to_b64, csum_to_copy);

  g_assert (memcmp (csum_to, csum_to_copy, OSTREE_SHA256_DIGEST_LEN) == 0);

  if (from != NULL)
    {
      guint8 csum_from[OSTREE_SHA256_DIGEST_LEN];
      char from_b64[44];

      ostree_checksum_inplace_to_bytes (from, csum_from);
      ostree_checksum_b64_inplace_from_bytes (csum_from, from_b64);

      g_string_append_c (ret, from_b64[0]);
      g_string_append_c (ret, from_b64[1]);
      g_string_append_c (ret, '/');
      g_string_append (ret, from_b64 + 2);
      g_string_append_c (ret, '-');
    }

  g_string_append_c (ret, to_b64[0]);
  g_string_append_c (ret, to_b64[1]);
  if (from == NULL)
    g_string_append_c (ret, '/');
  g_string_append (ret, to_b64 + 2);

  if (target != NULL)
    {
      g_string_append_c (ret, '/');
      g_string_append (ret, target);
    }
  
  return g_string_free (ret, FALSE);
}

char *
_ostree_get_relative_static_delta_superblock_path (const char        *from,
                                                   const char        *to)
{
  return _ostree_get_relative_static_delta_path (from, to, "superblock");
}

char *
_ostree_get_relative_static_delta_detachedmeta_path (const char        *from,
                                                     const char        *to)
{
  return _ostree_get_relative_static_delta_path (from, to, "meta");
}

char *
_ostree_get_relative_static_delta_part_path (const char        *from,
                                             const char        *to,
                                             guint              i)
{
  g_autofree char *partstr = g_strdup_printf ("%u", i);
  return _ostree_get_relative_static_delta_path (from, to, partstr);
}

gboolean
_ostree_parse_delta_name (const char   *delta_name,
                          char        **out_from,
                          char        **out_to,
                          GError      **error)
{
  g_auto(GStrv) parts = NULL;
  g_return_val_if_fail (delta_name != NULL, FALSE);

  parts = g_strsplit (delta_name, "-", 2);

  /* NB: if delta_name is "", parts[0] is NULL, but the error
   * validate_checksum_string() gives for "" is nice enough,
   * so we just coerce it here */
  if (!ostree_validate_checksum_string (parts[0] ?: "", error))
    return FALSE;

  if (parts[0] && parts[1] &&
      !ostree_validate_checksum_string (parts[1], error))
    return FALSE;

  *out_from = *out_to = NULL;
  if (parts[0] && parts[1])
    {
      ot_transfer_out_value (out_from, &parts[0]);
      ot_transfer_out_value (out_to, &parts[1]);
    }
  else
    {
      ot_transfer_out_value (out_to, &parts[0]);
    }

  return TRUE;
}

/*
 * file_header_parse:
 * @metadata: A metadata variant of type %OSTREE_FILE_HEADER_GVARIANT_FORMAT
 * @out_file_info: (out): Parsed file information
 * @out_xattrs: (out): Parsed extended attribute set
 * @error: Error
 *
 * Load file header information into standard Gio #GFileInfo object,
 * along with extended attributes tored in @out_xattrs.
 */
static gboolean
file_header_parse (GVariant         *metadata,
                   GFileInfo       **out_file_info,
                   GVariant        **out_xattrs,
                   GError          **error)
{
  gboolean ret = FALSE;
  guint32 uid, gid, mode, rdev;
  const char *symlink_target;
  g_autoptr(GFileInfo) ret_file_info = NULL;
  g_autoptr(GVariant) ret_xattrs = NULL;

  g_variant_get (metadata, "(uuuu&s@a(ayay))",
                 &uid, &gid, &mode, &rdev,
                 &symlink_target, &ret_xattrs);
  if (rdev != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted archive file; invalid rdev %u", GUINT32_FROM_BE (rdev));
      goto out;
    }

  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);

  ret_file_info = _ostree_header_gfile_info_new (mode, uid, gid);

  if (S_ISREG (mode))
    {
      ;
    }
  else if (S_ISLNK (mode))
    {
      g_file_info_set_attribute_byte_string (ret_file_info, "standard::symlink-target", symlink_target);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted archive file; invalid mode %u", mode);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_file_info, &ret_file_info);
  ot_transfer_out_value(out_xattrs, &ret_xattrs);
 out:
  return ret;
}

/*
 * zlib_file_header_parse:
 * @metadata: A metadata variant of type %OSTREE_FILE_HEADER_GVARIANT_FORMAT
 * @out_file_info: (out): Parsed file information
 * @out_xattrs: (out): Parsed extended attribute set
 * @error: Error
 *
 * Like ostree_file_header_parse(), but operates on zlib-compressed
 * content.
 */
static gboolean
zlib_file_header_parse (GVariant         *metadata,
                        GFileInfo       **out_file_info,
                        GVariant        **out_xattrs,
                        GError          **error)
{
  gboolean ret = FALSE;
  guint64 size;
  guint32 uid, gid, mode, rdev;
  const char *symlink_target;
  g_autoptr(GFileInfo) ret_file_info = NULL;
  g_autoptr(GVariant) ret_xattrs = NULL;

  g_variant_get (metadata, "(tuuuu&s@a(ayay))", &size,
                 &uid, &gid, &mode, &rdev,
                 &symlink_target, &ret_xattrs);
  if (rdev != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted archive file; invalid rdev %u", GUINT32_FROM_BE (rdev));
      goto out;
    }

  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  ret_file_info = _ostree_header_gfile_info_new (mode, uid, gid);

  g_file_info_set_size (ret_file_info, GUINT64_FROM_BE (size));

  if (S_ISREG (mode))
    {
      ;
    }
  else if (S_ISLNK (mode))
    {
      g_file_info_set_attribute_byte_string (ret_file_info, "standard::symlink-target", symlink_target);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted archive file; invalid mode %u", mode);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_file_info, &ret_file_info);
  ot_transfer_out_value(out_xattrs, &ret_xattrs);
 out:
  return ret;
}

/**
 * ostree_validate_structureof_objtype:
 * @objtype:
 * @error: Error
 *
 * Returns: %TRUE if @objtype represents a valid object type
 */
gboolean
ostree_validate_structureof_objtype (guchar    objtype,
                                     GError   **error)
{
  OstreeObjectType objtype_v = (OstreeObjectType) objtype;
  if (objtype_v < OSTREE_OBJECT_TYPE_FILE 
      || objtype_v > OSTREE_OBJECT_TYPE_COMMIT)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid object type '%u'", objtype);
      return FALSE;
    }
  return TRUE;
}

/**
 * ostree_validate_structureof_csum_v:
 * @checksum: a #GVariant of type "ay"
 * @error: Error
 *
 * Returns: %TRUE if @checksum is a valid binary SHA256 checksum
 */
gboolean
ostree_validate_structureof_csum_v (GVariant  *checksum,
                                    GError   **error)
{
  return ostree_checksum_bytes_peek_validate (checksum, error) != NULL;
}

/**
 * ostree_validate_structureof_checksum_string:
 * @checksum: an ASCII string
 * @error: Error
 *
 * Returns: %TRUE if @checksum is a valid ASCII SHA256 checksum
 */
gboolean
ostree_validate_structureof_checksum_string (const char *checksum,
                                             GError   **error)
{
  int i = 0;
  size_t len = strlen (checksum);

  if (len != OSTREE_SHA256_STRING_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid rev '%s'", checksum);
      return FALSE;
    }

  for (i = 0; i < len; i++)
    {
      guint8 c = ((guint8*) checksum)[i];

      if (!((c >= 48 && c <= 57)
            || (c >= 97 && c <= 102)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid character '%d' in rev '%s'",
                       c, checksum);
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
validate_variant (GVariant           *variant,
                  const GVariantType *variant_type,
                  GError            **error)
{
  if (!g_variant_is_normal_form (variant))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not normal form");
      return FALSE;
    }
  if (!g_variant_is_of_type (variant, variant_type))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Doesn't match variant type '%s'",
                   (char*)variant_type);
      return FALSE;
    }
  return TRUE;
}

/**
 * ostree_validate_structureof_commit:
 * @commit: A commit object, %OSTREE_OBJECT_TYPE_COMMIT
 * @error: Error
 * 
 * Use this to validate the basic structure of @commit, independent of
 * any other objects it references.
 *
 * Returns: %TRUE if @commit is structurally valid
 */
gboolean
ostree_validate_structureof_commit (GVariant      *commit,
                                    GError       **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) parent_csum_v = NULL;
  g_autoptr(GVariant) content_csum_v = NULL;
  g_autoptr(GVariant) metadata_csum_v = NULL;
  gsize n_elts;

  if (!validate_variant (commit, OSTREE_COMMIT_GVARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (commit, 1, "@ay", &parent_csum_v);
  (void) g_variant_get_fixed_array (parent_csum_v, &n_elts, 1);
  if (n_elts > 0)
    {
      if (!ostree_validate_structureof_csum_v (parent_csum_v, error))
        goto out;
    }

  g_variant_get_child (commit, 6, "@ay", &content_csum_v);
  if (!ostree_validate_structureof_csum_v (content_csum_v, error))
    goto out;

  g_variant_get_child (commit, 7, "@ay", &metadata_csum_v);
  if (!ostree_validate_structureof_csum_v (metadata_csum_v, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_validate_structureof_dirtree:
 * @dirtree: A dirtree object, %OSTREE_OBJECT_TYPE_DIR_TREE
 * @error: Error
 * 
 * Use this to validate the basic structure of @dirtree, independent of
 * any other objects it references.
 *
 * Returns: %TRUE if @dirtree is structurally valid
 */
gboolean
ostree_validate_structureof_dirtree (GVariant      *dirtree,
                                     GError       **error)
{
  gboolean ret = FALSE;
  const char *filename;
  g_autoptr(GVariant) content_csum_v = NULL;
  g_autoptr(GVariant) meta_csum_v = NULL;
  GVariantIter *contents_iter = NULL;

  if (!validate_variant (dirtree, OSTREE_TREE_GVARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (dirtree, 0, "a(say)", &contents_iter);

  while (g_variant_iter_loop (contents_iter, "(&s@ay)",
                              &filename, &content_csum_v))
    {
      if (!ot_util_filename_validate (filename, error))
        goto out;
      if (!ostree_validate_structureof_csum_v (content_csum_v, error))
        goto out;
    }
  content_csum_v = NULL;

  g_variant_iter_free (contents_iter);
  g_variant_get_child (dirtree, 1, "a(sayay)", &contents_iter);

  while (g_variant_iter_loop (contents_iter, "(&s@ay@ay)",
                              &filename, &content_csum_v, &meta_csum_v))
    {
      if (!ot_util_filename_validate (filename, error))
        goto out;
      if (!ostree_validate_structureof_csum_v (content_csum_v, error))
        goto out;
      if (!ostree_validate_structureof_csum_v (meta_csum_v, error))
        goto out;
    }
  content_csum_v = NULL;
  meta_csum_v = NULL;

  ret = TRUE;
 out:
  if (contents_iter)
    g_variant_iter_free (contents_iter);
  return ret;
}

static gboolean
validate_stat_mode_perms (guint32        mode,
                          GError       **error)
{
  gboolean ret = FALSE;
  guint32 otherbits = (~S_IFMT & ~S_IRWXU & ~S_IRWXG & ~S_IRWXO &
                       ~S_ISUID & ~S_ISGID & ~S_ISVTX);

  if (mode & otherbits)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode %u; invalid bits in mode", mode);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_validate_structureof_file_mode:
 * @mode: A Unix filesystem mode
 * @error: Error
 * 
 * Returns: %TRUE if @mode represents a valid file type and permissions
 */
gboolean
ostree_validate_structureof_file_mode (guint32            mode,
                                       GError           **error)
{
  gboolean ret = FALSE;

  if (!(S_ISREG (mode) || S_ISLNK (mode)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid file metadata mode %u; not a valid file type", mode);
      goto out;
    }

  if (!validate_stat_mode_perms (mode, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_validate_structureof_dirmeta:
 * @dirmeta: A dirmeta object, %OSTREE_OBJECT_TYPE_DIR_META
 * @error: Error
 * 
 * Use this to validate the basic structure of @dirmeta.
 *
 * Returns: %TRUE if @dirmeta is structurally valid
 */
gboolean
ostree_validate_structureof_dirmeta (GVariant      *dirmeta,
                                     GError       **error)
{
  gboolean ret = FALSE;
  guint32 mode;

  if (!validate_variant (dirmeta, OSTREE_DIRMETA_GVARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (dirmeta, 2, "u", &mode); 
  mode = GUINT32_FROM_BE (mode);

  if (!S_ISDIR (mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid directory metadata mode %u; not a directory", mode);
      goto out;
    }

  if (!validate_stat_mode_perms (mode, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_commit_get_parent:
 * @commit_variant: Variant of type %OSTREE_OBJECT_TYPE_COMMIT
 *
 * Returns: Checksum of the parent commit of @commit_variant, or %NULL
 * if none
 */
gchar *
ostree_commit_get_parent (GVariant  *commit_variant)
{
  g_autoptr(GVariant) bytes = NULL;
  bytes = g_variant_get_child_value (commit_variant, 1);
  if (g_variant_n_children (bytes) == 0)
    return NULL;
  return ostree_checksum_from_bytes_v (bytes);
}

guint64
ostree_commit_get_timestamp (GVariant  *commit_variant)
{
  guint64 ret;
  g_variant_get_child (commit_variant, 5, "t", &ret);
  return GUINT64_FROM_BE (ret);
}

GVariant *
_ostree_detached_metadata_append_gpg_sig (GVariant   *existing_metadata,
                                          GBytes     *signature_bytes)
{
  GVariantDict metadata_dict;
  g_autoptr(GVariant) signature_data = NULL;
  g_autoptr(GVariantBuilder) signature_builder = NULL;

  g_variant_dict_init (&metadata_dict, existing_metadata);

  signature_data = g_variant_dict_lookup_value (&metadata_dict,
                                                _OSTREE_METADATA_GPGSIGS_NAME,
                                                _OSTREE_METADATA_GPGSIGS_TYPE);

  /* signature_data may be NULL */
  signature_builder = ot_util_variant_builder_from_variant (signature_data, _OSTREE_METADATA_GPGSIGS_TYPE);

  g_variant_builder_add (signature_builder, "@ay", ot_gvariant_new_ay_bytes (signature_bytes));

  g_variant_dict_insert_value (&metadata_dict,
                               _OSTREE_METADATA_GPGSIGS_NAME,
                               g_variant_builder_end (signature_builder));

  return g_variant_dict_end (&metadata_dict);
}

/**
 * _ostree_get_default_sysroot_path:
 *
 * Returns a #GFile for the default system root, which is usually the root
 * directory ("/") unless overridden by the %OSTREE_SYSROOT environment
 * variable.
 *
 * Returns: a #GFile for the default system root
 */
GFile *
_ostree_get_default_sysroot_path (void)
{
  static gsize default_sysroot_path_initialized;
  static GFile *default_sysroot_path;

  if (g_once_init_enter (&default_sysroot_path_initialized))
    {
      const char *path = g_getenv ("OSTREE_SYSROOT");
      if (path == NULL || *path == '\0')
        path = "/";
      default_sysroot_path = g_file_new_for_path (path);
      g_once_init_leave (&default_sysroot_path_initialized, 1);
    }

  return default_sysroot_path;
}
