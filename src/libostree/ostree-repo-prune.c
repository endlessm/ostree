/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-autocleanups.h"
#include "otutil.h"

typedef struct {
  OstreeRepo *repo;
  GHashTable *reachable;
  guint n_reachable_meta;
  guint n_reachable_content;
  guint n_unreachable_meta;
  guint n_unreachable_content;
  guint64 freed_bytes;
} OtPruneData;

static gboolean
maybe_prune_loose_object (OtPruneData           *data,
                          OstreeRepoPruneFlags   flags,
                          GVariant              *key,
                          GCancellable          *cancellable,
                          GError               **error)
{
  gboolean reachable = FALSE;
  const char *checksum;
  OstreeObjectType objtype;

  ostree_object_name_deserialize (key, &checksum, &objtype);
  /* Return if we only want to delete commits and this object is not a commit object. */
  gboolean commit_only = flags & OSTREE_REPO_PRUNE_FLAGS_COMMIT_ONLY;
  if (commit_only && (objtype != OSTREE_OBJECT_TYPE_COMMIT))
    goto exit;

  if (g_hash_table_lookup_extended (data->reachable, key, NULL, NULL))
    reachable = TRUE;
  else
    {
      guint64 storage_size = 0;

      g_debug ("Pruning unneeded object %s.%s", checksum,
               ostree_object_type_to_string (objtype));

      if (!ostree_repo_query_object_storage_size (data->repo, objtype, checksum,
                                                  &storage_size, cancellable, error))
        return FALSE;

      data->freed_bytes += storage_size;

      if (!(flags & OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE))
        {
          if (objtype == OSTREE_OBJECT_TYPE_PAYLOAD_LINK)
            {
              ssize_t size;
              char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
              char target_checksum[OSTREE_SHA256_STRING_LEN+1];
              char target_buf[_OSTREE_LOOSE_PATH_MAX + _OSTREE_PAYLOAD_LINK_PREFIX_LEN];

              _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_PAYLOAD_LINK, data->repo->mode);
              size = readlinkat (data->repo->objects_dir_fd, loose_path_buf, target_buf, sizeof (target_buf));
              if (size < 0)
                return glnx_throw_errno_prefix (error, "readlinkat");

              if (size < OSTREE_SHA256_STRING_LEN + _OSTREE_PAYLOAD_LINK_PREFIX_LEN)
                return glnx_throw (error, "invalid data size for %s", loose_path_buf);

              sprintf (target_checksum, "%.2s%.62s", target_buf + _OSTREE_PAYLOAD_LINK_PREFIX_LEN, target_buf + _OSTREE_PAYLOAD_LINK_PREFIX_LEN + 3);

              g_autoptr(GVariant) target_key = ostree_object_name_serialize (target_checksum, OSTREE_OBJECT_TYPE_FILE);

              if (g_hash_table_lookup_extended (data->reachable, target_key, NULL, NULL))
                {
                  guint64 target_storage_size = 0;
                  if (!ostree_repo_query_object_storage_size (data->repo, OSTREE_OBJECT_TYPE_FILE, target_checksum,
                                                              &target_storage_size, cancellable, error))
                    return FALSE;

                  reachable = target_storage_size >= data->repo->payload_link_threshold;
                  if (reachable)
                    goto exit;
                }
            }
          else if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              if (!ostree_repo_mark_commit_partial (data->repo, checksum, FALSE, error))
                return FALSE;
            }

          if (!ostree_repo_delete_object (data->repo, objtype, checksum,
                                          cancellable, error))
            return FALSE;

        }

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_unreachable_meta++;
      else
        data->n_unreachable_content++;
    }

 exit:
  if (reachable)
    {
      g_debug ("Keeping needed object %s.%s", checksum,
               ostree_object_type_to_string (objtype));
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_reachable_meta++;
      else
        data->n_reachable_content++;
    }
    if (commit_only && (objtype != OSTREE_OBJECT_TYPE_COMMIT))
    {
      g_debug ("Keeping object (not commit) %s.%s", checksum,
               ostree_object_type_to_string (objtype));
    }
  return TRUE;
}

static gboolean
_ostree_repo_prune_tmp (OstreeRepo *self,
                        GCancellable *cancellable,
                        GError **error)
{
  if (self->cache_dir_fd == -1)
    return TRUE;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (self->cache_dir_fd, _OSTREE_SUMMARY_CACHE_DIR,
                                     &dfd_iter, &exists, error))
    return FALSE;
  /* Note early return */
  if (!exists)
    return TRUE;

  while (TRUE)
    {
      size_t len;
      gboolean has_sig_suffix = FALSE;
      struct dirent *dent;
      g_autofree gchar *d_name = NULL;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      /* dirent->d_name can't be modified directly; see `man 3 readdir` */
      d_name = g_strdup (dent->d_name);
      len = strlen (d_name);
      if (len > 4 && g_strcmp0 (d_name + len - 4, ".sig") == 0)
        {
          has_sig_suffix = TRUE;
          d_name[len - 4] = '\0';
        }

      if (!g_hash_table_contains (self->remotes, d_name))
        {
          /* Restore the previous value to get the file name.  */
          if (has_sig_suffix)
            d_name[len - 4] = '.';

          if (!glnx_unlinkat (dfd_iter.fd, d_name, 0, error))
            return FALSE;
        }
    }

  return TRUE;
}


/**
 * ostree_repo_prune_static_deltas:
 * @self: Repo
 * @commit: (allow-none): ASCII SHA256 checksum for commit, or %NULL for each
 * non existing commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Prune static deltas, if COMMIT is specified then delete static delta files only
 * targeting that commit; otherwise any static delta of non existing commits are
 * deleted.
 *
 * Locking: exclusive
 */
gboolean
ostree_repo_prune_static_deltas (OstreeRepo *self, const char *commit,
                                 GCancellable      *cancellable,
                                 GError           **error)
{
  g_autoptr(OstreeRepoAutoLock) lock =
    ostree_repo_auto_lock_push (self, OSTREE_REPO_LOCK_EXCLUSIVE, cancellable, error);
  if (!lock)
    return FALSE;

  g_autoptr(GPtrArray) deltas = NULL;
  if (!ostree_repo_list_static_delta_names (self, &deltas,
                                            cancellable, error))
    return FALSE;

  for (guint i = 0; i < deltas->len; i++)
    {
      const char *deltaname = deltas->pdata[i];
      const char *dash = strchr (deltaname, '-');
      const char *to = NULL;
      g_autofree char *from = NULL;

      if (!dash)
        {
          to = deltaname;
        }
      else
        {
          from = g_strndup (deltaname, dash - deltaname);
          to = dash + 1;
        }

      if (commit)
        {
          if (g_strcmp0 (to, commit))
            continue;
        }
      else
        {
          gboolean have_commit;
          if (!ostree_repo_has_object (self, OSTREE_OBJECT_TYPE_COMMIT,
                                       to, &have_commit,
                                       cancellable, error))
            return FALSE;

          if (have_commit)
            continue;
        }

      g_debug ("Trying to prune static delta %s", deltaname);
      g_autofree char *deltadir = _ostree_get_relative_static_delta_path (from, to, NULL);
      if (!glnx_shutil_rm_rf_at (self->repo_dir_fd, deltadir,
                                 cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
repo_prune_internal (OstreeRepo        *self,
                     GHashTable        *objects,
                     OstreeRepoPruneOptions *options,
                     gint              *out_objects_total,
                     gint              *out_objects_pruned,
                     guint64           *out_pruned_object_size_total,
                     GCancellable      *cancellable,
                     GError           **error)
{
  OtPruneData data = { 0, };

  data.repo = self;
  /* We unref this when we're done */
  g_autoptr(GHashTable) reachable_owned = g_hash_table_ref (options->reachable);
  data.reachable = reachable_owned;

  GLNX_HASH_TABLE_FOREACH_KV (objects, GVariant*, serialized_key, GVariant*, objdata)
    {
      gboolean is_loose;

      g_variant_get_child (objdata, 0, "b", &is_loose);

      if (!is_loose)
        continue;

      if (!maybe_prune_loose_object (&data, options->flags, serialized_key,
                                     cancellable, error))
        return FALSE;
    }

  if (!ostree_repo_prune_static_deltas (self, NULL, cancellable, error))
    return FALSE;

  if (!_ostree_repo_prune_tmp (self, cancellable, error))
    return FALSE;

  *out_objects_total = (data.n_reachable_meta + data.n_unreachable_meta +
                        data.n_reachable_content + data.n_unreachable_content);
  *out_objects_pruned = (data.n_unreachable_meta + data.n_unreachable_content);
  *out_pruned_object_size_total = data.freed_bytes;
  return TRUE;
}

static gboolean
traverse_reachable_internal (OstreeRepo                    *self,
                             OstreeRepoCommitTraverseFlags  flags,
                             guint                          depth,
                             GHashTable                    *reachable,
                             GCancellable                  *cancellable,
                             GError                       **error)
{
  g_autoptr(OstreeRepoAutoLock) lock =
    ostree_repo_auto_lock_push (self, OSTREE_REPO_LOCK_SHARED, cancellable, error);
  if (!lock)
    return FALSE;

  /* Ignoring collections. */
  g_autoptr(GHashTable) all_refs = NULL;  /* (element-type utf8 utf8) */

  if (!ostree_repo_list_refs (self, NULL, &all_refs,
                              cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH_V (all_refs, const char*, checksum)
    {
      g_debug ("Finding objects to keep for commit %s", checksum);
      if (!ostree_repo_traverse_commit_with_flags (self, flags, checksum, depth, reachable,
                                                    NULL, cancellable, error))
        return FALSE;
    }

  /* Using collections. */
  g_autoptr(GHashTable) all_collection_refs = NULL;  /* (element-type OstreeChecksumRef utf8) */

  if (!ostree_repo_list_collection_refs (self, NULL, &all_collection_refs,
                                         OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES, cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH_V (all_collection_refs, const char*, checksum)
    {
      g_debug ("Finding objects to keep for commit %s", checksum);
      if (!ostree_repo_traverse_commit_with_flags (self, flags, checksum, depth, reachable,
                                                    NULL, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_repo_traverse_reachable_refs:
 * @self: Repo
 * @depth: Depth of traversal
 * @reachable: (element-type GVariant GVariant): Set of reachable objects (will be modified)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Add all commit objects directly reachable via a ref to @reachable.
 *
 * Locking: shared
 * Since: 2018.6
 */
gboolean
ostree_repo_traverse_reachable_refs (OstreeRepo *self,
                                     guint       depth,
                                     GHashTable *reachable,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  return traverse_reachable_internal (self, 
                                      OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                      depth, reachable,
                                      cancellable, error);
}

/**
 * ostree_repo_prune:
 * @self: Repo
 * @flags: Options controlling prune process
 * @depth: Stop traversal after this many iterations (-1 for unlimited)
 * @out_objects_total: (out): Number of objects found
 * @out_objects_pruned: (out): Number of objects deleted
 * @out_pruned_object_size_total: (out): Storage size in bytes of objects deleted
 * @cancellable: Cancellable
 * @error: Error
 *
 * Delete content from the repository.  By default, this function will
 * only delete "orphaned" objects not referred to by any commit.  This
 * can happen during a local commit operation, when we have written
 * content objects but not saved the commit referencing them.
 *
 * However, if %OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY is provided, instead
 * of traversing all commits, only refs will be used.  Particularly
 * when combined with @depth, this is a convenient way to delete
 * history from the repository.
 *
 * Use the %OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE to just determine
 * statistics on objects that would be deleted, without actually
 * deleting them.
 *
 * Locking: exclusive
 */
gboolean
ostree_repo_prune (OstreeRepo        *self,
                   OstreeRepoPruneFlags   flags,
                   gint               depth,
                   gint              *out_objects_total,
                   gint              *out_objects_pruned,
                   guint64           *out_pruned_object_size_total,
                   GCancellable      *cancellable,
                   GError           **error)
{
  g_autoptr(OstreeRepoAutoLock) lock =
    ostree_repo_auto_lock_push (self, OSTREE_REPO_LOCK_EXCLUSIVE, cancellable, error);
  if (!lock)
    return FALSE;

  g_autoptr(GHashTable) objects = NULL;
  gboolean refs_only = flags & OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY;
  gboolean commit_only = flags & OSTREE_REPO_PRUNE_FLAGS_COMMIT_ONLY;

  g_autoptr(GHashTable) reachable = ostree_repo_traverse_new_reachable ();

  /* This original prune API has fixed logic for traversing refs or all commits
   * combined with actually deleting content. The newer backend API just does
   * the deletion.
   */

  OstreeRepoCommitTraverseFlags traverse_flags = OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE;
  if (commit_only)
    traverse_flags |= OSTREE_REPO_COMMIT_TRAVERSE_FLAG_COMMIT_ONLY;

  if (refs_only)
    {
      if (!traverse_reachable_internal (self, traverse_flags,
                                        depth, reachable,
                                        cancellable, error))
        return FALSE;
    }

  if (!ostree_repo_list_objects (self, OSTREE_REPO_LIST_OBJECTS_ALL | OSTREE_REPO_LIST_OBJECTS_NO_PARENTS,
                                 &objects, cancellable, error))
    return FALSE;

  if (!refs_only)
    {
      GLNX_HASH_TABLE_FOREACH (objects, GVariant*, serialized_key)
        {
          const char *checksum;
          OstreeObjectType objtype;

          ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

          if (objtype != OSTREE_OBJECT_TYPE_COMMIT)
            continue;

          g_debug ("Finding objects to keep for commit %s", checksum);
          if (!ostree_repo_traverse_commit_with_flags (self, traverse_flags, checksum, depth, reachable,
                                                       NULL, cancellable, error))
            return FALSE;
        }
    }

  { OstreeRepoPruneOptions opts = { flags, reachable };
    return repo_prune_internal (self, objects, &opts,
                                out_objects_total, out_objects_pruned,
                                out_pruned_object_size_total, cancellable, error);
  }
}

/**
 * ostree_repo_prune_from_reachable:
 * @self: Repo
 * @options: Options controlling prune process
 * @out_objects_total: (out): Number of objects found
 * @out_objects_pruned: (out): Number of objects deleted
 * @out_pruned_object_size_total: (out): Storage size in bytes of objects deleted
 * @cancellable: Cancellable
 * @error: Error
 *
 * Delete content from the repository.  This function is the "backend"
 * half of the higher level ostree_repo_prune().  To use this function,
 * you determine the root set yourself, and this function finds all other
 * unreferenced objects and deletes them.
 *
 * Use this API when you want to perform more selective pruning - for example,
 * retain all commits from a production branch, but just GC some history from
 * your dev branch.
 *
 * The %OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE flag may be specified to just determine
 * statistics on objects that would be deleted, without actually deleting them.
 *
 * Locking: exclusive
 *
 * Since: 2017.1
 */
gboolean
ostree_repo_prune_from_reachable (OstreeRepo        *self,
                                  OstreeRepoPruneOptions *options,
                                  gint              *out_objects_total,
                                  gint              *out_objects_pruned,
                                  guint64           *out_pruned_object_size_total,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  g_autoptr(OstreeRepoAutoLock) lock =
    ostree_repo_auto_lock_push (self, OSTREE_REPO_LOCK_EXCLUSIVE, cancellable, error);
  if (!lock)
    return FALSE;

  g_autoptr(GHashTable) objects = NULL;

  if (!ostree_repo_list_objects (self, OSTREE_REPO_LIST_OBJECTS_ALL | OSTREE_REPO_LIST_OBJECTS_NO_PARENTS,
                                 &objects, cancellable, error))
    return FALSE;

  return repo_prune_internal (self, objects, options, out_objects_total,
                              out_objects_pruned, out_pruned_object_size_total,
                              cancellable, error);
}
