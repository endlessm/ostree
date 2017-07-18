/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
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
 * Author: Stef Walter <stefw@redhat.com>
 *         Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <err.h>

#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ot-dump.h"
#include "otutil.h"
#include "ot-admin-functions.h"

void
ot_dump_variant (GVariant *variant)
{
  g_autofree char *formatted_variant = NULL;
  g_autoptr(GVariant) byteswapped = NULL;

  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      byteswapped = g_variant_byteswap (variant);
      formatted_variant = g_variant_print (byteswapped, TRUE);
    }
  else
    {
      formatted_variant = g_variant_print (variant, TRUE);
    }
  g_print ("%s\n", formatted_variant);
}

static gchar *
format_timestamp (guint64  timestamp,
                  GError **error)
{
  GDateTime *dt;
  gchar *str;

  dt = g_date_time_new_from_unix_utc (timestamp);
  if (dt == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid timestamp: %" G_GUINT64_FORMAT, timestamp);
      return NULL;
    }

  str = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S +0000");
  g_date_time_unref (dt);

  return str;
}

static gchar *
uint64_secs_to_iso8601 (guint64 secs)
{
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc (secs);
  g_autoptr(GDateTime) local = (dt != NULL) ? g_date_time_to_local (dt) : NULL;

  if (local != NULL)
    return g_date_time_format (local, "%FT%T%:::z");
  else
    return g_strdup ("invalid");
}

static void
dump_indented_lines (const gchar *data)
{
  const char* indent = "    ";
  const gchar *pos;

  for (;;)
    {
      pos = strchr (data, '\n');
      if (pos)
        {
          g_print ("%s%.*s", indent, (int)(pos + 1 - data), data);
          data = pos + 1;
        }
      else
        {
          if (data[0] != '\0')
            g_print ("%s%s\n", indent, data);
          break;
        }
    }
}

static void
dump_commit (GVariant            *variant,
             OstreeDumpFlags      flags)
{
  const gchar *subject;
  const gchar *body;
  guint64 timestamp;
  g_autofree char *str = NULL;
  g_autofree char *version = NULL;
  g_autoptr(GError) local_error = NULL;

  /* See OSTREE_COMMIT_GVARIANT_FORMAT */
  g_variant_get (variant, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                 &subject, &body, &timestamp, NULL, NULL);

  timestamp = GUINT64_FROM_BE (timestamp);
  str = format_timestamp (timestamp, &local_error);
  if (!str)
    errx (1, "Failed to read commit: %s", local_error->message);
  g_print ("Date:  %s\n", str);

  if ((version = ot_admin_checksum_version (variant)))
    {
      g_print ("Version: %s\n", version);
    }

  if (subject[0])
    {
      g_print ("\n");
      dump_indented_lines (subject);
    }
  else
    {
      g_print ("(no subject)\n");
    }

  if (body[0])
    {
      g_print ("\n");
      dump_indented_lines (body);
    }
  g_print ("\n");
}

void
ot_dump_object (OstreeObjectType   objtype,
                const char        *checksum,
                GVariant          *variant,
                OstreeDumpFlags    flags)
{
  g_print ("%s %s\n", ostree_object_type_to_string (objtype), checksum);

  if (flags & OSTREE_DUMP_RAW)
    {
      ot_dump_variant (variant);
      return;
    }

  switch (objtype)
  {
    case OSTREE_OBJECT_TYPE_COMMIT:
      dump_commit (variant, flags);
      break;
    /* TODO: Others could be implemented here */
    default:
      break;
  }
}

static void
dump_summary_ref (const char   *collection_id,
                  const char   *ref_name,
                  guint64       commit_size,
                  GVariant     *csum_v,
                  GVariantIter *metadata)
{
  const guchar *csum_bytes;
  GError *csum_error = NULL;
  g_autofree char *size = NULL;
  GVariant *value;
  char *key;

  if (collection_id == NULL)
    g_print ("* %s\n", ref_name);
  else
    g_print ("* (%s, %s)\n", collection_id, ref_name);

  size = g_format_size (commit_size);
  g_print ("    Latest Commit (%s):\n", size);

  csum_bytes = ostree_checksum_bytes_peek_validate (csum_v, &csum_error);
  if (csum_error == NULL)
    {
      char csum[OSTREE_SHA256_STRING_LEN+1];

      ostree_checksum_inplace_from_bytes (csum_bytes, csum);
      g_print ("      %s\n", csum);
    }
  else
    {
      g_print ("      %s\n", csum_error->message);
      g_clear_error (&csum_error);
    }

  while (g_variant_iter_loop (metadata, "{sv}", &key, &value))
    {
      g_autofree gchar *value_str = NULL;
      const gchar *pretty_key = NULL;

      if (g_strcmp0 (key, OSTREE_COMMIT_TIMESTAMP) == 0)
        {
          pretty_key = "Timestamp";
          value_str = uint64_secs_to_iso8601 (GUINT64_FROM_BE (g_variant_get_uint64 (value)));
        }
      else
        {
          value_str = g_variant_print (value, FALSE);
        }

      /* Print out. */
      if (pretty_key != NULL)
        g_print ("    %s (%s): %s\n", pretty_key, key, value_str);
      else
        g_print ("    %s: %s\n", key, value_str);
    }
}

static void
dump_summary_refs (const gchar *collection_id,
                   GVariant    *refs)
{
  GVariantIter iter;
  GVariant *value;

  g_variant_iter_init (&iter, refs);

  while ((value = g_variant_iter_next_value (&iter)) != NULL)
    {
      const char *ref_name = NULL;

      g_variant_get_child (value, 0, "&s", &ref_name);

      if (ref_name != NULL)
        {
          g_autoptr(GVariant) csum_v = NULL;
          g_autoptr(GVariantIter) metadata = NULL;
          guint64 commit_size;

          g_variant_get_child (value, 1, "(t@aya{sv})",
                               &commit_size, &csum_v, &metadata);

          dump_summary_ref (collection_id, ref_name, commit_size, csum_v, metadata);

          g_print ("\n");
        }

      g_variant_unref (value);
    }
}

void
ot_dump_summary_bytes (GBytes          *summary_bytes,
                       OstreeDumpFlags  flags)
{
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) refs = NULL;
  g_autoptr(GVariant) exts = NULL;
  GVariantIter iter;
  GVariant *value;
  char *key;

  g_return_if_fail (summary_bytes != NULL);

  summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                      summary_bytes, FALSE);

  if (flags & OSTREE_DUMP_RAW)
    {
      ot_dump_variant (summary);
      return;
    }

  refs = g_variant_get_child_value (summary, 0);
  exts = g_variant_get_child_value (summary, 1);

  /* Print the refs, including those with a collection ID specified. */
  const gchar *main_collection_id;
  g_autoptr(GVariant) collection_map = NULL;
  const gchar *collection_id;

  if (!g_variant_lookup (exts, OSTREE_SUMMARY_COLLECTION_ID, "&s", &main_collection_id))
    main_collection_id = NULL;

  dump_summary_refs (main_collection_id, refs);

  collection_map = g_variant_lookup_value (exts, OSTREE_SUMMARY_COLLECTION_MAP, G_VARIANT_TYPE ("a{sa(s(taya{sv}))}"));
  if (collection_map != NULL)
    {
      g_variant_iter_init (&iter, collection_map);

      while (g_variant_iter_loop (&iter, "{&s@a(s(taya{sv}))}", &collection_id, &refs))
        dump_summary_refs (collection_id, refs);
    }

  /* Print out the additional metadata. */
  g_variant_iter_init (&iter, exts);

  while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
    {
      g_autofree gchar *value_str = NULL;
      const gchar *pretty_key = NULL;

      if (g_strcmp0 (key, OSTREE_SUMMARY_STATIC_DELTAS) == 0)
        {
          pretty_key = "Static Deltas";
          value_str = g_variant_print (value, FALSE);
        }
      else if (g_strcmp0 (key, OSTREE_SUMMARY_LAST_MODIFIED) == 0)
        {
          pretty_key = "Last-Modified";
          value_str = uint64_secs_to_iso8601 (GUINT64_FROM_BE (g_variant_get_uint64 (value)));
        }
      else if (g_strcmp0 (key, OSTREE_SUMMARY_EXPIRES) == 0)
        {
          pretty_key = "Expires";
          value_str = uint64_secs_to_iso8601 (GUINT64_FROM_BE (g_variant_get_uint64 (value)));
        }
      else if (g_strcmp0 (key, OSTREE_SUMMARY_COLLECTION_ID) == 0)
        {
          pretty_key = "Collection ID";
          value_str = g_variant_dup_string (value, NULL);
        }
      else if (g_strcmp0 (key, OSTREE_SUMMARY_COLLECTION_MAP) == 0)
        {
          pretty_key = "Collection Map";
          value_str = g_strdup ("(printed above)");
        }
      else
        {
          value_str = g_variant_print (value, FALSE);
        }

      /* Print out. */
      if (pretty_key != NULL)
        g_print ("%s (%s): %s\n", pretty_key, key, value_str);
      else
        g_print ("%s: %s\n", key, value_str);
    }
}
