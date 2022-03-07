/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "libglnx.h"
#include "ostree.h"
#include "otutil.h"

struct _OstreeRepoRealCommitTraverseIter {
  gboolean initialized;
  OstreeRepo *repo;
  GVariant *commit;
  GVariant *current_dir;
  const char *name;
  OstreeRepoCommitIterResult state;
  guint idx;
  char checksum_content[OSTREE_SHA256_STRING_LEN+1];
  char checksum_meta[OSTREE_SHA256_STRING_LEN+1];
};

/**
 * ostree_repo_commit_traverse_iter_init_commit:
 * @iter: An iter
 * @repo: A repo
 * @commit: Variant of type %OSTREE_OBJECT_TYPE_COMMIT
 * @flags: Flags
 * @error: Error
 *
 * Initialize (in place) an iterator over the root of a commit object.
 */
gboolean
ostree_repo_commit_traverse_iter_init_commit (OstreeRepoCommitTraverseIter   *iter,
                                              OstreeRepo                     *repo,
                                              GVariant                       *commit,
                                              OstreeRepoCommitTraverseFlags   flags,
                                              GError                        **error)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;

  memset (real, 0, sizeof (*real));
  real->initialized = TRUE;
  real->repo = g_object_ref (repo);
  real->commit = g_variant_ref (commit);
  real->current_dir = NULL;
  real->idx = 0;

  g_autoptr(GVariant) content_csum_bytes = NULL;
  g_variant_get_child (commit, 6, "@ay", &content_csum_bytes);
  const guchar *csum = ostree_checksum_bytes_peek_validate (content_csum_bytes, error);
  if (!csum)
    return FALSE;
  ostree_checksum_inplace_from_bytes (csum, real->checksum_content);

  g_autoptr(GVariant) meta_csum_bytes = NULL;
  g_variant_get_child (commit, 7, "@ay", &meta_csum_bytes);
  csum = ostree_checksum_bytes_peek_validate (meta_csum_bytes, error);
  if (!csum)
    return FALSE;
  ostree_checksum_inplace_from_bytes (csum, real->checksum_meta);

  return TRUE;
}

/**
 * ostree_repo_commit_traverse_iter_init_dirtree:
 * @iter: An iter
 * @repo: A repo
 * @dirtree: Variant of type %OSTREE_OBJECT_TYPE_DIR_TREE
 * @flags: Flags
 * @error: Error
 *
 * Initialize (in place) an iterator over a directory tree.
 */
gboolean
ostree_repo_commit_traverse_iter_init_dirtree (OstreeRepoCommitTraverseIter   *iter,
                                               OstreeRepo                     *repo,
                                               GVariant                       *dirtree,
                                               OstreeRepoCommitTraverseFlags   flags,
                                               GError                        **error)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;

  memset (real, 0, sizeof (*real));
  real->initialized = TRUE;
  real->repo = g_object_ref (repo);
  real->current_dir = g_variant_ref (dirtree);
  real->idx = 0;

  return TRUE;
}

/**
 * ostree_repo_commit_traverse_iter_next:
 * @iter: An iter
 * @cancellable: Cancellable
 * @error: Error
 *
 * Step the interator to the next item.  Files will be returned first,
 * then subdirectories.  Call this in a loop; upon encountering
 * %OSTREE_REPO_COMMIT_ITER_RESULT_END, there will be no more files or
 * directories.  If %OSTREE_REPO_COMMIT_ITER_RESULT_DIR is returned,
 * then call ostree_repo_commit_traverse_iter_get_dir() to retrieve
 * data for that directory.  Similarly, if
 * %OSTREE_REPO_COMMIT_ITER_RESULT_FILE is returned, call
 * ostree_repo_commit_traverse_iter_get_file().
 * 
 * If %OSTREE_REPO_COMMIT_ITER_RESULT_ERROR is returned, it is a
 * program error to call any further API on @iter except for
 * ostree_repo_commit_traverse_iter_clear().
 */
OstreeRepoCommitIterResult
ostree_repo_commit_traverse_iter_next (OstreeRepoCommitTraverseIter *iter,
                                       GCancellable                 *cancellable,
                                       GError                      **error)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  OstreeRepoCommitIterResult res = OSTREE_REPO_COMMIT_ITER_RESULT_ERROR;

  if (!real->current_dir)
    {
      if (!ostree_repo_load_variant (real->repo, OSTREE_OBJECT_TYPE_DIR_TREE,
                                     real->checksum_content,
                                     &real->current_dir,
                                     error))
        goto out;
      res = OSTREE_REPO_COMMIT_ITER_RESULT_DIR;
    }
  else
    {
      guint nfiles;
      guint ndirs;
      guint idx;
      const guchar *csum;
      g_autoptr(GVariant) content_csum_v = NULL;
      g_autoptr(GVariant) meta_csum_v = NULL;
      g_autoptr(GVariant) files_variant = NULL;
      g_autoptr(GVariant) dirs_variant = NULL;

      files_variant = g_variant_get_child_value (real->current_dir, 0);
      dirs_variant = g_variant_get_child_value (real->current_dir, 1);

      nfiles = g_variant_n_children (files_variant);
      ndirs = g_variant_n_children (dirs_variant);
      if (real->idx < nfiles)
        {
          idx = real->idx;
          g_variant_get_child (files_variant, idx, "(&s@ay)",
                               &real->name,
                               &content_csum_v);

          csum = ostree_checksum_bytes_peek_validate (content_csum_v, error);
          if (!csum)
            goto out;
          ostree_checksum_inplace_from_bytes (csum, real->checksum_content);

          res = OSTREE_REPO_COMMIT_ITER_RESULT_FILE;

          real->idx++;
        }
      else if (real->idx < nfiles + ndirs)
        {
          idx = real->idx - nfiles;

          g_variant_get_child (dirs_variant, idx, "(&s@ay@ay)",
                               &real->name, &content_csum_v, &meta_csum_v);

          csum = ostree_checksum_bytes_peek_validate (content_csum_v, error);
          if (!csum)
            goto out;
          ostree_checksum_inplace_from_bytes (csum, real->checksum_content);

          csum = ostree_checksum_bytes_peek_validate (meta_csum_v, error);
          if (!csum)
            goto out;
          ostree_checksum_inplace_from_bytes (csum, real->checksum_meta);
          
          res = OSTREE_REPO_COMMIT_ITER_RESULT_DIR;

          real->idx++;
        }
      else
        res = OSTREE_REPO_COMMIT_ITER_RESULT_END;
    }
  
  real->state = res;
 out:
  return res;
}

/**
 * ostree_repo_commit_traverse_iter_get_file:
 * @iter: An iter
 * @out_name: (out) (transfer none): Name of current file
 * @out_checksum: (out) (transfer none): Checksum of current file
 *
 * Return information on the current file.  This function may only be
 * called if %OSTREE_REPO_COMMIT_ITER_RESULT_FILE was returned from
 * ostree_repo_commit_traverse_iter_next().
 */
void
ostree_repo_commit_traverse_iter_get_file (OstreeRepoCommitTraverseIter *iter,
                                           char                        **out_name,
                                           char                        **out_checksum)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  *out_name = (char*)real->name;
  *out_checksum = (char*)real->checksum_content;
}

/**
 * ostree_repo_commit_traverse_iter_get_dir:
 * @iter: An iter
 * @out_name: (out) (transfer none): Name of current dir
 * @out_content_checksum: (out) (transfer none): Checksum of current content
 * @out_meta_checksum: (out) (transfer none): Checksum of current metadata
 *
 * Return information on the current directory.  This function may
 * only be called if %OSTREE_REPO_COMMIT_ITER_RESULT_DIR was returned
 * from ostree_repo_commit_traverse_iter_next().
 */
void
ostree_repo_commit_traverse_iter_get_dir (OstreeRepoCommitTraverseIter *iter,
                                          char                        **out_name,
                                          char                        **out_content_checksum,
                                          char                        **out_meta_checksum)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  *out_name = (char*)real->name;
  *out_content_checksum = (char*)real->checksum_content;
  *out_meta_checksum = (char*)real->checksum_meta;
}

void
ostree_repo_commit_traverse_iter_clear (OstreeRepoCommitTraverseIter *iter)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  g_clear_object (&real->repo);
  g_clear_pointer (&real->commit, g_variant_unref);
  g_clear_pointer (&real->current_dir, g_variant_unref);
}

void
ostree_repo_commit_traverse_iter_cleanup (void *p)
{
  OstreeRepoCommitTraverseIter *iter = p;
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  if (real->initialized)
    {
      ostree_repo_commit_traverse_iter_clear (iter);
      real->initialized = FALSE;
    }
}

/**
 * ostree_repo_traverse_new_reachable:
 *
 * This hash table is a set of #GVariant which can be accessed via
 * ostree_object_name_deserialize().
 *
 * Returns: (transfer container) (element-type GVariant GVariant): A new hash table
 */
GHashTable *
ostree_repo_traverse_new_reachable (void)
{
  return g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                NULL, (GDestroyNotify)g_variant_unref);
}

/**
 * ostree_repo_traverse_new_parents:
 *
 * This hash table is a mapping from #GVariant which can be accessed
 * via ostree_object_name_deserialize() to a #GVariant containing either
 * a similar #GVariant or and array of them, listing the parents of the key.
 *
 * Returns: (transfer container) (element-type GVariant GVariant): A new hash table
 *
 * Since: 2018.5
 */
GHashTable *
ostree_repo_traverse_new_parents (void)
{
  return g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                (GDestroyNotify)g_variant_unref, (GDestroyNotify)g_variant_unref);
}

static void
parents_get_commits (GHashTable *parents_ht, GVariant *object, GHashTable *res)
{
  const char *checksum;
  OstreeObjectType type;

  if (object == NULL)
    return;

  ostree_object_name_deserialize (object, &checksum, &type);
  if (type == OSTREE_OBJECT_TYPE_COMMIT)
    g_hash_table_add (res, g_strdup (checksum));
  else
    {
      GVariant *parents = g_hash_table_lookup (parents_ht, object);

      if (parents == NULL)
        g_debug ("Unexpected NULL parent");
      else if (g_variant_is_of_type (parents, G_VARIANT_TYPE_ARRAY))
        {
          gsize i, len = g_variant_n_children (parents);

          for (i = 0; i < len; i++)
            {
              g_autoptr(GVariant) parent = g_variant_get_child_value (parents, i);
              parents_get_commits (parents_ht, parent, res);
            }
        }
      else
        parents_get_commits (parents_ht, parents, res);
    }
}

/**
 * ostree_repo_traverse_parents_get_commits:
 *
 * Gets all the commits that a certain object belongs to, as recorded
 * by a parents table gotten from ostree_repo_traverse_commit_union_with_parents.
 *
 * Returns: (transfer full) (array zero-terminated=1): An array of checksums for
 * the commits the key belongs to.
 *
 * Since: 2018.5
 */
char **
ostree_repo_traverse_parents_get_commits (GHashTable *parents, GVariant *object)
{
  g_autoptr(GHashTable) res = g_hash_table_new (g_str_hash, g_str_equal);

  parents_get_commits (parents, object, res);

  return (char **)g_hash_table_get_keys_as_array (res, NULL);
}

static gboolean
traverse_dirtree (OstreeRepo           *repo,
                  const char           *checksum,
                  GVariant             *parent_key,
                  GHashTable           *inout_reachable,
                  GHashTable           *inout_parents,
                  gboolean              ignore_missing_dirs,
                  GCancellable         *cancellable,
                  GError              **error);

static void
add_parent_ref (GHashTable  *inout_parents,
                GVariant    *key,
                GVariant    *parent_key)
{
  GVariant *old_parents;

  if (inout_parents == NULL)
    return;

  old_parents = g_hash_table_lookup (inout_parents, key);
  if (old_parents == NULL)
    {
      /* For the common case of a single pointer we skip using an array to save memory. */
      g_hash_table_insert (inout_parents, g_variant_ref (key), g_variant_ref (parent_key));
    }
  else
    {
      g_autofree GVariant **new_parents = NULL;
      gsize i, len = 0;

      if (g_variant_is_of_type (old_parents, G_VARIANT_TYPE_ARRAY))
        {
          gsize old_parents_len = g_variant_n_children (old_parents);
          new_parents = g_new (GVariant *,  old_parents_len + 1);
          for (i = 0; i < old_parents_len ; i++)
            {
              g_autoptr(GVariant) old_parent = g_variant_get_child_value (old_parents, i);
              if (!g_variant_equal (old_parent, parent_key))
                new_parents[len++] = g_steal_pointer (&old_parent);
            }
        }
      else
        {
          new_parents = g_new (GVariant *, 2);
          if (!g_variant_equal (old_parents, parent_key))
            new_parents[len++] = g_variant_ref (old_parents);
        }
      new_parents[len++] = g_variant_ref (parent_key);
      g_hash_table_insert (inout_parents, g_variant_ref (key),
                           g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(su)"), new_parents , len)));
      for (i = 0; i < len; i++)
        g_variant_unref (new_parents[i]);
    }
}


static gboolean
traverse_iter (OstreeRepo                          *repo,
               OstreeRepoCommitTraverseIter        *iter,
               GVariant                            *parent_key,
               GHashTable                          *inout_reachable,
               GHashTable                          *inout_parents,
               gboolean                             ignore_missing_dirs,
               GCancellable                        *cancellable,
               GError                             **error)
{
  while (TRUE)
    {
      g_autoptr(GVariant) key = NULL;
      g_autoptr(GError) local_error = NULL;
      OstreeRepoCommitIterResult iterres =
        ostree_repo_commit_traverse_iter_next (iter, cancellable, &local_error);

      if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_ERROR)
        {
          /* There is only one kind of not-found error, which is
             failing to load the dirmeta itself, if so, we ignore that
             (and the whole subtree) if told to. */
          if (ignore_missing_dirs &&
              g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_debug ("Ignoring not-found dirmeta");
              return TRUE;  /* Note early return */
            }

          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_END)
        break;
      else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_FILE)
        {
          char *name;
          char *checksum;

          ostree_repo_commit_traverse_iter_get_file (iter, &name, &checksum);

          g_debug ("Found file object %s", checksum);
          key = g_variant_ref_sink (ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_FILE));
          add_parent_ref (inout_parents, key, parent_key);
          g_hash_table_add (inout_reachable, g_steal_pointer (&key));
        }
      else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_DIR)
        {
          char *name;
          char *content_checksum;
          char *meta_checksum;

          ostree_repo_commit_traverse_iter_get_dir (iter, &name, &content_checksum,
                                                    &meta_checksum);

          g_debug ("Found dirtree object %s", content_checksum);
          g_debug ("Found dirmeta object %s", meta_checksum);
          key = g_variant_ref_sink (ostree_object_name_serialize (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META));
          add_parent_ref (inout_parents, key, parent_key);
          g_hash_table_add (inout_reachable, g_steal_pointer (&key));

          key = g_variant_ref_sink (ostree_object_name_serialize (content_checksum, OSTREE_OBJECT_TYPE_DIR_TREE));
          add_parent_ref (inout_parents, key, parent_key);
          if (!g_hash_table_lookup (inout_reachable, key))
            {
              if (!traverse_dirtree (repo, content_checksum, key, inout_reachable, inout_parents,
                                     ignore_missing_dirs, cancellable, error))
                return FALSE;

              g_hash_table_add (inout_reachable, g_steal_pointer (&key));
            }
        }
      else
        g_assert_not_reached ();
    }

  return TRUE;
}

static gboolean
traverse_dirtree (OstreeRepo           *repo,
                  const char           *checksum,
                  GVariant             *parent_key,
                  GHashTable           *inout_reachable,
                  GHashTable           *inout_parents,
                  gboolean              ignore_missing_dirs,
                  GCancellable         *cancellable,
                  GError              **error)
{
  g_autoptr(GError) local_error = NULL;

  g_autoptr(GVariant) dirtree = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &dirtree, &local_error))
    {
      if (ignore_missing_dirs &&
          g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_debug ("Ignoring not-found dirmeta %s", checksum);
          return TRUE; /* Early return */
        }

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_debug ("Traversing dirtree %s", checksum);
  ostree_cleanup_repo_commit_traverse_iter
    OstreeRepoCommitTraverseIter iter = { 0, };
  if (!ostree_repo_commit_traverse_iter_init_dirtree (&iter, repo, dirtree,
                                                      OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                      error))
    return FALSE;

  if (!traverse_iter (repo, &iter, parent_key, inout_reachable, inout_parents, ignore_missing_dirs, cancellable, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_repo_traverse_commit_with_flags: (skip)
 * @repo: Repo
 * @flags: change traversal behaviour according to these flags
 * @commit_checksum: ASCII SHA256 checksum
 * @maxdepth: Traverse this many parent commits, -1 for unlimited
 * @inout_reachable: Set of reachable objects
 * @inout_parents: Map from object to parent object
 * @cancellable: Cancellable
 * @error: Error
 *
 * Update the set @inout_reachable containing all objects reachable
 * from @commit_checksum, traversing @maxdepth parent commits.
 *
 * Additionally this constructs a mapping from each object to the parents
 * of the object, which can be used to track which commits an object
 * belongs to.
 *
 * Since: 2018.5
 */
gboolean
ostree_repo_traverse_commit_with_flags (OstreeRepo                     *repo,
                                        OstreeRepoCommitTraverseFlags   flags,
                                        const char                     *commit_checksum,
                                        int                             maxdepth,
                                        GHashTable                     *inout_reachable,
                                        GHashTable                     *inout_parents,
                                        GCancellable                   *cancellable,
                                        GError                        **error)
{
  g_autofree char *tmp_checksum = NULL;
  gboolean commit_only = flags & OSTREE_REPO_COMMIT_TRAVERSE_FLAG_COMMIT_ONLY;

  while (TRUE)
    {
      g_autoptr(GVariant) key =
        g_variant_ref_sink (ostree_object_name_serialize (commit_checksum, OSTREE_OBJECT_TYPE_COMMIT));

      if (g_hash_table_contains (inout_reachable, key))
        break;

      g_autoptr(GVariant) commit = NULL;
      if (!ostree_repo_load_variant_if_exists (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                               commit_checksum, &commit,
                                               error))
        return FALSE;

      /* Just return if the parent isn't found; we do expect most
       * people to have partial repositories.
       */
      if (!commit)
        break;

      /* See if the commit is partial, if so it's not an error to lack objects */
      OstreeRepoCommitState commitstate;
      if (!ostree_repo_load_commit (repo, commit_checksum, NULL, &commitstate,
                                    error))
        return FALSE;

      gboolean ignore_missing_dirs = FALSE;
      if ((commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL) != 0)
        ignore_missing_dirs = TRUE;

      g_hash_table_add (inout_reachable, g_variant_ref (key));

      /* Save time by skipping traversal of non-commit objects */
      if (!commit_only) 
        {
          g_debug ("Traversing commit %s", commit_checksum);
          ostree_cleanup_repo_commit_traverse_iter
            OstreeRepoCommitTraverseIter iter = { 0, };
          if (!ostree_repo_commit_traverse_iter_init_commit (&iter, repo, commit,
                                                            OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                            error))
            return FALSE;

          if (!traverse_iter (repo, &iter, key, inout_reachable, inout_parents, ignore_missing_dirs, cancellable, error))
            return FALSE;
        } 

      gboolean recurse = FALSE;
      if (maxdepth == -1 || maxdepth > 0)
        {
          g_free (tmp_checksum);
          tmp_checksum = ostree_commit_get_parent (commit);
          if (tmp_checksum)
            {
              commit_checksum = tmp_checksum;
              if (maxdepth > 0)
                maxdepth -= 1;
              recurse = TRUE;
            }
        }
      if (!recurse)
        break;
    }

  return TRUE;
}

/**
 * ostree_repo_traverse_commit_union_with_parents: (skip)
 * @repo: Repo
 * @commit_checksum: ASCII SHA256 checksum
 * @maxdepth: Traverse this many parent commits, -1 for unlimited
 * @inout_reachable: Set of reachable objects
 * @inout_parents: Map from object to parent object
 * @cancellable: Cancellable
 * @error: Error
 *
 * Update the set @inout_reachable containing all objects reachable
 * from @commit_checksum, traversing @maxdepth parent commits.
 *
 * Additionally this constructs a mapping from each object to the parents
 * of the object, which can be used to track which commits an object
 * belongs to.
 *
 * Since: 2018.5
 */
gboolean
ostree_repo_traverse_commit_union_with_parents (OstreeRepo      *repo,
                                                const char      *commit_checksum,
                                                int              maxdepth,
                                                GHashTable      *inout_reachable,
                                                GHashTable      *inout_parents,
                                                GCancellable    *cancellable,
                                                GError         **error)
{
  return ostree_repo_traverse_commit_with_flags(repo, OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                commit_checksum, maxdepth, inout_reachable, inout_parents,
                                                cancellable, error);
}

/**
 * ostree_repo_traverse_commit_union: (skip)
 * @repo: Repo
 * @commit_checksum: ASCII SHA256 checksum
 * @maxdepth: Traverse this many parent commits, -1 for unlimited
 * @inout_reachable: Set of reachable objects
 * @cancellable: Cancellable
 * @error: Error
 *
 * Update the set @inout_reachable containing all objects reachable
 * from @commit_checksum, traversing @maxdepth parent commits.
 */
gboolean
ostree_repo_traverse_commit_union (OstreeRepo      *repo,
                                   const char      *commit_checksum,
                                   int              maxdepth,
                                   GHashTable      *inout_reachable,
                                   GCancellable    *cancellable,
                                   GError         **error)
{
  return
    ostree_repo_traverse_commit_union_with_parents (repo, commit_checksum, maxdepth,
                                                    inout_reachable, NULL,
                                                    cancellable, error);
}

/**
 * ostree_repo_traverse_commit:
 * @repo: Repo
 * @commit_checksum: ASCII SHA256 checksum
 * @maxdepth: Traverse this many parent commits, -1 for unlimited
 * @out_reachable: (out) (transfer container) (element-type GVariant GVariant): Set of reachable objects
 * @cancellable: Cancellable
 * @error: Error
 *
 * Create a new set @out_reachable containing all objects reachable
 * from @commit_checksum, traversing @maxdepth parent commits.
 */
gboolean
ostree_repo_traverse_commit (OstreeRepo      *repo,
                             const char      *commit_checksum,
                             int              maxdepth,
                             GHashTable     **out_reachable,
                             GCancellable    *cancellable,
                             GError         **error)
{
  g_autoptr(GHashTable) ret_reachable = ostree_repo_traverse_new_reachable ();
  if (!ostree_repo_traverse_commit_union (repo, commit_checksum, maxdepth,
                                          ret_reachable, cancellable, error))
    return FALSE;

  if (out_reachable)
    *out_reachable = g_steal_pointer (&ret_reachable);
  return TRUE;
}
