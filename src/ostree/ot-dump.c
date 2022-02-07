/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
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
                  gboolean local_tz,
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

  if (local_tz)
    {
      /* Convert to local time and display in the locale's preferred
       * representation.
       */
      g_autoptr(GDateTime) dt_local = g_date_time_to_local (dt);
      str = g_date_time_format (dt_local, "%c");
    }
  else
    {
      str = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S +0000");
    }

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
  g_autofree char *parent = NULL;
  g_autofree char *str = NULL;
  g_autofree char *version = NULL;
  g_autoptr(GError) local_error = NULL;

  /* See OSTREE_COMMIT_GVARIANT_FORMAT */
  g_variant_get (variant, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                 &subject, &body, &timestamp, NULL, NULL);

  timestamp = GUINT64_FROM_BE (timestamp);
  str = format_timestamp (timestamp, FALSE, &local_error);
  if (!str)
    {
      g_assert (local_error); /* Pacify static analysis */
      errx (1, "Failed to read commit: %s", local_error->message);
    }

  if ((parent = ostree_commit_get_parent(variant)))
    {
      g_print ("Parent:  %s\n", parent);
    }

  g_autofree char *contents = ostree_commit_get_content_checksum (variant);
  g_print ("ContentChecksum:  %s\n", contents ?: "<invalid commit>");
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

  if (flags & OSTREE_DUMP_UNSWAPPED)
    {
      g_autofree char *formatted = g_variant_print (variant, TRUE);
      g_print ("%s\n", formatted);
    }
  else if (flags & OSTREE_DUMP_RAW)
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
      else if (g_strcmp0 (key, OSTREE_COMMIT_VERSION) == 0)
        {
          pretty_key = "Version";
          value_str = g_strdup (g_variant_get_string (value, NULL));
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
      g_autoptr(GVariant) collection_refs = NULL;
      g_variant_iter_init (&iter, collection_map);

      while (g_variant_iter_loop (&iter, "{&s@a(s(taya{sv}))}", &collection_id, &collection_refs))
        dump_summary_refs (collection_id, collection_refs);
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
      else if (g_strcmp0 (key, OSTREE_SUMMARY_MODE) == 0)
        {
          OstreeRepoMode repo_mode;
          const char *repo_mode_str = g_variant_get_string (value, NULL);

          pretty_key = "Repository Mode";
          if (!ostree_repo_mode_from_string (repo_mode_str, &repo_mode, NULL))
            value_str = g_strdup_printf ("Invalid (‘%s’)", repo_mode_str);
          else
            value_str = g_strdup (repo_mode_str);
        }
      else if (g_strcmp0 (key, OSTREE_SUMMARY_TOMBSTONE_COMMITS) == 0)
        {
          pretty_key = "Has Tombstone Commits";
          value_str = g_strdup (g_variant_get_boolean (value) ? "Yes" : "No");
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

static gboolean
dump_gpg_subkey (GVariant  *subkey,
                 gboolean   primary,
                 GError   **error)
{
  const gchar *fingerprint = NULL;
  gint64 created = 0;
  gint64 expires = 0;
  gboolean revoked = FALSE;
  gboolean expired = FALSE;
  gboolean invalid = FALSE;
  (void) g_variant_lookup (subkey, "fingerprint", "&s", &fingerprint);
  (void) g_variant_lookup (subkey, "created", "x", &created);
  (void) g_variant_lookup (subkey, "expires", "x", &expires);
  (void) g_variant_lookup (subkey, "revoked", "b", &revoked);
  (void) g_variant_lookup (subkey, "expired", "b", &expired);
  (void) g_variant_lookup (subkey, "invalid", "b", &invalid);

  /* Convert timestamps from big endian if needed */
  created = GINT64_FROM_BE (created);
  expires = GINT64_FROM_BE (expires);

  g_print ("%s: %s%s%s\n",
           primary ? "Key" : "  Subkey",
           fingerprint,
           revoked ? " (revoked)" : "",
           invalid ? " (invalid)" : "");

  g_autofree gchar *created_str = format_timestamp (created, TRUE,
                                                    error);
  if (created_str == NULL)
    return FALSE;
  g_print ("%sCreated: %s\n",
           primary ? "  " : "    ",
           created_str);

  if (expires > 0)
    {
      g_autofree gchar *expires_str = format_timestamp (expires, TRUE,
                                                        error);
      if (expires_str == NULL)
        return FALSE;
      g_print ("%s%s: %s\n",
               primary ? "  " : "    ",
               expired ? "Expired" : "Expires",
               expires_str);
    }

  return TRUE;
}

gboolean
ot_dump_gpg_key (GVariant  *key,
                 GError   **error)
{
  if (!g_variant_is_of_type (key, OSTREE_GPG_KEY_GVARIANT_FORMAT))
    return glnx_throw (error, "GPG key variant type doesn't match '%s'",
                       OSTREE_GPG_KEY_GVARIANT_STRING);

  g_autoptr(GVariant) subkeys_v = g_variant_get_child_value (key, 0);
  GVariantIter subkeys_iter;
  g_variant_iter_init (&subkeys_iter, subkeys_v);

  g_autoptr(GVariant) primary_key = NULL;
  g_variant_iter_next (&subkeys_iter, "@a{sv}", &primary_key);
  if (!dump_gpg_subkey (primary_key, TRUE, error))
    return FALSE;

  g_autoptr(GVariant) uids_v = g_variant_get_child_value (key, 1);
  GVariantIter uids_iter;
  g_variant_iter_init (&uids_iter, uids_v);
  GVariant *uid_v = NULL;
  while (g_variant_iter_loop (&uids_iter, "@a{sv}", &uid_v))
    {
      const gchar *uid = NULL;
      gboolean revoked = FALSE;
      gboolean invalid = FALSE;
      (void) g_variant_lookup (uid_v, "uid", "&s", &uid);
      (void) g_variant_lookup (uid_v, "revoked", "b", &revoked);
      (void) g_variant_lookup (uid_v, "invalid", "b", &invalid);
      g_print ("  UID: %s%s%s\n",
               uid,
               revoked ? " (revoked)" : "",
               invalid ? " (invalid)" : "");

      const char *advanced_url = NULL;
      const char *direct_url = NULL;
      (void) g_variant_lookup (uid_v, "advanced_url", "m&s", &advanced_url);
      (void) g_variant_lookup (uid_v, "direct_url", "m&s", &direct_url);
      g_print ("  Advanced update URL: %s\n", advanced_url ?: "");
      g_print ("  Direct update URL: %s\n", direct_url ?: "");
    }

  GVariant *subkey = NULL;
  while (g_variant_iter_loop (&subkeys_iter, "@a{sv}", &subkey))
    {
      if (!dump_gpg_subkey (subkey, FALSE, error))
        return FALSE;
    }

  return TRUE;
}
