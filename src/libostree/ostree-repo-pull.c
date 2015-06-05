/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2012,2013 Colin Walters <walters@verbum.org>
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

#include "ostree.h"
#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-metalink.h"
#include "otutil.h"

#include <gio/gunixinputstream.h>

#define OSTREE_REPO_PULL_CONTENT_PRIORITY  (OSTREE_FETCHER_DEFAULT_PRIORITY)
#define OSTREE_REPO_PULL_METADATA_PRIORITY (OSTREE_REPO_PULL_CONTENT_PRIORITY - 100)

typedef struct {
  OstreeRepo   *repo;
  int           tmpdir_dfd;
  OstreeRepoPullFlags flags;
  char         *remote_name;
  OstreeRepoMode remote_mode;
  OstreeFetcher *fetcher;
  SoupURI      *base_uri;
  OstreeRepo   *remote_repo_local;

  GMainContext    *main_context;
  GMainLoop    *loop;
  GCancellable *cancellable;
  OstreeAsyncProgress *progress;

  gboolean      transaction_resuming;
  enum {
    OSTREE_PULL_PHASE_FETCHING_REFS,
    OSTREE_PULL_PHASE_FETCHING_OBJECTS
  }             phase;
  gint          n_scanned_metadata;
  SoupURI       *fetching_sync_uri;
  
  gboolean          gpg_verify;

  GBytes           *summary_data;
  GVariant         *summary;
  GPtrArray        *static_delta_superblocks;
  GHashTable       *expected_commit_sizes; /* Maps commit checksum to known size */
  GHashTable       *commit_to_depth; /* Maps commit checksum maximum depth */
  GHashTable       *scanned_metadata; /* Maps object name to itself */
  GHashTable       *requested_metadata; /* Maps object name to itself */
  GHashTable       *requested_content; /* Maps object name to itself */
  guint             n_outstanding_metadata_fetches;
  guint             n_outstanding_metadata_write_requests;
  guint             n_outstanding_content_fetches;
  guint             n_outstanding_content_write_requests;
  guint             n_outstanding_deltapart_fetches;
  guint             n_outstanding_deltapart_write_requests;
  guint             n_total_deltaparts;
  guint64           total_deltapart_size;
  gint              n_requested_metadata;
  gint              n_requested_content;
  guint             n_fetched_deltaparts;
  guint             n_fetched_metadata;
  guint             n_fetched_content;

  int               maxdepth;
  guint64           start_time;

  gboolean          is_mirror;

  char         *dir;
  gboolean      commitpartial_exists;

  gboolean      have_previous_bytes;
  guint64       previous_bytes_sec;
  guint64       previous_total_downloaded;

  GError      **async_error;
  gboolean      caught_error;
} OtPullData;

typedef struct {
  OtPullData  *pull_data;
  GVariant    *object;
  gboolean     is_detached_meta;
} FetchObjectData;

typedef struct {
  OtPullData  *pull_data;
  GVariant *objects;
  char *expected_checksum;
} FetchStaticDeltaData;

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...) G_GNUC_NULL_TERMINATED;

static gboolean scan_one_metadata_object (OtPullData         *pull_data,
                                          const char         *csum,
                                          OstreeObjectType    objtype,
                                          guint               recursion_depth,
                                          GCancellable       *cancellable,
                                          GError            **error);

static gboolean scan_one_metadata_object_c (OtPullData         *pull_data,
                                            const guchar       *csum,
                                            OstreeObjectType    objtype,
                                            guint               recursion_depth,
                                            GCancellable       *cancellable,
                                            GError            **error);

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...)
{
  va_list args;
  GPtrArray *arg_array;
  const char *arg;
  char *subpath;
  SoupURI *ret;

  arg_array = g_ptr_array_new ();
  g_ptr_array_add (arg_array, (char*)soup_uri_get_path (base));
  g_ptr_array_add (arg_array, (char*)first);

  va_start (args, first);
  
  while ((arg = va_arg (args, const char *)) != NULL)
    g_ptr_array_add (arg_array, (char*)arg);
  g_ptr_array_add (arg_array, NULL);

  subpath = g_build_filenamev ((char**)arg_array->pdata);
  g_ptr_array_unref (arg_array);
  
  ret = soup_uri_copy (base);
  soup_uri_set_path (ret, subpath);
  g_free (subpath);
  
  va_end (args);
  
  return ret;
}

static gboolean
update_progress (gpointer user_data)
{
  OtPullData *pull_data;
  guint outstanding_writes;
  guint outstanding_fetches;
  guint64 bytes_transferred;
  guint fetched;
  guint requested;
  guint n_scanned_metadata;
  guint64 start_time;

  pull_data = user_data;

  if (! pull_data->progress)
    return FALSE;

  outstanding_writes = pull_data->n_outstanding_content_write_requests +
    pull_data->n_outstanding_metadata_write_requests +
    pull_data->n_outstanding_deltapart_write_requests;
  outstanding_fetches = pull_data->n_outstanding_content_fetches +
    pull_data->n_outstanding_metadata_fetches +
    pull_data->n_outstanding_deltapart_fetches;
  bytes_transferred = _ostree_fetcher_bytes_transferred (pull_data->fetcher);
  fetched = pull_data->n_fetched_metadata + pull_data->n_fetched_content;
  requested = pull_data->n_requested_metadata + pull_data->n_requested_content;
  n_scanned_metadata = pull_data->n_scanned_metadata;
  start_time = pull_data->start_time;

  ostree_async_progress_set_uint (pull_data->progress, "outstanding-fetches", outstanding_fetches);
  ostree_async_progress_set_uint (pull_data->progress, "outstanding-writes", outstanding_writes);
  ostree_async_progress_set_uint (pull_data->progress, "fetched", fetched);
  ostree_async_progress_set_uint (pull_data->progress, "requested", requested);
  ostree_async_progress_set_uint (pull_data->progress, "scanned-metadata", n_scanned_metadata);
  ostree_async_progress_set_uint64 (pull_data->progress, "bytes-transferred", bytes_transferred);
  ostree_async_progress_set_uint64 (pull_data->progress, "start-time", start_time);

  /* Deltas */
  ostree_async_progress_set_uint (pull_data->progress, "fetched-delta-parts",
                                  pull_data->n_fetched_deltaparts);
  ostree_async_progress_set_uint (pull_data->progress, "total-delta-parts",
                                  pull_data->n_total_deltaparts);
  ostree_async_progress_set_uint64 (pull_data->progress, "total-delta-part-size",
                                    pull_data->total_deltapart_size);
  ostree_async_progress_set_uint (pull_data->progress, "total-delta-superblocks",
                                  pull_data->static_delta_superblocks->len);

  /* We fetch metadata before content.  These allow us to report metadata fetch progress specifically. */
  ostree_async_progress_set_uint (pull_data->progress, "outstanding-metadata-fetches", pull_data->n_outstanding_metadata_fetches);
  ostree_async_progress_set_uint (pull_data->progress, "metadata-fetched", pull_data->n_fetched_metadata);

  if (pull_data->fetching_sync_uri)
    {
      gs_free char *uri_string = soup_uri_to_string (pull_data->fetching_sync_uri, TRUE);
      gs_free char *status_string = g_strconcat ("Requesting ", uri_string, NULL);
      ostree_async_progress_set_status (pull_data->progress, status_string);
    }
  else
    ostree_async_progress_set_status (pull_data->progress, NULL);

  return TRUE;
}

static void
throw_async_error (OtPullData          *pull_data,
                   GError              *error)
{
  if (error)
    {
      if (!pull_data->caught_error)
        {
          pull_data->caught_error = TRUE;
          g_propagate_error (pull_data->async_error, error);
          g_main_loop_quit (pull_data->loop);
        }
      else
        {
          g_error_free (error);
        }
    }
}

static void
check_outstanding_requests_handle_error (OtPullData          *pull_data,
                                         GError              *error)
{
  gboolean current_fetch_idle = (pull_data->n_outstanding_metadata_fetches == 0 &&
                                 pull_data->n_outstanding_content_fetches == 0 &&
                                 pull_data->n_outstanding_deltapart_fetches == 0);
  gboolean current_write_idle = (pull_data->n_outstanding_metadata_write_requests == 0 &&
                                 pull_data->n_outstanding_content_write_requests == 0 &&
                                 pull_data->n_outstanding_deltapart_write_requests == 0 );
  gboolean current_idle = current_fetch_idle && current_write_idle;

  throw_async_error (pull_data, error);

  switch (pull_data->phase)
    {
    case OSTREE_PULL_PHASE_FETCHING_REFS:
      if (!pull_data->fetching_sync_uri)
        g_main_loop_quit (pull_data->loop);
      break;
    case OSTREE_PULL_PHASE_FETCHING_OBJECTS:
      if (current_idle && !pull_data->fetching_sync_uri)
        {
          g_debug ("pull: idle, exiting mainloop");
          
          g_main_loop_quit (pull_data->loop);
        }
      break;
    }
}

static gboolean
idle_check_outstanding_requests (gpointer user_data)
{
  check_outstanding_requests_handle_error (user_data, NULL);
  return FALSE;
}

typedef struct {
  OtPullData     *pull_data;
  GInputStream   *result_stream;
} OstreeFetchUriSyncData;

static gboolean
fetch_uri_contents_membuf_sync (OtPullData    *pull_data,
                                SoupURI        *uri,
                                gboolean        add_nul,
                                gboolean        allow_noent,
                                GBytes        **out_contents,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret;
  pull_data->fetching_sync_uri = uri;
  ret = _ostree_fetcher_request_uri_to_membuf (pull_data->fetcher,
                                               uri,
                                               add_nul,
                                               allow_noent,
                                               out_contents,
                                               pull_data->loop,
                                               OSTREE_MAX_METADATA_SIZE,
                                               cancellable,
                                               error);
  pull_data->fetching_sync_uri = NULL;
  return ret;
}

static gboolean
fetch_uri_contents_utf8_sync (OtPullData  *pull_data,
                              SoupURI     *uri,
                              char       **out_contents,
                              GCancellable  *cancellable,
                              GError     **error)
{
  gboolean ret = FALSE;
  gs_unref_bytes GBytes *bytes = NULL;
  gs_free char *ret_contents = NULL;
  gsize len;

  if (!fetch_uri_contents_membuf_sync (pull_data, uri, TRUE, FALSE,
                                       &bytes, cancellable, error))
    goto out;

  ret_contents = g_bytes_unref_to_data (bytes, &len);
  bytes = NULL;

  if (!g_utf8_validate (ret_contents, -1, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid UTF-8");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  return ret;
}

static void
enqueue_one_object_request (OtPullData        *pull_data,
                            const char        *checksum,
                            OstreeObjectType   objtype,
                            gboolean           is_detached_meta);

static gboolean
scan_dirtree_object (OtPullData   *pull_data,
                     const char   *checksum,
                     int           recursion_depth,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  int i, n;
  gs_unref_variant GVariant *tree = NULL;
  gs_unref_variant GVariant *files_variant = NULL;
  gs_unref_variant GVariant *dirs_variant = NULL;
  char *subdir_target = NULL;
  const char *dirname = NULL;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &tree, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (tree, 0);
  dirs_variant = g_variant_get_child_value (tree, 1);

  /* Skip files if we're traversing a request only directory */
  if (pull_data->dir)
    n = 0;
  else
    n = g_variant_n_children (files_variant);

  for (i = 0; i < n; i++)
    {
      const char *filename;
      gboolean file_is_stored;
      gs_unref_variant GVariant *csum = NULL;
      gs_free char *file_checksum = NULL;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (!ot_util_filename_validate (filename, error))
        goto out;

      file_checksum = ostree_checksum_from_bytes_v (csum);

      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                   &file_is_stored, cancellable, error))
        goto out;

      if (!file_is_stored && pull_data->remote_repo_local)
        {
          if (!ostree_repo_import_object_from (pull_data->repo, pull_data->remote_repo_local,
                                               OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                               cancellable, error))
            goto out;
        }
      else if (!file_is_stored && !g_hash_table_lookup (pull_data->requested_content, file_checksum))
        {
          g_hash_table_insert (pull_data->requested_content, file_checksum, file_checksum);
          enqueue_one_object_request (pull_data, file_checksum, OSTREE_OBJECT_TYPE_FILE, FALSE);
          file_checksum = NULL;  /* Transfer ownership */
        }
    }

    if (pull_data->dir)
      {
        const char *subpath = NULL;  
        const char *nextslash = NULL;
        gs_free char *dir_data = NULL;

        g_assert (pull_data->dir[0] == '/'); // assert it starts with / like "/usr/share/rpm"
        subpath = pull_data->dir + 1;  // refers to name minus / like "usr/share/rpm"
        nextslash = strchr (subpath, '/'); //refers to start of next slash like "/share/rpm"
        dir_data = pull_data->dir; // keep the original pointer around since strchr() points into it
        pull_data->dir = NULL;

        if (nextslash)
          {
            subdir_target = g_strndup (subpath, nextslash - subpath); // refers to first dir, like "usr"
            pull_data->dir = g_strdup (nextslash); // sets dir to new deeper level like "/share/rpm"
          }
        else // we're as deep as it goes, i.e. subpath = "rpm"
          subdir_target = g_strdup (subpath); 
      }

  n = g_variant_n_children (dirs_variant);

  for (i = 0; i < n; i++)
    {
      gs_unref_variant GVariant *tree_csum = NULL;
      gs_unref_variant GVariant *meta_csum = NULL;

      g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                           &dirname, &tree_csum, &meta_csum);

      if (!ot_util_filename_validate (dirname, error))
        goto out;

      if (subdir_target && strcmp (subdir_target, dirname) != 0)
        continue;
      
      if (!scan_one_metadata_object_c (pull_data, ostree_checksum_bytes_peek (tree_csum),
                                       OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1,
                                       cancellable, error))
        goto out;
      
      if (!scan_one_metadata_object_c (pull_data, ostree_checksum_bytes_peek (meta_csum),
                                       OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1,
                                       cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fetch_ref_contents (OtPullData    *pull_data,
                    const char    *ref,
                    char         **out_contents,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_contents = NULL;
  SoupURI *target_uri = NULL;

  target_uri = suburi_new (pull_data->base_uri, "refs", "heads", ref, NULL);
  
  if (!fetch_uri_contents_utf8_sync (pull_data, target_uri, &ret_contents, cancellable, error))
    goto out;

  g_strchomp (ret_contents);

  if (!ostree_validate_checksum_string (ret_contents, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  if (target_uri)
    soup_uri_free (target_uri);
  return ret;
}

static gboolean
lookup_commit_checksum_from_summary (OtPullData    *pull_data,
                                     const char    *ref,
                                     char         **out_checksum,
                                     gsize         *out_size,
                                     GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *refs = g_variant_get_child_value (pull_data->summary, 0);
  gs_unref_variant GVariant *refdata = NULL;
  gs_unref_variant GVariant *reftargetdata = NULL;
  gs_unref_variant GVariant *commit_data = NULL;
  guint64 commit_size;
  gs_unref_variant GVariant *commit_csum_v = NULL;
  gs_unref_bytes GBytes *commit_bytes = NULL;
  int i;
  
  if (!ot_variant_bsearch_str (refs, ref, &i))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No such branch '%s' in repository summary",
                   ref);
      goto out;
    }
      
  refdata = g_variant_get_child_value (refs, i);
  reftargetdata = g_variant_get_child_value (refdata, 1);
  g_variant_get (reftargetdata, "(t@ay@a{sv})", &commit_size, &commit_csum_v, NULL);

  if (!ostree_validate_structureof_csum_v (commit_csum_v, error))
    goto out;

  ret = TRUE;
  *out_checksum = ostree_checksum_from_bytes_v (commit_csum_v);
  *out_size = commit_size;
 out:
  return ret;
}

static void
content_fetch_on_write_complete (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  OstreeObjectType objtype;
  const char *expected_checksum;
  gs_free guchar *csum = NULL;
  gs_free char *checksum = NULL;

  if (!ostree_repo_write_content_finish ((OstreeRepo*)object, result, 
                                         &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  g_debug ("write of %s complete", ostree_object_to_string (checksum, objtype));

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted content object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  pull_data->n_fetched_content++;
 out:
  pull_data->n_outstanding_content_write_requests--;
  check_outstanding_requests_handle_error (pull_data, local_error);
  g_variant_unref (fetch_data->object);
  g_free (fetch_data);
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data) 
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  guint64 length;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_object GInputStream *file_in = NULL;
  gs_unref_object GInputStream *object_input = NULL;
  gs_free char *temp_path = NULL;
  const char *checksum;
  OstreeObjectType objtype;

  temp_path = _ostree_fetcher_request_uri_with_partial_finish ((OstreeFetcher*)object, result, error);
  if (!temp_path)
    goto out;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  g_debug ("fetch of %s complete", ostree_object_to_string (checksum, objtype));

  if (pull_data->is_mirror && pull_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      gboolean have_object;
      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                   &have_object,
                                   cancellable, error))
        goto out;

      if (!have_object)
        {
          if (!_ostree_repo_commit_loose_final (pull_data->repo, checksum, OSTREE_OBJECT_TYPE_FILE,
                                                pull_data->tmpdir_dfd, temp_path,
                                                cancellable, error))
            goto out;
        }
      pull_data->n_fetched_content++;
    }
  else
    {
      /* Non-mirroring path */
      
      if (!ostree_content_file_parse_at (TRUE, pull_data->tmpdir_dfd, temp_path, FALSE,
                                         &file_in, &file_info, &xattrs,
                                         cancellable, error))
        {
          /* If it appears corrupted, delete it */
          (void) unlinkat (pull_data->tmpdir_dfd, temp_path, 0);
          goto out;
        }

      /* Also, delete it now that we've opened it, we'll hold
       * a reference to the fd.  If we fail to write later, then
       * the temp space will be cleaned up.
       */
      (void) unlinkat (pull_data->tmpdir_dfd, temp_path, 0);
      
      if (!ostree_raw_file_to_content_stream (file_in, file_info, xattrs,
                                              &object_input, &length,
                                              cancellable, error))
        goto out;
  
      pull_data->n_outstanding_content_write_requests++;
      ostree_repo_write_content_async (pull_data->repo, checksum,
                                       object_input, length,
                                       cancellable,
                                       content_fetch_on_write_complete, fetch_data);
    }

 out:
  pull_data->n_outstanding_content_fetches--;
  check_outstanding_requests_handle_error (pull_data, local_error);
}

static void
on_metadata_written (GObject           *object,
                     GAsyncResult      *result,
                     gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  const char *expected_checksum;
  OstreeObjectType objtype;
  gs_free char *checksum = NULL;
  gs_free guchar *csum = NULL;
  gs_free char *stringified_object = NULL;

  if (!ostree_repo_write_metadata_finish ((OstreeRepo*)object, result, 
                                          &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (OSTREE_OBJECT_TYPE_IS_META (objtype));

  stringified_object = ostree_object_to_string (checksum, objtype);
  g_debug ("write of %s complete", stringified_object);

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  if (!scan_one_metadata_object_c (pull_data, csum, objtype, 0,
                                   pull_data->cancellable, error))
    goto out;

 out:
  pull_data->n_outstanding_metadata_write_requests--;
  g_variant_unref (fetch_data->object);
  g_free (fetch_data);

  check_outstanding_requests_handle_error (pull_data, local_error);
}

/* GFile pointing to the <repodir>/state/<checksum>.commitpartial file */
static GFile *
get_commitpartial_path (OstreeRepo  *repo,
                        const char  *commit)
{
  gs_free char *commitpartial_filename = g_strdup_printf ("%s.commitpartial", commit);
  return g_file_get_child (repo->state_dir, commitpartial_filename);
}

static void
meta_fetch_on_complete (GObject           *object,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  gs_unref_variant GVariant *metadata = NULL;
  gs_free char *temp_path = NULL;
  const char *checksum;
  OstreeObjectType objtype;
  GError *local_error = NULL;
  GError **error = &local_error;
  gs_fd_close int fd = -1;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  g_debug ("fetch of %s%s complete", ostree_object_to_string (checksum, objtype),
           fetch_data->is_detached_meta ? " (detached)" : "");

  temp_path = _ostree_fetcher_request_uri_with_partial_finish ((OstreeFetcher*)object, result, error);
  if (!temp_path)
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        goto out;
      else if (fetch_data->is_detached_meta)
        {
          /* There isn't any detached metadata, just fetch the commit */
          g_clear_error (&local_error);
          enqueue_one_object_request (pull_data, checksum, objtype, FALSE);
        }

      goto out;
    }

  fd = openat (pull_data->tmpdir_dfd, temp_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  if (fetch_data->is_detached_meta)
    {
      if (!ot_util_variant_map_fd (fd, 0, G_VARIANT_TYPE ("a{sv}"),
                                   FALSE, &metadata, error))
        goto out;

      /* Now delete it, see comment in corresponding content fetch path */
      (void) unlinkat (pull_data->tmpdir_dfd, temp_path, 0);

      if (!ostree_repo_write_commit_detached_metadata (pull_data->repo, checksum, metadata,
                                                       pull_data->cancellable, error))
        goto out;

      enqueue_one_object_request (pull_data, checksum, objtype, FALSE);
    }
  else
    {
      if (!ot_util_variant_map_fd (fd, 0, ostree_metadata_variant_type (objtype),
                                   FALSE, &metadata, error))
        goto out;

      (void) unlinkat (pull_data->tmpdir_dfd, temp_path, 0);

      /* Write the commitpartial file now while we're still fetching data */
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          GFile *commitpartial_path = get_commitpartial_path (pull_data->repo, checksum);

          if (!g_file_query_exists (commitpartial_path, NULL))
            {
              if (!g_file_replace_contents (commitpartial_path, "", 0, NULL, FALSE, 
                                            G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                            pull_data->cancellable, error))
                goto out;
            }
        }
      
      ostree_repo_write_metadata_async (pull_data->repo, objtype, checksum, metadata,
                                        pull_data->cancellable,
                                        on_metadata_written, fetch_data);
      pull_data->n_outstanding_metadata_write_requests++;
    }

 out:
  g_assert (pull_data->n_outstanding_metadata_fetches > 0);
  pull_data->n_outstanding_metadata_fetches--;
  pull_data->n_fetched_metadata++;
  check_outstanding_requests_handle_error (pull_data, local_error);
  if (local_error)
    {
      g_variant_unref (fetch_data->object);
      g_free (fetch_data);
    }
}

static void
fetch_static_delta_data_free (gpointer  data)
{
  FetchStaticDeltaData *fetch_data = data;
  g_free (fetch_data->expected_checksum);
  g_variant_unref (fetch_data->objects);
  g_free (fetch_data);
}

static void
on_static_delta_written (GObject           *object,
                         GAsyncResult      *result,
                         gpointer           user_data)
{
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;

  g_debug ("execute static delta part %s complete", fetch_data->expected_checksum);

  if (!_ostree_static_delta_part_execute_finish (pull_data->repo, result, error))
    goto out;

 out:
  g_assert (pull_data->n_outstanding_deltapart_write_requests > 0);
  pull_data->n_outstanding_deltapart_write_requests--;
  check_outstanding_requests_handle_error (pull_data, local_error);
  /* Always free state */
  fetch_static_delta_data_free (fetch_data);
}

static void
static_deltapart_fetch_on_complete (GObject           *object,
                                    GAsyncResult      *result,
                                    gpointer           user_data)
{
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  gs_unref_variant GVariant *metadata = NULL;
  gs_free char *temp_path = NULL;
  gs_unref_object GInputStream *in = NULL;
  gs_free char *actual_checksum = NULL;
  gs_free guint8 *csum = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  gs_fd_close int fd = -1;

  g_debug ("fetch static delta part %s complete", fetch_data->expected_checksum);

  temp_path = _ostree_fetcher_request_uri_with_partial_finish ((OstreeFetcher*)object, result, error);
  if (!temp_path)
    goto out;

  fd = openat (pull_data->tmpdir_dfd, temp_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }
  in = g_unix_input_stream_new (fd, FALSE);

  /* TODO - consider making async */
  if (!ot_gio_checksum_stream (in, &csum, pull_data->cancellable, error))
    goto out;

  actual_checksum = ostree_checksum_from_bytes (csum);

  if (strcmp (actual_checksum, fetch_data->expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted static delta part; checksum expected='%s' actual='%s'",
                   fetch_data->expected_checksum, actual_checksum);
      goto out;
    }

  /* Might as well close the fd here */
  (void) g_input_stream_close (in, NULL, NULL);

  {
    GMappedFile *mfile = NULL;
    gs_unref_bytes GBytes *delta_data = NULL;

    mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
    if (!mfile)
      goto out;
    delta_data = g_mapped_file_get_bytes (mfile);
    g_mapped_file_unref (mfile);

    /* Unlink now while we're holding an open fd, so that on success
     * or error, the file will be gone.  This is particularly
     * important if say we hit e.g. ENOSPC.
     */
    (void) unlinkat (pull_data->tmpdir_dfd, temp_path, 0); 

    _ostree_static_delta_part_execute_async (pull_data->repo,
                                             fetch_data->objects,
                                             delta_data,
                                             pull_data->cancellable,
                                             on_static_delta_written,
                                             fetch_data);
    pull_data->n_outstanding_deltapart_write_requests++;
  }

 out:
  g_assert (pull_data->n_outstanding_deltapart_fetches > 0);
  pull_data->n_outstanding_deltapart_fetches--;
  pull_data->n_fetched_deltaparts++;
  check_outstanding_requests_handle_error (pull_data, local_error);
  if (local_error)
    fetch_static_delta_data_free (fetch_data);
}

static gboolean
scan_commit_object (OtPullData         *pull_data,
                    const char         *checksum,
                    guint               recursion_depth,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  gboolean have_parent;
  gs_unref_variant GVariant *commit = NULL;
  gs_unref_variant GVariant *parent_csum = NULL;
  gs_unref_variant GVariant *tree_contents_csum = NULL;
  gs_unref_variant GVariant *tree_meta_csum = NULL;
  gpointer depthp;
  gint depth;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (g_hash_table_lookup_extended (pull_data->commit_to_depth, checksum,
                                    NULL, &depthp))
    {
      depth = GPOINTER_TO_INT (depthp);
    }
  else
    {
      depth = pull_data->maxdepth;
      g_hash_table_insert (pull_data->commit_to_depth, g_strdup (checksum),
                           GINT_TO_POINTER (depth));
    }

  if (pull_data->gpg_verify)
    {
      gs_unref_object OstreeGpgVerifyResult *result = NULL;

      result = ostree_repo_verify_commit_ext (pull_data->repo,
                                              checksum,
                                              NULL,
                                              NULL,
                                              cancellable,
                                              error);

      if (result == NULL)
        goto out;

      /* Allow callers to output the results immediately. */
      g_signal_emit_by_name (pull_data->repo,
                             "gpg-verify-result",
                             checksum, result);

      if (ostree_gpg_verify_result_count_valid (result) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "GPG signatures found, but none are in trusted keyring");
          goto out;
        }
    }

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 1, "@ay", &parent_csum);
  have_parent = g_variant_n_children (parent_csum) > 0;
  if (have_parent && pull_data->maxdepth == -1)
    {
      if (!scan_one_metadata_object_c (pull_data,
                                       ostree_checksum_bytes_peek (parent_csum),
                                       OSTREE_OBJECT_TYPE_COMMIT, recursion_depth + 1,
                                       cancellable, error))
        goto out;
    }
  else if (have_parent && depth > 0)
    {
      char parent_checksum[65];
      gpointer parent_depthp;
      int parent_depth;

      ostree_checksum_inplace_from_bytes (ostree_checksum_bytes_peek (parent_csum), parent_checksum);
  
      if (g_hash_table_lookup_extended (pull_data->commit_to_depth, parent_checksum,
                                        NULL, &parent_depthp))
        {
          parent_depth = GPOINTER_TO_INT (parent_depthp);
        }
      else
        {
          parent_depth = depth - 1;
        }

      if (parent_depth >= 0)
        {
          g_hash_table_insert (pull_data->commit_to_depth, g_strdup (parent_checksum),
                               GINT_TO_POINTER (parent_depth));
          if (!scan_one_metadata_object_c (pull_data,
                                           ostree_checksum_bytes_peek (parent_csum),
                                           OSTREE_OBJECT_TYPE_COMMIT, recursion_depth + 1,
                                           cancellable, error))
            goto out;
        }
    }

  g_variant_get_child (commit, 6, "@ay", &tree_contents_csum);
  g_variant_get_child (commit, 7, "@ay", &tree_meta_csum);

  // If this is a metadata only pull, don't grab the top dirtree/dirmeta:
  if (!(pull_data->flags & OSTREE_REPO_PULL_FLAGS_METADATA))
    {
      if (!scan_one_metadata_object_c (pull_data,
                                       ostree_checksum_bytes_peek (tree_contents_csum),
                                       OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1,
                                       cancellable, error))
        goto out;

      if (!scan_one_metadata_object_c (pull_data,
                                       ostree_checksum_bytes_peek (tree_meta_csum),
                                       OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1,
                                       cancellable, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
scan_one_metadata_object (OtPullData         *pull_data,
                          const char         *csum,
                          OstreeObjectType    objtype,
                          guint               recursion_depth,
                          GCancellable       *cancellable,
                          GError            **error)
{
  guchar buf[32];
  ostree_checksum_inplace_to_bytes (csum, buf);
  
  return scan_one_metadata_object_c (pull_data, buf, objtype,
                                     recursion_depth,
                                     cancellable, error);
}

static gboolean
scan_one_metadata_object_c (OtPullData         *pull_data,
                            const guchar         *csum,
                            OstreeObjectType    objtype,
                            guint               recursion_depth,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *object = NULL;
  gs_free char *tmp_checksum = NULL;
  gboolean is_requested;
  gboolean is_stored;

  tmp_checksum = ostree_checksum_from_bytes (csum);
  object = ostree_object_name_serialize (tmp_checksum, objtype);

  if (g_hash_table_lookup (pull_data->scanned_metadata, object))
    return TRUE;

  is_requested = g_hash_table_lookup (pull_data->requested_metadata, tmp_checksum) != NULL;
  if (!ostree_repo_has_object (pull_data->repo, objtype, tmp_checksum, &is_stored,
                               cancellable, error))
    goto out;

  if (pull_data->remote_repo_local)
    {
      if (!ostree_repo_import_object_from (pull_data->repo, pull_data->remote_repo_local,
                                           objtype, tmp_checksum,
                                           cancellable, error))
        goto out;
      is_stored = TRUE;
      is_requested = TRUE;
    }

  if (!is_stored && !is_requested)
    {
      char *duped_checksum = g_strdup (tmp_checksum);
      gboolean do_fetch_detached;

      g_hash_table_insert (pull_data->requested_metadata, duped_checksum, duped_checksum);

      do_fetch_detached = (objtype == OSTREE_OBJECT_TYPE_COMMIT);
      enqueue_one_object_request (pull_data, tmp_checksum, objtype, do_fetch_detached);
    }
  else if (is_stored)
    {
      gboolean do_scan = pull_data->transaction_resuming || is_requested || pull_data->commitpartial_exists;

      /* For commits, check whether we only had a partial fetch */
      if (!do_scan && objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          gs_unref_object GFile *commitpartial_file = get_commitpartial_path (pull_data->repo, tmp_checksum);

          if (g_file_query_exists (commitpartial_file, NULL))
            {
              do_scan = TRUE;
              pull_data->commitpartial_exists = TRUE;
            }
          else if (pull_data->maxdepth != 0)
            {
              /* Not fully accurate, but the cost here of scanning all
               * input commit objects if we're doing a depth fetch is
               * pretty low.  We'll do more accurate handling of depth
               * when parsing the actual commit.
               */
              do_scan = TRUE;
            }
        }

      if (do_scan)
        {
          switch (objtype)
            {
            case OSTREE_OBJECT_TYPE_COMMIT:
              if (!scan_commit_object (pull_data, tmp_checksum, recursion_depth,
                                       pull_data->cancellable, error))
                goto out;
              break;
            case OSTREE_OBJECT_TYPE_DIR_META:
              break;
            case OSTREE_OBJECT_TYPE_DIR_TREE:
              if (!scan_dirtree_object (pull_data, tmp_checksum, recursion_depth,
                                        pull_data->cancellable, error))
                goto out;
              break;
            default:
              g_assert_not_reached ();
              break;
            }
        }
      g_hash_table_insert (pull_data->scanned_metadata, g_variant_ref (object), object);
      pull_data->n_scanned_metadata++;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
enqueue_one_object_request (OtPullData        *pull_data,
                            const char        *checksum,
                            OstreeObjectType   objtype,
                            gboolean           is_detached_meta)
{
  SoupURI *obj_uri = NULL;
  gboolean is_meta;
  FetchObjectData *fetch_data;
  gs_free char *objpath = NULL;
  guint64 *expected_max_size_p;
  guint64 expected_max_size;

  g_debug ("queuing fetch of %s.%s%s", checksum,
           ostree_object_type_to_string (objtype),
           is_detached_meta ? " (detached)" : "");

  if (is_detached_meta)
    {
      char buf[_OSTREE_LOOSE_PATH_MAX];
      _ostree_loose_path_with_suffix (buf, checksum, OSTREE_OBJECT_TYPE_COMMIT,
                                      pull_data->remote_mode, "meta");
      obj_uri = suburi_new (pull_data->base_uri, "objects", buf, NULL);
    }
  else
    {
      objpath = _ostree_get_relative_object_path (checksum, objtype, TRUE);
      obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);
    }

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);
  if (is_meta)
    {
      pull_data->n_outstanding_metadata_fetches++;
      pull_data->n_requested_metadata++;
    }
  else
    {
      pull_data->n_outstanding_content_fetches++;
      pull_data->n_requested_content++;
    }
  fetch_data = g_new0 (FetchObjectData, 1);
  fetch_data->pull_data = pull_data;
  fetch_data->object = ostree_object_name_serialize (checksum, objtype);
  fetch_data->is_detached_meta = is_detached_meta;

  expected_max_size_p = g_hash_table_lookup (pull_data->expected_commit_sizes, checksum);
  if (expected_max_size_p)
    expected_max_size = *expected_max_size_p;
  else if (is_meta)
    expected_max_size = OSTREE_MAX_METADATA_SIZE;
  else
    expected_max_size = 0;

  _ostree_fetcher_request_uri_with_partial_async (pull_data->fetcher, obj_uri,
                                                  expected_max_size,
                                                  is_meta ? OSTREE_REPO_PULL_METADATA_PRIORITY
                                                          : OSTREE_REPO_PULL_CONTENT_PRIORITY,
                                                  pull_data->cancellable,
                                                  is_meta ? meta_fetch_on_complete : content_fetch_on_complete, fetch_data);
  soup_uri_free (obj_uri);
}

static gboolean
repo_get_remote_option_inherit (OstreeRepo  *self,
                                const char  *remote_name,
                                const char  *option_name,
                                char       **out_value,
                                GError     **error)
{
  OstreeRepo *parent = ostree_repo_get_parent (self);
  gs_free char *value = NULL;
  gboolean ret = FALSE;

  if (!_ostree_repo_get_remote_option (self, remote_name, option_name, NULL, &value, error))
    goto out;

  if (value == NULL && parent != NULL)
    {
        if (!repo_get_remote_option_inherit (parent, remote_name, option_name, &value, error))
          goto out;
    }

  /* Success here just means no error occurred during lookup,
   * not necessarily that we found a value for the option name. */
  ot_transfer_out_value (out_value, &value);
  ret = TRUE;

 out:
  return ret;
}

static gboolean
load_remote_repo_config (OtPullData    *pull_data,
                         GKeyFile     **out_keyfile,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  gs_free char *contents = NULL;
  GKeyFile *ret_keyfile = NULL;
  SoupURI *target_uri = NULL;

  target_uri = suburi_new (pull_data->base_uri, "config", NULL);
  
  if (!fetch_uri_contents_utf8_sync (pull_data, target_uri, &contents,
                                     cancellable, error))
    goto out;

  ret_keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (ret_keyfile, contents, strlen (contents),
                                  0, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_keyfile, &ret_keyfile);
 out:
  g_clear_pointer (&ret_keyfile, (GDestroyNotify) g_key_file_unref);
  g_clear_pointer (&target_uri, (GDestroyNotify) soup_uri_free);
  return ret;
}

static gboolean
fetch_metadata_to_verify_delta_superblock (OtPullData      *pull_data,
                                           const char      *from_revision,
                                           const char      *checksum,
                                           GBytes          *superblock_data,
                                           GCancellable    *cancellable,
                                           GError         **error)
{
  gboolean ret = FALSE;
  gs_free char *meta_path = _ostree_get_relative_static_delta_detachedmeta_path (from_revision, checksum);
  gs_unref_bytes GBytes *detached_meta_data = NULL;
  SoupURI *target_uri = NULL;
  gs_unref_variant GVariant *metadata = NULL;

  target_uri = suburi_new (pull_data->base_uri, meta_path, NULL);

  if (!fetch_uri_contents_membuf_sync (pull_data, target_uri, FALSE, FALSE,
                                       &detached_meta_data,
                                       pull_data->cancellable, error))
    {
      g_prefix_error (error, "GPG verification enabled, but failed to fetch metadata: ");
      goto out;
    }

  metadata = g_variant_new_from_bytes (G_VARIANT_TYPE ("a{sv}"),
                                       detached_meta_data,
                                       FALSE);

  if (!_ostree_repo_gpg_verify_with_metadata (pull_data->repo, superblock_data,
                                              metadata, NULL, NULL,
                                              cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
request_static_delta_superblock_sync (OtPullData  *pull_data,
                                      const char  *from_revision,
                                      const char  *to_revision,
                                      GVariant   **out_delta_superblock,
                                      GCancellable *cancellable,
                                      GError     **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *ret_delta_superblock = NULL;
  gs_free char *delta_name =
    _ostree_get_relative_static_delta_superblock_path (from_revision, to_revision);
  gs_unref_bytes GBytes *delta_superblock_data = NULL;
  gs_unref_bytes GBytes *delta_meta_data = NULL;
  gs_unref_variant GVariant *delta_superblock = NULL;
  SoupURI *target_uri = NULL;
  
  target_uri = suburi_new (pull_data->base_uri, delta_name, NULL);
  
  if (!fetch_uri_contents_membuf_sync (pull_data, target_uri, FALSE, TRUE,
                                       &delta_superblock_data,
                                       pull_data->cancellable, error))
    goto out;
  
  if (delta_superblock_data)
    {
      if (pull_data->gpg_verify)
        {
          if (!fetch_metadata_to_verify_delta_superblock (pull_data,
                                                          from_revision,
                                                          to_revision,
                                                          delta_superblock_data,
                                                          pull_data->cancellable, error))
            goto out;
        }

      ret_delta_superblock = g_variant_new_from_bytes ((GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                                                       delta_superblock_data, FALSE);
    }
  
  ret = TRUE;
  gs_transfer_out_value (out_delta_superblock, &ret_delta_superblock);
 out:
  return ret;
}

static gboolean
process_one_static_delta_fallback (OtPullData   *pull_data,
                                   GVariant     *fallback_object,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *csum_v = NULL;
  gs_free char *checksum = NULL;
  guint8 objtype_y;
  OstreeObjectType objtype;
  gboolean is_stored;
  guint64 compressed_size, uncompressed_size;

  g_variant_get (fallback_object, "(y@aytt)",
                 &objtype_y, &csum_v, &compressed_size, &uncompressed_size);
  if (!ostree_validate_structureof_objtype (objtype_y, error))
    goto out;
  if (!ostree_validate_structureof_csum_v (csum_v, error))
    goto out;

  objtype = (OstreeObjectType)objtype_y;
  checksum = ostree_checksum_from_bytes_v (csum_v);

  pull_data->total_deltapart_size += compressed_size;

  if (!ostree_repo_has_object (pull_data->repo, objtype, checksum,
                               &is_stored,
                               cancellable, error))
    goto out;

  if (!is_stored)
    { 
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (!g_hash_table_lookup (pull_data->requested_metadata, checksum))
            {
              gboolean do_fetch_detached;
              g_hash_table_insert (pull_data->requested_metadata, checksum, checksum);
              
              do_fetch_detached = (objtype == OSTREE_OBJECT_TYPE_COMMIT);
              enqueue_one_object_request (pull_data, checksum, objtype, do_fetch_detached);
              checksum = NULL;  /* Transfer ownership */
            }
        }
      else
        {
          if (!g_hash_table_lookup (pull_data->requested_content, checksum))
            {
              g_hash_table_insert (pull_data->requested_content, checksum, checksum);
              enqueue_one_object_request (pull_data, checksum, OSTREE_OBJECT_TYPE_FILE, FALSE);
              checksum = NULL;  /* Transfer ownership */
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
process_one_static_delta (OtPullData   *pull_data,
                          const char   *from_revision,
                          const char   *to_revision,
                          GVariant     *delta_superblock,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *headers = NULL;
  gs_unref_variant GVariant *fallback_objects = NULL;
  guint i, n;

  /* Parsing OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */
  headers = g_variant_get_child_value (delta_superblock, 6);
  fallback_objects = g_variant_get_child_value (delta_superblock, 7);

  /* First process the fallbacks */
  n = g_variant_n_children (fallback_objects);
  for (i = 0; i < n; i++)
    {
      gs_unref_variant GVariant *fallback_object =
        g_variant_get_child_value (fallback_objects, i);

      if (!process_one_static_delta_fallback (pull_data,
                                              fallback_object,
                                              cancellable, error))
        goto out;
    }

  /* Write the to-commit object */
  {
    gs_unref_variant GVariant *to_csum_v = NULL;
    gs_free char *to_checksum = NULL;
    gs_unref_variant GVariant *to_commit = NULL;
    gboolean have_to_commit;

    to_csum_v = g_variant_get_child_value (delta_superblock, 3);
    if (!ostree_validate_structureof_csum_v (to_csum_v, error))
      goto out;
    to_checksum = ostree_checksum_from_bytes_v (to_csum_v);

    if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                 &have_to_commit, cancellable, error))
      goto out;
    
    if (!have_to_commit)
      {
        FetchObjectData *fetch_data = g_new0 (FetchObjectData, 1);
        fetch_data->pull_data = pull_data;
        fetch_data->object = ostree_object_name_serialize (to_checksum, OSTREE_OBJECT_TYPE_COMMIT);
        fetch_data->is_detached_meta = FALSE;

        to_commit = g_variant_get_child_value (delta_superblock, 4);

        ostree_repo_write_metadata_async (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                          to_commit,
                                          pull_data->cancellable,
                                          on_metadata_written, fetch_data);
        pull_data->n_outstanding_metadata_write_requests++;
      }
  }

  n = g_variant_n_children (headers);
  pull_data->n_total_deltaparts += n;
  
  for (i = 0; i < n; i++)
    {
      const guchar *csum;
      gs_unref_variant GVariant *header = NULL;
      gboolean have_all = FALSE;
      SoupURI *target_uri = NULL;
      gs_free char *deltapart_path = NULL;
      FetchStaticDeltaData *fetch_data;
      gs_unref_variant GVariant *csum_v = NULL;
      gs_unref_variant GVariant *objects = NULL;
      guint64 size, usize;
      guint32 version;

      header = g_variant_get_child_value (headers, i);
      g_variant_get (header, "(u@aytt@ay)", &version, &csum_v, &size, &usize, &objects);

      if (version > OSTREE_DELTAPART_VERSION)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Delta part has too new version %u", version);
          goto out;
        }

      csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        goto out;

      pull_data->total_deltapart_size += size;

      if (!_ostree_repo_static_delta_part_have_all_objects (pull_data->repo,
                                                            objects,
                                                            &have_all,
                                                            cancellable, error))
        goto out;

      if (have_all)
        {
          g_debug ("Have all objects from static delta %s-%s part %u",
                   from_revision ? from_revision : "empty", to_revision,
                   i);
          pull_data->n_fetched_deltaparts++;
          continue;
        }

      fetch_data = g_new0 (FetchStaticDeltaData, 1);
      fetch_data->pull_data = pull_data;
      fetch_data->objects = g_variant_ref (objects);
      fetch_data->expected_checksum = ostree_checksum_from_bytes_v (csum_v);

      deltapart_path = _ostree_get_relative_static_delta_part_path (from_revision, to_revision, i);

      target_uri = suburi_new (pull_data->base_uri, deltapart_path, NULL);
      _ostree_fetcher_request_uri_with_partial_async (pull_data->fetcher, target_uri, size,
                                                      OSTREE_FETCHER_DEFAULT_PRIORITY,
                                                      pull_data->cancellable,
                                                      static_deltapart_fetch_on_complete,
                                                      fetch_data);
      pull_data->n_outstanding_deltapart_fetches++;
      soup_uri_free (target_uri);
    }

  ret = TRUE;
 out:
  return ret;
}

/* documented in ostree-repo.c */
gboolean
ostree_repo_pull (OstreeRepo               *self,
                  const char               *remote_name,
                  char                    **refs_to_fetch,
                  OstreeRepoPullFlags       flags,
                  OstreeAsyncProgress      *progress,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  return ostree_repo_pull_one_dir (self, remote_name, NULL, refs_to_fetch, flags, progress, cancellable, error);
}

/* Documented in ostree-repo.c */
gboolean
ostree_repo_pull_one_dir (OstreeRepo               *self,
                          const char               *remote_name,
                          const char               *dir_to_pull,
                          char                    **refs_to_fetch,
                          OstreeRepoPullFlags       flags,
                          OstreeAsyncProgress      *progress,
                          GCancellable             *cancellable,
                          GError                  **error)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  if (dir_to_pull)
    g_variant_builder_add (&builder, "{s@v}", "subdir",
                           g_variant_new_variant (g_variant_new_string (dir_to_pull)));
  g_variant_builder_add (&builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));
  if (refs_to_fetch)
    g_variant_builder_add (&builder, "{s@v}", "refs",
                           g_variant_new_variant (g_variant_new_strv ((const char *const*) refs_to_fetch, -1)));

  return ostree_repo_pull_with_options (self, remote_name, g_variant_builder_end (&builder),
                                        progress, cancellable, error);
}

/* Documented in ostree-repo.c */
gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name_or_baseurl,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gboolean tls_permissive = FALSE;
  OstreeFetcherConfigFlags fetcher_flags = 0;
  gs_free char *remote_key = NULL;
  gs_free char *path = NULL;
  gs_free char *baseurl = NULL;
  gs_free char *metalink_url_str = NULL;
  gs_unref_hashtable GHashTable *requested_refs_to_fetch = NULL;
  gs_unref_hashtable GHashTable *commits_to_fetch = NULL;
  gs_free char *remote_mode_str = NULL;
  gs_unref_object OstreeMetalink *metalink = NULL;
  OtPullData pull_data_real = { 0, };
  OtPullData *pull_data = &pull_data_real;
  GKeyFile *remote_config = NULL;
  char **configured_branches = NULL;
  guint64 bytes_transferred;
  guint64 end_time;
  OstreeRepoPullFlags flags = 0;
  const char *dir_to_pull = NULL;
  char **refs_to_fetch = NULL;
  GSource *update_timeout = NULL;
  GSource *idle_src;

  if (options)
    {
      int flags_i;
      (void) g_variant_lookup (options, "refs", "^a&s", &refs_to_fetch);
      (void) g_variant_lookup (options, "flags", "i", &flags_i);
      /* Reduce risk of issues if enum happens to be 64 bit for some reason */
      flags = flags_i;
      (void) g_variant_lookup (options, "subdir", "&s", &dir_to_pull);
      (void) g_variant_lookup (options, "override-remote-name", "s", &pull_data->remote_name);
      (void) g_variant_lookup (options, "depth", "i", &pull_data->maxdepth);
    }

  g_return_val_if_fail (pull_data->maxdepth >= -1, FALSE);

  if (dir_to_pull)
    g_return_val_if_fail (dir_to_pull[0] == '/', FALSE);

  pull_data->is_mirror = (flags & OSTREE_REPO_PULL_FLAGS_MIRROR) > 0;

  pull_data->async_error = error;
  pull_data->main_context = g_main_context_ref_thread_default ();
  pull_data->loop = g_main_loop_new (pull_data->main_context, FALSE);
  pull_data->flags = flags;

  pull_data->repo = self;
  pull_data->progress = progress;

  pull_data->expected_commit_sizes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            NULL,
                                                            (GDestroyNotify)g_free);
  pull_data->commit_to_depth = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free,
                                                      NULL);
  pull_data->scanned_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                       (GDestroyNotify)g_variant_unref, NULL);
  pull_data->requested_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        (GDestroyNotify)g_free, NULL);
  pull_data->requested_metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                         (GDestroyNotify)g_free, NULL);
  pull_data->dir = g_strdup (dir_to_pull);

  pull_data->start_time = g_get_monotonic_time ();

  if (_ostree_repo_remote_name_is_file (remote_name_or_baseurl))
    {
      baseurl = g_strdup (remote_name_or_baseurl);
      /* For compatibility with pull-local, don't gpg verify local
       * pulls.
       */
      pull_data->gpg_verify = FALSE;
    }
  else
    {
      pull_data->remote_name = g_strdup (remote_name_or_baseurl);

      if (!ostree_repo_remote_get_gpg_verify (self, remote_name_or_baseurl,
                                              &pull_data->gpg_verify, error))
        goto out;
    }

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_REFS;

  if (!_ostree_repo_get_remote_boolean_option (self,
                                               remote_name_or_baseurl, "tls-permissive",
                                               FALSE, &tls_permissive, error))
    goto out;
  if (tls_permissive)
    fetcher_flags |= OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE;

  pull_data->tmpdir_dfd = pull_data->repo->tmp_dir_fd;
  pull_data->fetcher = _ostree_fetcher_new (pull_data->tmpdir_dfd, fetcher_flags);
  requested_refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  commits_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  {
    gs_free char *tls_client_cert_path = NULL;
    gs_free char *tls_client_key_path = NULL;

    if (!_ostree_repo_get_remote_option (self,
                                         remote_name_or_baseurl, "tls-client-cert-path",
                                         NULL, &tls_client_cert_path, error))
      goto out;
    if (!_ostree_repo_get_remote_option (self,
                                         remote_name_or_baseurl, "tls-client-key-path",
                                         NULL, &tls_client_key_path, error))
      goto out;

    if ((tls_client_cert_path != NULL) != (tls_client_key_path != NULL))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "remote \"%s\" must specify both \"tls-client-cert-path\" and \"tls-client-key-path\"",
                     remote_name_or_baseurl);
        goto out;
      }
    else if (tls_client_cert_path)
      {
        gs_unref_object GTlsCertificate *client_cert = NULL;

        g_assert (tls_client_key_path);

        client_cert = g_tls_certificate_new_from_files (tls_client_cert_path,
                                                        tls_client_key_path,
                                                        error);
        if (!client_cert)
          goto out;

        _ostree_fetcher_set_client_cert (pull_data->fetcher, client_cert);
      }
  }

  {
    gs_free char *tls_ca_path = NULL;
    gs_unref_object GTlsDatabase *db = NULL;

    if (!_ostree_repo_get_remote_option (self,
                                         remote_name_or_baseurl, "tls-ca-path",
                                         NULL, &tls_ca_path, error))
      goto out;

    if (tls_ca_path)
      {
        db = g_tls_file_database_new (tls_ca_path, error);
        if (!db)
          goto out;
        
        _ostree_fetcher_set_tls_database (pull_data->fetcher, db);
      }
  }

  {
    gs_free char *http_proxy = NULL;

    if (!_ostree_repo_get_remote_option (self,
                                         remote_name_or_baseurl, "proxy",
                                         NULL, &http_proxy, error))
      goto out;

    if (http_proxy)
      _ostree_fetcher_set_proxy (pull_data->fetcher, http_proxy);
  }

  if (!_ostree_repo_get_remote_option (self,
                                       remote_name_or_baseurl, "metalink",
                                       NULL, &metalink_url_str, error))
    goto out;

  if (!metalink_url_str)
    {
      if (baseurl == NULL)
        {
          if (!repo_get_remote_option_inherit (self, remote_name_or_baseurl, "url", &baseurl, error))
            goto out;
        }

      if (baseurl == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No \"url\" option in remote \"%s\"",
                       remote_name_or_baseurl);
          goto out;
        }

      pull_data->base_uri = soup_uri_new (baseurl);

      if (!pull_data->base_uri)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to parse url '%s'", baseurl);
          goto out;
        }
    }
  else
    {
      gs_free char *metalink_data = NULL;
      SoupURI *metalink_uri = soup_uri_new (metalink_url_str);
      SoupURI *target_uri = NULL;
      gs_fd_close int fd = -1;
      
      if (!metalink_uri)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid metalink URL: %s", metalink_url_str);
          goto out;
        }
      
      metalink = _ostree_metalink_new (pull_data->fetcher, "summary",
                                       OSTREE_MAX_METADATA_SIZE, metalink_uri);
      soup_uri_free (metalink_uri);

      if (! _ostree_metalink_request_sync (metalink,
                                           pull_data->loop,
                                           &target_uri,
                                           &metalink_data,
                                           &pull_data->fetching_sync_uri,
                                           cancellable,
                                           error))
        goto out;

      {
        gs_free char *repo_base = g_path_get_dirname (soup_uri_get_path (target_uri));
        pull_data->base_uri = soup_uri_copy (target_uri);
        soup_uri_set_path (pull_data->base_uri, repo_base);
      }

      fd = openat (pull_data->tmpdir_dfd, metalink_data, O_RDONLY | O_CLOEXEC);
      if (fd == -1)
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }

      if (!ot_util_variant_map_fd (fd, 0, OSTREE_SUMMARY_GVARIANT_FORMAT, FALSE,
                                   &pull_data->summary, error))
        goto out;
    }

  if (!_ostree_repo_get_remote_list_option (self,
                                            remote_name_or_baseurl, "branches",
                                            &configured_branches, error))
    goto out;

  if (strcmp (soup_uri_get_scheme (pull_data->base_uri), "file") == 0)
    {
      gs_unref_object GFile *remote_repo_path = g_file_new_for_path (soup_uri_get_path (pull_data->base_uri));
      pull_data->remote_repo_local = ostree_repo_new (remote_repo_path);
      if (!ostree_repo_open (pull_data->remote_repo_local, cancellable, error))
        goto out;
    }
  else
    {
      if (!load_remote_repo_config (pull_data, &remote_config, cancellable, error))
        goto out;

      if (!ot_keyfile_get_value_with_default (remote_config, "core", "mode", "bare",
                                              &remote_mode_str, error))
        goto out;

      if (!ostree_repo_mode_from_string (remote_mode_str, &pull_data->remote_mode, error))
        goto out;
    
      if (pull_data->remote_mode != OSTREE_REPO_MODE_ARCHIVE_Z2)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Can't pull from archives with mode \"%s\"",
                       remote_mode_str);
          goto out;
        }
    }

  pull_data->static_delta_superblocks = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  if (pull_data->is_mirror && !refs_to_fetch && !configured_branches)
    {
      SoupURI *summary_uri = NULL;
      gs_unref_bytes GBytes *bytes = NULL;
      gs_free char *ret_contents = NULL;
      
      summary_uri = suburi_new (pull_data->base_uri, "summary", NULL);
      if (!fetch_uri_contents_membuf_sync (pull_data, summary_uri, FALSE, TRUE,
                                           &bytes, cancellable, error))
        goto out;
      soup_uri_free (summary_uri);
      
      if (bytes)
        {
          gs_unref_variant GVariant *refs = NULL;
          gsize i, n;

          pull_data->summary_data = g_bytes_ref (bytes);
          pull_data->summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, FALSE);
          refs = g_variant_get_child_value (pull_data->summary, 0);
          n = g_variant_n_children (refs);
          for (i = 0; i < n; i++)
            {
              const char *refname;
              gs_unref_variant GVariant *ref = g_variant_get_child_value (refs, i);

              g_variant_get_child (ref, 0, "&s", &refname);

              if (!ostree_validate_rev (refname, error))
                goto out;
              
              g_hash_table_insert (requested_refs_to_fetch, g_strdup (refname), NULL);
            }
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Fetching all refs was requested in mirror mode, but remote repository does not have a summary");
          goto out;
        }

    } 
  else if (refs_to_fetch != NULL)
    {
      char **strviter;
      for (strviter = refs_to_fetch; *strviter; strviter++)
        {
          const char *branch = *strviter;

          if (ostree_validate_checksum_string (branch, NULL))
            {
              char *key = g_strdup (branch);
              g_hash_table_insert (commits_to_fetch, key, key);
            }
          else
            {
              g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), NULL);
            }
        }
    }
  else
    {
      char **branches_iter;

      branches_iter = configured_branches;

      if (!(branches_iter && *branches_iter))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No configured branches for remote %s", remote_name_or_baseurl);
          goto out;
        }
      for (;branches_iter && *branches_iter; branches_iter++)
        {
          const char *branch = *branches_iter;
              
          g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), NULL);
        }
    }

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *branch = key;
      char *contents = NULL;

      if (pull_data->summary)
        {
          gsize commit_size = 0;
          guint64 *malloced_size;

          if (!lookup_commit_checksum_from_summary (pull_data, branch, &contents, &commit_size, error))
            goto out;

          malloced_size = g_new0 (guint64, 1);
          *malloced_size = commit_size;
          g_hash_table_insert (pull_data->expected_commit_sizes, contents, malloced_size);
        }
      else
        {
          if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
            goto out;
        }
      
      /* Transfer ownership of contents */
      g_hash_table_replace (requested_refs_to_fetch, g_strdup (branch), contents);
    }

  /* Create the state directory here - it's new with the commitpartial code,
   * and may not exist in older repositories.
   */
  if (!gs_file_ensure_directory (pull_data->repo->state_dir, FALSE, pull_data->cancellable, error))
    goto out;

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_OBJECTS;

  if (!ostree_repo_prepare_transaction (pull_data->repo, &pull_data->transaction_resuming,
                                        cancellable, error))
    goto out;

  g_debug ("resuming transaction: %s", pull_data->transaction_resuming ? "true" : " false");

  g_hash_table_iter_init (&hash_iter, commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;
      if (!scan_one_metadata_object (pull_data, commit, OSTREE_OBJECT_TYPE_COMMIT,
                                     0, pull_data->cancellable, error))
        goto out;
    }

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      gs_free char *from_revision = NULL;
      const char *ref = key;
      const char *to_revision = value;
      GVariant *delta_superblock = NULL;

      if (!ostree_repo_resolve_rev (pull_data->repo, ref, TRUE,
                                    &from_revision, error))
        goto out;

#ifdef BUILDOPT_STATIC_DELTAS
      if (from_revision == NULL || g_strcmp0 (from_revision, to_revision) != 0)
        {
          if (!request_static_delta_superblock_sync (pull_data, from_revision, to_revision,
                                                     &delta_superblock, cancellable, error))
            goto out;
        }
#endif
          
      if (!delta_superblock)
        {
          g_debug ("no delta superblock for %s-%s", from_revision ? from_revision : "empty", to_revision);
          if (!scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT,
                                         0, pull_data->cancellable, error))
            goto out;
        }
      else
        {
          g_debug ("processing delta superblock for %s-%s", from_revision ? from_revision : "empty", to_revision);
          g_ptr_array_add (pull_data->static_delta_superblocks, g_variant_ref (delta_superblock));
          if (!process_one_static_delta (pull_data, from_revision, to_revision,
                                         delta_superblock,
                                         cancellable, error))
            goto out;
        }
    }

  idle_src = g_idle_source_new ();
  g_source_set_callback (idle_src, idle_check_outstanding_requests, pull_data, NULL);
  g_source_attach (idle_src, pull_data->main_context);
  g_source_unref (idle_src);

  if (pull_data->progress)
    {
      update_timeout = g_timeout_source_new_seconds (1);
      g_source_set_priority (update_timeout, G_PRIORITY_HIGH);
      g_source_set_callback (update_timeout, update_progress, pull_data, NULL);
      g_source_attach (update_timeout, g_main_loop_get_context (pull_data->loop));
      g_source_unref (update_timeout);
    }

  /* Now await work completion */
  g_main_loop_run (pull_data->loop);
  if (pull_data->caught_error)
    goto out;
  
  g_assert_cmpint (pull_data->n_outstanding_metadata_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_metadata_write_requests, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_write_requests, ==, 0);

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      gs_free char *remote_ref = NULL;
      gs_free char *original_rev = NULL;
          
      if (pull_data->remote_name)
        remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);
      else
        remote_ref = g_strdup (ref);

      if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
        goto out;
          
      if (original_rev && strcmp (checksum, original_rev) == 0)
        {
        }
      else
        {
          ostree_repo_transaction_set_ref (pull_data->repo, pull_data->is_mirror ? NULL : pull_data->remote_name,
                                          ref, checksum);
        }
    }

  if (pull_data->is_mirror && pull_data->summary_data)
    {
      if (!ot_file_replace_contents_at (pull_data->repo->repo_dir_fd, "summary",
                                        pull_data->summary_data, !pull_data->repo->disable_fsync,
                                        cancellable, error))
        goto out;
    }

  if (!ostree_repo_commit_transaction (pull_data->repo, NULL, cancellable, error))
    goto out;

  end_time = g_get_monotonic_time ();

  bytes_transferred = _ostree_fetcher_bytes_transferred (pull_data->fetcher);
  if (bytes_transferred > 0 && pull_data->progress)
    {
      guint shift; 
      GString *buf = g_string_new ("");

      if (bytes_transferred < 1024)
        shift = 1;
      else
        shift = 1024;

      if (pull_data->n_fetched_deltaparts > 0)
        g_string_append_printf (buf, "%u delta parts, %u loose fetched",
                                pull_data->n_fetched_deltaparts,
                                pull_data->n_fetched_metadata + pull_data->n_fetched_content);
      else
        g_string_append_printf (buf, "%u metadata, %u content objects fetched",
                                pull_data->n_fetched_metadata, pull_data->n_fetched_content);

      g_string_append_printf (buf, "; %" G_GUINT64_FORMAT " %s transferred in %u seconds",
                              (guint64)(bytes_transferred / shift),
                              shift == 1 ? "B" : "KiB",
                              (guint) ((end_time - pull_data->start_time) / G_USEC_PER_SEC));

      ostree_async_progress_set_status (pull_data->progress, buf->str);
      g_string_free (buf, TRUE);
    }

  /* iterate over commits fetched and delete any commitpartial files */
  if (!(dir_to_pull || (pull_data->flags & OSTREE_REPO_PULL_FLAGS_METADATA)))
    {
      g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          gs_unref_object GFile *commitpartial_path = get_commitpartial_path (pull_data->repo, checksum);
          if (!ot_gfile_ensure_unlinked (commitpartial_path, cancellable, error))
            goto out;
        }
        g_hash_table_iter_init (&hash_iter, commits_to_fetch);
        while (g_hash_table_iter_next (&hash_iter, &key, &value))
          {
            const char *commit = value;
            gs_unref_object GFile *commitpartial_path = get_commitpartial_path (pull_data->repo, commit);
            if (!ot_gfile_ensure_unlinked (commitpartial_path, cancellable, error))
              goto out;
          }
    }

  ret = TRUE;
 out:
  g_main_context_unref (pull_data->main_context);
  if (update_timeout)
    g_source_destroy (update_timeout);
  if (pull_data->loop)
    g_main_loop_unref (pull_data->loop);
  g_strfreev (configured_branches);
  g_clear_object (&pull_data->fetcher);
  g_clear_object (&pull_data->remote_repo_local);
  g_free (pull_data->remote_name);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  g_clear_pointer (&pull_data->summary_data, (GDestroyNotify) g_bytes_unref);
  g_clear_pointer (&pull_data->summary, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&pull_data->static_delta_superblocks, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->commit_to_depth, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->expected_commit_sizes, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->scanned_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&remote_config, (GDestroyNotify) g_key_file_unref);
  return ret;
}
