/*
 * Copyright (C) 2011,2012,2013 Colin Walters <walters@verbum.org>
 * Copyright © 2017 Endless Mobile, Inc.
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
 *
 * Authors:
 *  - Colin Walters <walters@verbum.org>
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include "libglnx.h"
#include "ostree.h"
#include "otutil.h"
#include "ostree-repo-pull-private.h"

#ifdef HAVE_LIBCURL_OR_LIBSOUP

#include "ostree-core-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-metalink.h"

#include "ostree-repo-finder.h"
#include "ostree-repo-finder-config.h"
#include "ostree-repo-finder-mount.h"
#ifdef HAVE_AVAHI
#include "ostree-repo-finder-avahi.h"
#endif  /* HAVE_AVAHI */

#include <gio/gunixinputstream.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#define OSTREE_MESSAGE_FETCH_COMPLETE_ID SD_ID128_MAKE(75,ba,3d,eb,0a,f0,41,a9,a4,62,72,ff,85,d9,e7,3e)

#define OSTREE_REPO_PULL_CONTENT_PRIORITY  (OSTREE_FETCHER_DEFAULT_PRIORITY)
#define OSTREE_REPO_PULL_METADATA_PRIORITY (OSTREE_REPO_PULL_CONTENT_PRIORITY - 100)

/* Arbitrarily chosen number of retries for all download operations when they
 * receive a transient network error (such as a socket timeout) — see
 * _ostree_fetcher_should_retry_request(). This is the default value for the
 * `n-network-retries` pull option. */
#define DEFAULT_N_NETWORK_RETRIES 5

typedef struct {
  OtPullData  *pull_data;
  GVariant    *object;
  char        *path;
  gboolean     is_detached_meta;

  /* Only relevant when is_detached_meta is TRUE.  Controls
   * whether to fetch the primary object after fetching its
   * detached metadata (no need if it's already stored). */
  gboolean     object_is_stored;

  OstreeCollectionRef *requested_ref;  /* (nullable) */
  guint n_retries_remaining;
} FetchObjectData;

typedef struct {
  OtPullData  *pull_data;
  GVariant *objects;
  char *expected_checksum;
  char *from_revision;
  char *to_revision;
  guint i;
  guint64 size;
  guint n_retries_remaining;
} FetchStaticDeltaData;

typedef struct {
  guchar csum[OSTREE_SHA256_DIGEST_LEN];
  char *path;
  OstreeObjectType objtype;
  guint recursion_depth; /* NB: not used anymore, though might be nice to print */
  OstreeCollectionRef *requested_ref;  /* (nullable) */
} ScanObjectQueueData;

typedef struct {
  OtPullData *pull_data;
  char *from_revision;
  char *to_revision;
  OstreeCollectionRef *requested_ref;  /* (nullable) */
  guint n_retries_remaining;
} FetchDeltaSuperData;

typedef struct {
  OtPullData *pull_data;
  char *from_revision;
  char *to_revision;
  OstreeCollectionRef *requested_ref;  /* (nullable) */
  guint n_retries_remaining;
} FetchDeltaIndexData;

static void
variant_or_null_unref (gpointer data)
{
  if (data)
    g_variant_unref (data);
}

static void start_fetch (OtPullData *pull_data, FetchObjectData *fetch);
static void start_fetch_deltapart (OtPullData *pull_data,
                                   FetchStaticDeltaData *fetch);
static void start_fetch_delta_superblock (OtPullData          *pull_data,
                                          FetchDeltaSuperData *fetch_data);
static void start_fetch_delta_index (OtPullData          *pull_data,
                                     FetchDeltaIndexData *fetch_data);
static gboolean fetcher_queue_is_full (OtPullData *pull_data);
static void queue_scan_one_metadata_object (OtPullData                *pull_data,
                                            const char                *csum,
                                            OstreeObjectType           objtype,
                                            const char                *path,
                                            guint                      recursion_depth,
                                            const OstreeCollectionRef *ref);

static void queue_scan_one_metadata_object_s (OtPullData                *pull_data,
                                              ScanObjectQueueData       *scan_data);
static void queue_scan_one_metadata_object_c (OtPullData                *pull_data,
                                              const guchar              *csum,
                                              OstreeObjectType           objtype,
                                              const char                *path,
                                              guint                      recursion_depth,
                                              const OstreeCollectionRef *ref);

static void enqueue_one_object_request_s (OtPullData      *pull_data,
                                          FetchObjectData *fetch_data);
static void enqueue_one_static_delta_index_request_s (OtPullData          *pull_data,
                                                      FetchDeltaIndexData *fetch_data);
static void enqueue_one_static_delta_superblock_request_s (OtPullData          *pull_data,
                                                           FetchDeltaSuperData *fetch_data);
static void enqueue_one_static_delta_part_request_s (OtPullData           *pull_data,
                                                     FetchStaticDeltaData *fetch_data);

static gboolean scan_one_metadata_object (OtPullData                 *pull_data,
                                          const char                 *checksum,
                                          OstreeObjectType            objtype,
                                          const char                 *path,
                                          guint                       recursion_depth,
                                          const OstreeCollectionRef  *ref,
                                          GCancellable               *cancellable,
                                          GError                    **error);
static void scan_object_queue_data_free (ScanObjectQueueData *scan_data);
static gboolean initiate_delta_request (OtPullData                *pull_data,
                                        const OstreeCollectionRef *ref,
                                        const char                *to_revision,
                                        const char                 *delta_from_revision,
                                        GError                    **error);

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

  /* In dry run, we only emit progress once metadata is done */
  if (pull_data->dry_run && pull_data->n_outstanding_metadata_fetches > 0)
    return TRUE;

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

  ostree_async_progress_set (pull_data->progress,
                             "outstanding-fetches", "u", outstanding_fetches,
                             "outstanding-writes", "u", outstanding_writes,
                             "fetched", "u", fetched,
                             "requested", "u", requested,
                             "scanning", "u", g_queue_is_empty (&pull_data->scan_object_queue) ? 0 : 1,
                             "caught-error", "b", pull_data->caught_error,
                             "scanned-metadata", "u", n_scanned_metadata,
                             "bytes-transferred", "t", bytes_transferred,
                             "start-time", "t", start_time,
                             /* We use these status keys even though we now also
                              * use these values for filesystem-local pulls.
                              */
                             "metadata-fetched-localcache", "u", pull_data->n_imported_metadata,
                             "content-fetched-localcache", "u", pull_data->n_imported_content,
                             /* Deltas */
                             "fetched-delta-parts",
                                  "u", pull_data->n_fetched_deltaparts,
                             "total-delta-parts",
                                  "u", pull_data->n_total_deltaparts,
                             "fetched-delta-fallbacks",
                                  "u", pull_data->n_fetched_deltapart_fallbacks,
                             "total-delta-fallbacks",
                                  "u", pull_data->n_total_delta_fallbacks,
                             "fetched-delta-part-size",
                                  "t", pull_data->fetched_deltapart_size,
                             "total-delta-part-size",
                                  "t", pull_data->total_deltapart_size,
                             "total-delta-part-usize",
                                  "t", pull_data->total_deltapart_usize,
                             "total-delta-superblocks",
                                  "u", pull_data->static_delta_superblocks->len,
                             /* We fetch metadata before content.  These allow us to report metadata fetch progress specifically. */
                             "outstanding-metadata-fetches", "u", pull_data->n_outstanding_metadata_fetches,
                             "metadata-fetched", "u", pull_data->n_fetched_metadata,
                             /* Overall status. */
                             "status", "s", "",
                             NULL);

  if (pull_data->dry_run)
    pull_data->dry_run_emitted_progress = TRUE;

  return TRUE;
}

/* The core logic function for whether we should continue the main loop */
static gboolean
pull_termination_condition (OtPullData          *pull_data)
{
  gboolean current_fetch_idle = (pull_data->n_outstanding_metadata_fetches == 0 &&
                                 pull_data->n_outstanding_content_fetches == 0 &&
                                 pull_data->n_outstanding_deltapart_fetches == 0);
  gboolean current_write_idle = (pull_data->n_outstanding_metadata_write_requests == 0 &&
                                 pull_data->n_outstanding_content_write_requests == 0 &&
                                 pull_data->n_outstanding_deltapart_write_requests == 0 );
  gboolean current_scan_idle = g_queue_is_empty (&pull_data->scan_object_queue);
  gboolean current_idle = current_fetch_idle && current_write_idle && current_scan_idle;

  /* we only enter the main loop when we're fetching objects */
  g_assert (pull_data->phase == OSTREE_PULL_PHASE_FETCHING_OBJECTS);

  if (pull_data->dry_run)
    return pull_data->dry_run_emitted_progress;

  if (current_idle)
    g_debug ("pull: idle, exiting mainloop");

  return current_idle;
}

/* Most async operations finish by calling this function; it will consume
 * @errorp if set, update statistics, and initiate processing of any further
 * requests as appropriate.
 */
static void
check_outstanding_requests_handle_error (OtPullData          *pull_data,
                                         GError             **errorp)
{
  g_assert (errorp);

  GError *error = *errorp;
  if (error)
    {
      g_debug ("Request caught error: %s", error->message);

      if (!pull_data->caught_error)
        {
          pull_data->caught_error = TRUE;
          g_propagate_error (pull_data->async_error, g_steal_pointer (errorp));
        }
      else
        {
          g_clear_error (errorp);
        }
    }

  /* If we're in error state, we wait for any pending operations to complete,
   * but ensure that all no further operations are queued.
   */
  if (pull_data->caught_error)
    {
      g_queue_foreach (&pull_data->scan_object_queue, (GFunc) scan_object_queue_data_free, NULL);
      g_queue_clear (&pull_data->scan_object_queue);
      g_hash_table_remove_all (pull_data->pending_fetch_metadata);
      g_hash_table_remove_all (pull_data->pending_fetch_delta_indexes);
      g_hash_table_remove_all (pull_data->pending_fetch_delta_superblocks);
      g_hash_table_remove_all (pull_data->pending_fetch_deltaparts);
      g_hash_table_remove_all (pull_data->pending_fetch_content);
    }
  else
    {
      GHashTableIter hiter;
      gpointer key, value;

      /* We may have just completed an async fetch operation. Now we look at
       * possibly enqueuing more requests. The goal of queuing is to both avoid
       * overloading the fetcher backend with HTTP requests, but also to
       * prioritize metadata fetches over content, so we have accurate
       * reporting. Hence here, we process metadata fetches first.
       */

      /* Try filling the queue with metadata we need to fetch */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_metadata);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          GVariant *objname = key;
          FetchObjectData *fetch = value;

          /* Steal both key and value */
          g_hash_table_iter_steal (&hiter);

          /* This takes ownership of the value */
          start_fetch (pull_data, fetch);
          /* And unref the key */
          g_variant_unref (objname);
        }

      /* Next, process delta index requests */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_delta_indexes);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          FetchDeltaIndexData *fetch = key;
          g_hash_table_iter_steal (&hiter);
          start_fetch_delta_index (pull_data, g_steal_pointer (&fetch));
        }

      /* Next, process delta superblock requests */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_delta_superblocks);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          FetchDeltaSuperData *fetch = key;
          g_hash_table_iter_steal (&hiter);
          start_fetch_delta_superblock (pull_data, g_steal_pointer (&fetch));
        }

      /* Now, process deltapart requests */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_deltaparts);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          FetchStaticDeltaData *fetch = key;
          g_hash_table_iter_steal (&hiter);
          /* Takes ownership */
          start_fetch_deltapart (pull_data, fetch);
        }

      /* Next, fill the queue with content */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_content);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          char *checksum = key;
          FetchObjectData *fetch = value;

          /* Steal both key and value */
          g_hash_table_iter_steal (&hiter);

          /* This takes ownership of the value */
          start_fetch (pull_data, fetch);
          /* And unref the key */
          g_free (checksum);
        }

    }
}

/* We have a total-request limit, as well has a hardcoded max of 2 for delta
 * parts. The logic for the delta one is that processing them is expensive, and
 * doing multiple simultaneously could risk space/memory on smaller devices. We
 * also throttle on outstanding writes in case fetches are faster.
 */
static gboolean
fetcher_queue_is_full (OtPullData *pull_data)
{
  const gboolean fetch_full =
      ((pull_data->n_outstanding_metadata_fetches +
        pull_data->n_outstanding_content_fetches +
        pull_data->n_outstanding_deltapart_fetches) ==
         _OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS);
  const gboolean deltas_full =
      (pull_data->n_outstanding_deltapart_fetches ==
        _OSTREE_MAX_OUTSTANDING_DELTAPART_REQUESTS);
  const gboolean writes_full =
      ((pull_data->n_outstanding_metadata_write_requests +
        pull_data->n_outstanding_content_write_requests +
        pull_data->n_outstanding_deltapart_write_requests) >=
         _OSTREE_MAX_OUTSTANDING_WRITE_REQUESTS);
  return fetch_full || deltas_full || writes_full;
}

static void
scan_object_queue_data_free (ScanObjectQueueData *scan_data)
{
  g_free (scan_data->path);
  if (scan_data->requested_ref != NULL)
    ostree_collection_ref_free (scan_data->requested_ref);
  g_free (scan_data);
}

/* Called out of the main loop to process the "scan object queue", which is a
 * queue of metadata objects (commits and dirtree, but not dirmeta) to parse to
 * look for further objects. Basically wraps execution of
 * `scan_one_metadata_object()`.
 */
static gboolean
idle_worker (gpointer user_data)
{
  OtPullData *pull_data = user_data;
  ScanObjectQueueData *scan_data;
  g_autoptr(GError) error = NULL;

  scan_data = g_queue_pop_head (&pull_data->scan_object_queue);
  if (!scan_data)
    {
      g_clear_pointer (&pull_data->idle_src, g_source_destroy);
      return G_SOURCE_REMOVE;
    }

  char checksum[OSTREE_SHA256_STRING_LEN+1];
  ostree_checksum_inplace_from_bytes (scan_data->csum, checksum);
  scan_one_metadata_object (pull_data, checksum, scan_data->objtype,
                            scan_data->path, scan_data->recursion_depth,
                            scan_data->requested_ref, pull_data->cancellable, &error);

  /* No need to retry scan tasks, since they’re local. */
  check_outstanding_requests_handle_error (pull_data, &error);
  scan_object_queue_data_free (scan_data);

  return G_SOURCE_CONTINUE;
}

static void
ensure_idle_queued (OtPullData *pull_data)
{
  GSource *idle_src;

  if (pull_data->idle_src)
    return;

  idle_src = g_idle_source_new ();
  g_source_set_callback (idle_src, idle_worker, pull_data, NULL);
  g_source_attach (idle_src, pull_data->main_context);
  pull_data->idle_src = idle_src;
  /* Ownership is transferred to pull_data */
  g_source_unref (idle_src);
}

typedef struct {
  OtPullData     *pull_data;
  GInputStream   *result_stream;
} OstreeFetchUriSyncData;

static gboolean
fetch_mirrored_uri_contents_utf8_sync (OstreeFetcher  *fetcher,
                                       GPtrArray      *mirrorlist,
                                       const char     *filename,
                                       guint           n_network_retries,
                                       char          **out_contents,
                                       GCancellable   *cancellable,
                                       GError        **error)
{
  g_autoptr(GBytes) bytes = NULL;
  if (!_ostree_fetcher_mirrored_request_to_membuf (fetcher, mirrorlist,
                                                   filename, OSTREE_FETCHER_REQUEST_NUL_TERMINATION,
                                                   NULL, 0,
                                                   n_network_retries,
                                                   &bytes, NULL, NULL, NULL,
                                                   OSTREE_MAX_METADATA_SIZE,
                                                   cancellable, error))
    return FALSE;

  gsize len;
  g_autofree char *ret_contents = g_bytes_unref_to_data (g_steal_pointer (&bytes), &len);

  if (!g_utf8_validate (ret_contents, -1, NULL))
    return glnx_throw (error, "Invalid UTF-8");

  ot_transfer_out_value (out_contents, &ret_contents);
  return TRUE;
}

static gboolean
fetch_uri_contents_utf8_sync (OstreeFetcher  *fetcher,
                              OstreeFetcherURI *uri,
                              guint           n_network_retries,
                              char          **out_contents,
                              GCancellable   *cancellable,
                              GError        **error)
{
  g_autoptr(GPtrArray) mirrorlist = g_ptr_array_new ();
  g_ptr_array_add (mirrorlist, uri); /* no transfer */
  return fetch_mirrored_uri_contents_utf8_sync (fetcher, mirrorlist,
                                                NULL, n_network_retries,
                                                out_contents,
                                                cancellable, error);
}

static void
enqueue_one_object_request (OtPullData                *pull_data,
                            const char                *checksum,
                            OstreeObjectType           objtype,
                            const char                *path,
                            gboolean                   is_detached_meta,
                            gboolean                   object_is_stored,
                            const OstreeCollectionRef *ref);

static gboolean
matches_pull_dir (const char *current_file,
                  const char *pull_dir,
                  gboolean current_file_is_dir)
{
  const char *rest;

  if (g_str_has_prefix (pull_dir, current_file))
    {
      rest = pull_dir + strlen (current_file);
      if (*rest == 0)
        {
          /* The current file is exactly the same as the specified
             pull dir. This matches always, even if the file is not a
             directory. */
          return TRUE;
        }

      if (*rest == '/')
        {
          /* The current file is a directory-prefix of the pull_dir.
             Match only if this is supposed to be a directory */
          return current_file_is_dir;
        }

      /* Matched a non-directory prefix such as /foo being a prefix of /fooo,
         no match */
      return FALSE;
    }

  if (g_str_has_prefix (current_file, pull_dir))
    {
      rest = current_file + strlen (pull_dir);
      /* Only match if the prefix match matched the entire directory
         component */
      return *rest == '/';
    }

  return FALSE;
}


static gboolean
pull_matches_subdir (OtPullData *pull_data,
                     const char *path,
                     const char *basename,
                     gboolean basename_is_dir)
{
  if (pull_data->dirs == NULL)
    return TRUE;

  g_autofree char *file = g_strconcat (path, basename, NULL);

  for (guint i = 0; i < pull_data->dirs->len; i++)
    {
      const char *pull_dir = g_ptr_array_index (pull_data->dirs, i);
      if (matches_pull_dir (file, pull_dir, basename_is_dir))
        return TRUE;
    }

  return FALSE;
}

typedef struct {
  OtPullData *pull_data;
  OstreeRepo *src_repo;
  char checksum[OSTREE_SHA256_STRING_LEN+1];
} ImportLocalAsyncData;

/* Asynchronously import a single content object. @src_repo is either
 * pull_data->remote_repo_local or one of pull_data->localcache_repos.
 */
static void
async_import_in_thread (GTask *task,
                        gpointer source,
                        gpointer task_data,
                        GCancellable *cancellable)
{
  ImportLocalAsyncData *iataskdata = task_data;
  OtPullData *pull_data = iataskdata->pull_data;
  g_autoptr(GError) local_error = NULL;
  /* pull_data->importflags was set up in the pull option processing */
  if (!_ostree_repo_import_object (pull_data->repo, iataskdata->src_repo,
                                   OSTREE_OBJECT_TYPE_FILE, iataskdata->checksum,
                                   pull_data->importflags, cancellable, &local_error))
    g_task_return_error (task, g_steal_pointer (&local_error));
  else
    g_task_return_boolean (task, TRUE);
}

/* Start an async import of a single object; currently used for content objects.
 * @src_repo is from pull_data->remote_repo_local or
 * pull_data->localcache_repos.
 *
 * One important special case here is handling the
 * OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES flag.
 */
static void
async_import_one_local_content_object (OtPullData *pull_data,
                                       OstreeRepo *src_repo,
                                       const char *checksum,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  ImportLocalAsyncData *iataskdata = g_new0 (ImportLocalAsyncData, 1);
  iataskdata->pull_data = pull_data;
  iataskdata->src_repo = src_repo;
  memcpy (iataskdata->checksum, checksum, OSTREE_SHA256_STRING_LEN);
  g_autoptr(GTask) task = g_task_new (pull_data->repo, cancellable, callback, user_data);
  g_task_set_source_tag (task, async_import_one_local_content_object);
  g_task_set_task_data (task, iataskdata, g_free);
  pull_data->n_outstanding_content_write_requests++;
  g_task_run_in_thread (task, async_import_in_thread);
}

static gboolean
async_import_one_local_content_object_finish (OtPullData *pull_data,
                                              GAsyncResult *result,
                                              GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, pull_data->repo), FALSE);
  return g_task_propagate_boolean ((GTask*)result, error);
}

static void
on_local_object_imported (GObject        *object,
                          GAsyncResult   *result,
                          gpointer        user_data)
{
  OtPullData *pull_data = user_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  if (!async_import_one_local_content_object_finish (pull_data, result, error))
    goto out;

 out:
  pull_data->n_imported_content++;
  g_assert_cmpint (pull_data->n_outstanding_content_write_requests, >, 0);
  pull_data->n_outstanding_content_write_requests--;
  /* No retries for local reads. */
  check_outstanding_requests_handle_error (pull_data, &local_error);
}

static gboolean
scan_dirtree_object (OtPullData   *pull_data,
                     const char   *checksum,
                     const char   *path,
                     int           recursion_depth,
                     GCancellable *cancellable,
                     GError      **error)
{
  g_autoptr(GVariant) tree = NULL;
  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &tree, error))
    return FALSE;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  g_autoptr(GVariant) files_variant = g_variant_get_child_value (tree, 0);
  const guint n = g_variant_n_children (files_variant);
  for (guint i = 0; i < n; i++)
    {
      const char *filename;
      gboolean file_is_stored;
      g_autoptr(GVariant) csum = NULL;
      g_autofree char *file_checksum = NULL;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      /* Note this is now obsoleted by the _ostree_validate_structureof_metadata()
       * but I'm keeping this since:
       *  1) It's cheap
       *  2) We want to continue to do validation for objects written to disk
       *     before libostree's validation was strengthened.
       */
      if (!ot_util_filename_validate (filename, error))
        return glnx_prefix_error (error, "File %u in dirtree", i);

      /* Skip files if we're traversing a request only directory, unless it exactly
       * matches the path */
      if (!pull_matches_subdir (pull_data, path, filename, FALSE))
        continue;

      file_checksum = ostree_checksum_from_bytes_v (csum);

      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                   &file_is_stored, cancellable, error))
        return FALSE;

      /* If we already have this object, move on to the next */
      if (file_is_stored)
        continue;

      /* Already have a request pending?  If so, move on to the next */
      if (g_hash_table_lookup (pull_data->requested_content, file_checksum))
        continue;

      /* Is this a local repo? */
      if (pull_data->remote_repo_local)
        {
          async_import_one_local_content_object (pull_data, pull_data->remote_repo_local,
                                                 file_checksum, cancellable,
                                                 on_local_object_imported,
                                                 pull_data);
          g_hash_table_add (pull_data->requested_content, g_steal_pointer (&file_checksum));
          /* Note early loop continue */
          continue;
        }

      /* We're doing HTTP, but see if we have the object in a local cache first */
      gboolean did_import_from_cache_repo = FALSE;
      if (pull_data->localcache_repos)
        {
          for (guint j = 0; j < pull_data->localcache_repos->len; j++)
            {
              OstreeRepo *localcache_repo = pull_data->localcache_repos->pdata[j];
              gboolean localcache_repo_has_obj;

              if (!ostree_repo_has_object (localcache_repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                           &localcache_repo_has_obj, cancellable, error))
                return FALSE;
              if (!localcache_repo_has_obj)
                continue;
              async_import_one_local_content_object (pull_data, localcache_repo, file_checksum, cancellable,
                                                     on_local_object_imported, pull_data);
              g_hash_table_add (pull_data->requested_content, g_steal_pointer (&file_checksum));
              did_import_from_cache_repo = TRUE;
              break;
            }
        }
      if (did_import_from_cache_repo)
        continue; /* Note early continue */

      /* Not available locally, queue a HTTP request */
      g_hash_table_add (pull_data->requested_content, file_checksum);
      enqueue_one_object_request (pull_data, file_checksum, OSTREE_OBJECT_TYPE_FILE, path, FALSE, FALSE, NULL);
      file_checksum = NULL;  /* Transfer ownership */
    }

  g_autoptr(GVariant) dirs_variant = g_variant_get_child_value (tree, 1);
  const guint m = g_variant_n_children (dirs_variant);
  for (guint i = 0; i < m; i++)
    {
      const char *dirname = NULL;
      g_autoptr(GVariant) tree_csum = NULL;
      g_autoptr(GVariant) meta_csum = NULL;
      g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                           &dirname, &tree_csum, &meta_csum);

      /* See comment above for files */
      if (!ot_util_filename_validate (dirname, error))
        return glnx_prefix_error (error, "Dir %u in dirtree", i);

      if (!pull_matches_subdir (pull_data, path, dirname, TRUE))
        continue;

      const guchar *tree_csum_bytes = ostree_checksum_bytes_peek_validate (tree_csum, error);
      if (tree_csum_bytes == NULL)
        return FALSE;

      const guchar *meta_csum_bytes = ostree_checksum_bytes_peek_validate (meta_csum, error);
      if (meta_csum_bytes == NULL)
        return FALSE;

      g_autofree char *subpath = g_strconcat (path, dirname, "/", NULL);
      queue_scan_one_metadata_object_c (pull_data, tree_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_TREE, subpath, recursion_depth + 1, NULL);
      queue_scan_one_metadata_object_c (pull_data, meta_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_META, subpath, recursion_depth + 1, NULL);
    }

  return TRUE;
}

/* Given a @ref, fetch its contents (should be a SHA256 ASCII string) */
static gboolean
fetch_ref_contents (OtPullData                 *pull_data,
                    const char                 *main_collection_id,
                    const OstreeCollectionRef  *ref,
                    char                      **out_contents,
                    GCancellable               *cancellable,
                    GError                    **error)
{
  g_autofree char *ret_contents = NULL;

  if (pull_data->remote_repo_local != NULL && ref->collection_id != NULL)
    {
      if (!ostree_repo_resolve_collection_ref (pull_data->remote_repo_local,
                                               ref, FALSE,
                                               OSTREE_REPO_RESOLVE_REV_EXT_NONE,
                                               &ret_contents, cancellable, error))
        return FALSE;
    }
  else if (pull_data->remote_repo_local != NULL)
    {
      if (!ostree_repo_resolve_rev_ext (pull_data->remote_repo_local,
                                        ref->ref_name, FALSE,
                                        OSTREE_REPO_RESOLVE_REV_EXT_NONE,
                                        &ret_contents, error))
        return FALSE;
    }
  else
    {
      g_autofree char *filename = NULL;

      if (ref->collection_id == NULL || g_strcmp0 (ref->collection_id, main_collection_id) == 0)
        filename = g_build_filename ("refs", "heads", ref->ref_name, NULL);
      else
        filename = g_build_filename ("refs", "mirrors", ref->collection_id, ref->ref_name, NULL);

      if (!fetch_mirrored_uri_contents_utf8_sync (pull_data->fetcher,
                                                  pull_data->meta_mirrorlist,
                                                  filename, pull_data->n_network_retries,
                                                  &ret_contents,
                                                  cancellable, error))
        return FALSE;

      g_strchomp (ret_contents);
    }

  g_assert (ret_contents);

  if (!ostree_validate_checksum_string (ret_contents, error))
    return glnx_prefix_error (error, "Fetching checksum for ref (%s, %s)",
                              ref->collection_id ?: "(empty)",
                              ref->ref_name);

  ot_transfer_out_value (out_contents, &ret_contents);
  return TRUE;
}

static gboolean
lookup_commit_checksum_and_collection_from_summary (OtPullData                 *pull_data,
                                                    const OstreeCollectionRef  *ref,
                                                    char                      **out_checksum,
                                                    gsize                      *out_size,
                                                    char                      **out_collection_id,
                                                    GError                    **error)
{
  g_autoptr(GVariant) additional_metadata = g_variant_get_child_value (pull_data->summary, 1);
  const gchar *main_collection_id;

  if (!g_variant_lookup (additional_metadata, OSTREE_SUMMARY_COLLECTION_ID, "&s", &main_collection_id))
    main_collection_id = NULL;

  g_autoptr(GVariant) refs = NULL;
  const gchar *resolved_collection_id = NULL;

  if (ref->collection_id == NULL || g_strcmp0 (ref->collection_id, main_collection_id) == 0)
    {
      refs = g_variant_get_child_value (pull_data->summary, 0);
      resolved_collection_id = main_collection_id;
    }
  else if (ref->collection_id != NULL)
    {
      g_autoptr(GVariant) collection_map = NULL;

      collection_map = g_variant_lookup_value (additional_metadata, OSTREE_SUMMARY_COLLECTION_MAP,
                                               G_VARIANT_TYPE ("a{sa(s(taya{sv}))}"));
      if (collection_map != NULL)
        refs = g_variant_lookup_value (collection_map, ref->collection_id, G_VARIANT_TYPE ("a(s(taya{sv}))"));
      resolved_collection_id = ref->collection_id;
    }

  int i;
  if (refs == NULL || !ot_variant_bsearch_str (refs, ref->ref_name, &i))
    {
      if (ref->collection_id != NULL)
        return glnx_throw (error, "No such branch (%s, %s) in repository summary", ref->collection_id, ref->ref_name);
      else
        return glnx_throw (error, "No such branch '%s' in repository summary", ref->ref_name);
    }

  g_autoptr(GVariant) refdata = g_variant_get_child_value (refs, i);
  g_autoptr(GVariant) reftargetdata = g_variant_get_child_value (refdata, 1);
  guint64 commit_size;
  g_autoptr(GVariant) commit_csum_v = NULL;
  g_variant_get (reftargetdata, "(t@ay@a{sv})", &commit_size, &commit_csum_v, NULL);

  if (resolved_collection_id != NULL &&
      !ostree_validate_collection_id (resolved_collection_id, error))
    return FALSE;
  if (!ostree_validate_structureof_csum_v (commit_csum_v, error))
    return FALSE;

  *out_checksum = ostree_checksum_from_bytes_v (commit_csum_v);
  *out_size = commit_size;
  *out_collection_id = g_strdup (resolved_collection_id);
  return TRUE;
}

static void
fetch_object_data_free (FetchObjectData *fetch_data)
{
  g_variant_unref (fetch_data->object);
  g_free (fetch_data->path);
  if (fetch_data->requested_ref)
    ostree_collection_ref_free (fetch_data->requested_ref);
  g_free (fetch_data);
}

static void
content_fetch_on_write_complete (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  OstreeObjectType objtype;
  const char *expected_checksum;
  g_autofree guchar *csum = NULL;
  g_autofree char *checksum = NULL;
  g_autofree char *checksum_obj = NULL;

  if (!ostree_repo_write_content_finish ((OstreeRepo*)object, result,
                                         &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("write of %s complete", checksum_obj);

  if (!_ostree_compare_object_checksum (objtype, expected_checksum, checksum, error))
    goto out;

  pull_data->n_fetched_content++;
  /* Was this a delta fallback? */
  if (g_hash_table_remove (pull_data->requested_fallback_content, expected_checksum))
    pull_data->n_fetched_deltapart_fallbacks++;
 out:
  pull_data->n_outstanding_content_write_requests--;
  /* No retries for local writes. */
  check_outstanding_requests_handle_error (pull_data, &local_error);
  fetch_object_data_free (fetch_data);
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  guint64 length;
  g_auto(GLnxTmpfile) tmpf = { 0, };
  g_autoptr(GInputStream) tmpf_input = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  g_autoptr(GInputStream) file_in = NULL;
  g_autoptr(GInputStream) object_input = NULL;
  const char *checksum;
  g_autofree char *checksum_obj = NULL;
  OstreeObjectType objtype;
  gboolean free_fetch_data = TRUE;

  if (!_ostree_fetcher_request_to_tmpfile_finish (fetcher, result, &tmpf, NULL, NULL, NULL, error))
    goto out;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("fetch of %s complete", checksum_obj);

  const gboolean verifying_bareuseronly =
    (pull_data->importflags & _OSTREE_REPO_IMPORT_FLAGS_VERIFY_BAREUSERONLY) > 0;

  /* See comments where we set this variable; this is implementing
   * the --trusted-http/OSTREE_REPO_PULL_FLAGS_TRUSTED_HTTP flags.
   */
  if (pull_data->trusted_http_direct)
    {
      g_assert (!verifying_bareuseronly);
      if (!_ostree_repo_commit_tmpf_final (pull_data->repo, checksum, objtype,
                                           &tmpf, cancellable, error))
        goto out;
      pull_data->n_fetched_content++;
    }
  else
    {
      struct stat stbuf;
      if (!glnx_fstat (tmpf.fd, &stbuf, error))
        goto out;
      /* Non-mirroring path */
      tmpf_input = g_unix_input_stream_new (glnx_steal_fd (&tmpf.fd), TRUE);

      /* If it appears corrupted, we'll delete it below */
      if (!ostree_content_stream_parse (TRUE, tmpf_input, stbuf.st_size, FALSE,
                                        &file_in, &file_info, &xattrs,
                                        cancellable, error))
        {
          g_prefix_error (error, "Parsing %s: ", checksum_obj);
          goto out;
        }

      if (verifying_bareuseronly)
        {
          if (!_ostree_validate_bareuseronly_mode_finfo (file_info, checksum, error))
            goto out;
        }

      if (!ostree_raw_file_to_content_stream (file_in, file_info, xattrs,
                                              &object_input, &length,
                                              cancellable, error))
        goto out;

      pull_data->n_outstanding_content_write_requests++;
      ostree_repo_write_content_async (pull_data->repo, checksum,
                                       object_input, length,
                                       cancellable,
                                       content_fetch_on_write_complete, fetch_data);
      free_fetch_data = FALSE;
    }

 out:
  g_assert (pull_data->n_outstanding_content_fetches > 0);
  pull_data->n_outstanding_content_fetches--;

  if (_ostree_fetcher_should_retry_request (local_error, fetch_data->n_retries_remaining--))
    enqueue_one_object_request_s (pull_data, g_steal_pointer (&fetch_data));
  else
    check_outstanding_requests_handle_error (pull_data, &local_error);

  if (free_fetch_data)
    g_clear_pointer (&fetch_data, fetch_object_data_free);
}

static void
on_metadata_written (GObject           *object,
                     GAsyncResult      *result,
                     gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  const char *expected_checksum;
  OstreeObjectType objtype;
  g_autofree char *checksum = NULL;
  g_autofree guchar *csum = NULL;
  g_autofree char *stringified_object = NULL;

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

  queue_scan_one_metadata_object_c (pull_data, csum, objtype, fetch_data->path, 0, fetch_data->requested_ref);

 out:
  g_assert (pull_data->n_outstanding_metadata_write_requests > 0);
  pull_data->n_outstanding_metadata_write_requests--;
  fetch_object_data_free (fetch_data);

  /* No need to retry local write operations. */
  check_outstanding_requests_handle_error (pull_data, &local_error);
}

static gboolean
is_parent_commit (OtPullData *pull_data,
                  const char *checksum)
{
  /* FIXME: Only parent commits are added to the commit_to_depth table,
   * so if the checksum isn't in the table then a new commit chain is
   * being started. However, if the desired commit was a parent in a
   * previously followed chain, then this will be wrong.
   */
  return g_hash_table_contains (pull_data->commit_to_depth, checksum);
}

static void
meta_fetch_on_complete (GObject           *object,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GVariant) metadata = NULL;
  g_auto(GLnxTmpfile) tmpf = { 0, };
  const char *checksum;
  g_autofree char *checksum_obj = NULL;
  OstreeObjectType objtype;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  gboolean free_fetch_data = TRUE;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("fetch of %s%s complete", checksum_obj,
           fetch_data->is_detached_meta ? " (detached)" : "");

  if (!_ostree_fetcher_request_to_tmpfile_finish (fetcher, result, &tmpf, NULL, NULL, NULL, error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          if (fetch_data->is_detached_meta)
            {
              /* There isn't any detached metadata, just fetch the commit */
              g_clear_error (&local_error);

              /* Now that we've at least tried to fetch it, we can proceed to
               * scan/fetch the commit object */
              g_hash_table_insert (pull_data->fetched_detached_metadata, g_strdup (checksum), NULL);

              if (!fetch_data->object_is_stored)
                enqueue_one_object_request (pull_data, checksum, objtype, fetch_data->path, FALSE, FALSE, fetch_data->requested_ref);
              else
                queue_scan_one_metadata_object (pull_data, checksum, objtype, fetch_data->path, 0, fetch_data->requested_ref);
            }

          /* When traversing parents, do not fail on a missing commit.
           * We may be pulling from a partial repository that ends in a
           * dangling parent reference. This logic should match the
           * local case in scan_one_metadata_object.
           */
          else if (objtype == OSTREE_OBJECT_TYPE_COMMIT &&
                   pull_data->maxdepth != 0 &&
                   is_parent_commit (pull_data, checksum))
            {
              g_clear_error (&local_error);
              /* If the remote repo supports tombstone commits, check if the commit was intentionally
                 deleted.  */
              if (pull_data->has_tombstone_commits)
                {
                  enqueue_one_object_request (pull_data, checksum, OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT,
                                              fetch_data->path, FALSE, FALSE, NULL);
                }
            }
        }

      goto out;
    }

  /* Tombstone commits are always empty, so skip all processing here */
  if (objtype == OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT)
    goto out;

  if (fetch_data->is_detached_meta)
    {
      if (!ot_variant_read_fd (tmpf.fd, 0, G_VARIANT_TYPE ("a{sv}"),
                               FALSE, &metadata, error))
        goto out;

      if (!ostree_repo_write_commit_detached_metadata (pull_data->repo, checksum, metadata,
                                                       pull_data->cancellable, error))
        goto out;

      g_hash_table_insert (pull_data->fetched_detached_metadata, g_strdup (checksum), g_steal_pointer (&metadata));

      if (!fetch_data->object_is_stored)
        enqueue_one_object_request (pull_data, checksum, objtype, fetch_data->path, FALSE, FALSE, fetch_data->requested_ref);
      else
        queue_scan_one_metadata_object (pull_data, checksum, objtype, fetch_data->path, 0, fetch_data->requested_ref);
    }
  else
    {
      if (!ot_variant_read_fd (tmpf.fd, 0, ostree_metadata_variant_type (objtype),
                               FALSE, &metadata, error))
        goto out;

      /* Compute checksum and verify structure now. Note this is a recent change
       * (Jan 2018) - we used to verify the checksum only when writing down
       * below. But we want to do "structure" verification early on as well
       * before the object is written even to the staging directory.
       */
      if (!_ostree_verify_metadata_object (objtype, checksum, metadata, error))
        goto out;

      /* For commit objects, check the signature before writing to the repo,
       * and also write the .commitpartial to say that we're still processing
       * this commit.
       */
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          /* Do signature verification. `detached_data` may be NULL if no detached
           * metadata was found during pull; that's handled by
           * ostree_ostree_verify_unwritten_commit(). If we ever change the pull code to
           * not always fetch detached metadata, this bit will have to learn how
           * to look up from the disk state as well, or insert the on-disk
           * metadata into this hash.
           */
          GVariant *detached_data = g_hash_table_lookup (pull_data->fetched_detached_metadata, checksum);
          if (!_verify_unwritten_commit (pull_data, checksum, metadata, detached_data,
                                         fetch_data->requested_ref, pull_data->cancellable, error))
            goto out;

          if (!ostree_repo_mark_commit_partial (pull_data->repo, checksum, TRUE, error))
            goto out;
        }

      /* Note that we now (Jan 2018) pass NULL for checksum, which means "don't
       * verify checksum", since we just did it above. Related to this...now
       * that we're doing all the verification here, one thing we could do later
       * just `glnx_link_tmpfile_at()` into the repository, like the content
       * fetch path does for trusted commits.
       */
      ostree_repo_write_metadata_async (pull_data->repo, objtype, NULL, metadata,
                                        pull_data->cancellable,
                                        on_metadata_written, fetch_data);
      pull_data->n_outstanding_metadata_write_requests++;
      free_fetch_data = FALSE;
    }

 out:
  g_assert (pull_data->n_outstanding_metadata_fetches > 0);
  pull_data->n_outstanding_metadata_fetches--;

  if (local_error == NULL)
    pull_data->n_fetched_metadata++;

  if (_ostree_fetcher_should_retry_request (local_error, fetch_data->n_retries_remaining--))
    enqueue_one_object_request_s (pull_data, g_steal_pointer (&fetch_data));
  else
    check_outstanding_requests_handle_error (pull_data, &local_error);

  if (free_fetch_data)
    g_clear_pointer (&fetch_data, fetch_object_data_free);
}

static void
fetch_static_delta_data_free (gpointer  data)
{
  FetchStaticDeltaData *fetch_data = data;
  g_free (fetch_data->expected_checksum);
  g_variant_unref (fetch_data->objects);
  g_free (fetch_data->from_revision);
  g_free (fetch_data->to_revision);
  g_free (fetch_data);
}

static void
on_static_delta_written (GObject           *object,
                         GAsyncResult      *result,
                         gpointer           user_data)
{
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  g_debug ("execute static delta part %s complete", fetch_data->expected_checksum);

  if (!_ostree_static_delta_part_execute_finish (pull_data->repo, result, error))
    goto out;

 out:
  g_assert (pull_data->n_outstanding_deltapart_write_requests > 0);
  pull_data->n_outstanding_deltapart_write_requests--;
  /* No need to retry on failure to write locally. */
  check_outstanding_requests_handle_error (pull_data, &local_error);
  /* Always free state */
  fetch_static_delta_data_free (fetch_data);
}

static void
static_deltapart_fetch_on_complete (GObject           *object,
                                    GAsyncResult      *result,
                                    gpointer           user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_auto(GLnxTmpfile) tmpf = { 0, };
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GVariant) part = NULL;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  gboolean free_fetch_data = TRUE;

  g_debug ("fetch static delta part %s complete", fetch_data->expected_checksum);

  if (!_ostree_fetcher_request_to_tmpfile_finish (fetcher, result, &tmpf, NULL, NULL, NULL, error))
    goto out;

  /* Transfer ownership of the fd */
  in = g_unix_input_stream_new (glnx_steal_fd (&tmpf.fd), TRUE);

  /* TODO - make async */
  if (!_ostree_static_delta_part_open (in, NULL, 0, fetch_data->expected_checksum,
                                       &part, pull_data->cancellable, error))
    goto out;

  _ostree_static_delta_part_execute_async (pull_data->repo,
                                           fetch_data->objects,
                                           part,
                                           pull_data->cancellable,
                                           on_static_delta_written,
                                           fetch_data);
  pull_data->n_outstanding_deltapart_write_requests++;
  free_fetch_data = FALSE;

 out:
  g_assert (pull_data->n_outstanding_deltapart_fetches > 0);
  pull_data->n_outstanding_deltapart_fetches--;

  if (local_error == NULL)
    pull_data->n_fetched_deltaparts++;

  if (_ostree_fetcher_should_retry_request (local_error, fetch_data->n_retries_remaining--))
    enqueue_one_static_delta_part_request_s (pull_data, g_steal_pointer (&fetch_data));
  else
    check_outstanding_requests_handle_error (pull_data, &local_error);

  if (free_fetch_data)
    g_clear_pointer (&fetch_data, fetch_static_delta_data_free);
}

static gboolean
commitstate_is_partial (OtPullData   *pull_data,
                        OstreeRepoCommitState commitstate)
{
  return pull_data->legacy_transaction_resuming
    || (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL) > 0;
}

#endif  /* HAVE_LIBCURL_OR_LIBSOUP */

/* Reads the collection-id of a given remote from the repo
 * configuration.
 */
static char *
get_real_remote_repo_collection_id (OstreeRepo  *repo,
                                    const gchar *remote_name)
{
  /* remote_name == NULL can happen for pull-local */
  if (!remote_name)
    return NULL;

  g_autofree gchar *remote_collection_id = NULL;
  if (!ostree_repo_get_remote_option (repo, remote_name, "collection-id", NULL,
                                      &remote_collection_id, NULL) ||
      (remote_collection_id == NULL) ||
      (remote_collection_id[0] == '\0'))
    return NULL;

  return g_steal_pointer (&remote_collection_id);
}

#ifdef HAVE_LIBCURL_OR_LIBSOUP

/* Reads the collection-id of the remote repo. Where it will be read
 * from depends on whether we pull from the "local" remote repo (the
 * "file://" URL) or "remote" remote repo (likely the "http(s)://"
 * URL).
 */
static char *
get_remote_repo_collection_id (OtPullData *pull_data)
{
  if (pull_data->remote_repo_local != NULL)
    {
      const char *remote_collection_id =
        ostree_repo_get_collection_id (pull_data->remote_repo_local);
      if ((remote_collection_id == NULL) ||
          (remote_collection_id[0] == '\0'))
        return NULL;
      return g_strdup (remote_collection_id);
    }

  return get_real_remote_repo_collection_id (pull_data->repo,
                                             pull_data->remote_name);
}

#endif  /* HAVE_LIBCURL_OR_LIBSOUP */

/* Check whether the given remote exists, has a `collection-id` key set, and it
 * equals @collection_id. If so, return %TRUE. Otherwise, %FALSE. */
static gboolean
check_remote_matches_collection_id (OstreeRepo  *repo,
                                    const gchar *remote_name,
                                    const gchar *collection_id)
{
  g_autofree gchar *remote_collection_id = NULL;

  remote_collection_id = get_real_remote_repo_collection_id (repo, remote_name);
  if (remote_collection_id == NULL)
    return FALSE;

  return g_str_equal (remote_collection_id, collection_id);
}

/**
 * ostree_repo_resolve_keyring_for_collection:
 * @self: an #OstreeRepo
 * @collection_id: the collection ID to look up a keyring for
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Find the GPG keyring for the given @collection_id, using the local
 * configuration from the given #OstreeRepo. This will search the configured
 * remotes for ones whose `collection-id` key matches @collection_id, and will
 * return the first matching remote.
 *
 * If multiple remotes match and have different keyrings, a debug message will
 * be emitted, and the first result will be returned. It is expected that the
 * keyrings should match.
 *
 * If no match can be found, a %G_IO_ERROR_NOT_FOUND error will be returned.
 *
 * Returns: (transfer full): #OstreeRemote containing the GPG keyring for
 *    @collection_id
 * Since: 2018.6
 */
OstreeRemote *
ostree_repo_resolve_keyring_for_collection (OstreeRepo    *self,
                                            const gchar   *collection_id,
                                            GCancellable  *cancellable,
                                            GError       **error)
{
#ifndef OSTREE_DISABLE_GPGME
  gsize i;
  g_auto(GStrv) remotes = NULL;
  g_autoptr(OstreeRemote) keyring_remote = NULL;

  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (ostree_validate_collection_id (collection_id, NULL), NULL);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  /* Look through all the currently configured remotes for the given collection. */
  remotes = ostree_repo_remote_list (self, NULL);

  for (i = 0; remotes != NULL && remotes[i] != NULL; i++)
    {
      g_autoptr(GError) local_error = NULL;

      if (!check_remote_matches_collection_id (self, remotes[i], collection_id))
        continue;

      if (keyring_remote == NULL)
        {
          g_debug ("%s: Found match for collection ‘%s’ in remote ‘%s’.",
                   G_STRFUNC, collection_id, remotes[i]);
          keyring_remote = _ostree_repo_get_remote_inherited (self, remotes[i], &local_error);

          if (keyring_remote == NULL)
            {
              g_debug ("%s: Error loading remote ‘%s’: %s",
                       G_STRFUNC, remotes[i], local_error->message);
              continue;
            }

          if (g_strcmp0 (keyring_remote->keyring, "") == 0 ||
              g_strcmp0 (keyring_remote->keyring, "/dev/null") == 0)
            {
              g_debug ("%s: Ignoring remote ‘%s’ as it has no keyring configured.",
                       G_STRFUNC, remotes[i]);
              g_clear_object (&keyring_remote);
              continue;
            }

          /* continue so we can catch duplicates */
        }
      else
        {
          g_debug ("%s: Duplicate keyring for collection ‘%s’ in remote ‘%s’."
                   "Keyring will be loaded from remote ‘%s’.",
                   G_STRFUNC, collection_id, remotes[i],
                   keyring_remote->name);
        }
    }

  if (keyring_remote != NULL)
    return g_steal_pointer (&keyring_remote);
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No keyring found configured locally for collection ‘%s’",
                   collection_id);
      return NULL;
    }
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "'%s': GPG feature is disabled in a build time",
          __FUNCTION__);
  return NULL;
#endif /* OSTREE_DISABLE_GPGME */
}

#ifdef HAVE_LIBCURL_OR_LIBSOUP

/* Look at a commit object, and determine whether there are
 * more things to fetch.
 */
static gboolean
scan_commit_object (OtPullData                 *pull_data,
                    const char                 *checksum,
                    guint                       recursion_depth,
                    const OstreeCollectionRef  *ref,
                    GCancellable               *cancellable,
                    GError                    **error)
{
  gpointer depthp;
  gint depth;
  if (g_hash_table_lookup_extended (pull_data->commit_to_depth, checksum,
                                    NULL, &depthp))
    {
      depth = GPOINTER_TO_INT (depthp);
    }
  else
    {
      depth = pull_data->maxdepth;
    }

#ifndef OSTREE_DISABLE_GPGME
  /* See comment in process_verify_result() - we now gpg check before writing,
   * but also ensure we've done it here if not already.
   */
  if (pull_data->gpg_verify &&
      !g_hash_table_contains (pull_data->verified_commits, checksum))
    {
      g_autoptr(OstreeGpgVerifyResult) result = NULL;
      const char *keyring_remote = NULL;

      if (ref != NULL)
        keyring_remote = g_hash_table_lookup (pull_data->ref_keyring_map, ref);
      if (keyring_remote == NULL)
        keyring_remote = pull_data->remote_name;

      result = ostree_repo_verify_commit_for_remote (pull_data->repo,
                                                     checksum,
                                                     keyring_remote,
                                                     cancellable,
                                                     error);
      if (!_process_gpg_verify_result (pull_data, checksum, result, error))
        return FALSE;
    }
#endif /* OSTREE_DISABLE_GPGME */

  if (pull_data->signapi_commit_verifiers &&
      !g_hash_table_contains (pull_data->signapi_verified_commits, checksum))
    {
      g_autoptr(GError) last_verification_error = NULL;
      gboolean found_any_signature = FALSE;
      gboolean found_valid_signature = FALSE;
      g_autofree char *success_message = NULL;

      for (guint i = 0; i < pull_data->signapi_commit_verifiers->len; i++)
        {
          OstreeSign *sign = pull_data->signapi_commit_verifiers->pdata[i];

          found_any_signature = TRUE;

          /* Set return to true if any sign fit */
          if (ostree_sign_commit_verify (sign,
                                          pull_data->repo,
                                          checksum,
                                          &success_message,
                                          cancellable,
                                          last_verification_error ? NULL : &last_verification_error))
            {
              found_valid_signature = TRUE;
              break;
            }
         }

      if (!found_any_signature)
        return glnx_throw (error, "No signatures found for commit %s", checksum);

      if (!found_valid_signature)
        {
          g_assert (last_verification_error);
          g_propagate_error (error, g_steal_pointer (&last_verification_error));
          return glnx_prefix_error (error, "Can't verify commit %s", checksum);
        }
      g_assert (success_message);
      g_hash_table_insert (pull_data->signapi_verified_commits, g_strdup (checksum), g_steal_pointer (&success_message));
    }

  /* If we found a legacy transaction flag, assume we have to scan.
   * We always do a scan of dirtree objects; see
   * https://github.com/ostreedev/ostree/issues/543
   */
  OstreeRepoCommitState commitstate;
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (pull_data->repo, checksum, &commit, &commitstate, error))
    return FALSE;

  if (!pull_data->disable_verify_bindings)
    {
      /* If ref is non-NULL then the commit we fetched was requested through
       * the branch, otherwise we requested a commit checksum without
       * specifying a branch.
       */
      g_autofree char *remote_collection_id = NULL;
      remote_collection_id = get_remote_repo_collection_id (pull_data);
      if (!_ostree_repo_verify_bindings (remote_collection_id,
                                         (ref != NULL) ? ref->ref_name : NULL,
                                         commit, error))
        return glnx_prefix_error (error, "Commit %s", checksum);
    }

  guint64 new_ts = ostree_commit_get_timestamp (commit);
  if (pull_data->timestamp_check)
    {
      /* We don't support timestamp checking while recursing right now */
      g_assert (ref);
      g_assert_cmpint (recursion_depth, ==, 0);
      const char *orig_rev = NULL;
      if (!g_hash_table_lookup_extended (pull_data->ref_original_commits,
                                         ref, NULL, (void**)&orig_rev))
        g_assert_not_reached ();

      g_autoptr(GVariant) orig_commit = NULL;
      if (orig_rev)
        {
          if (!ostree_repo_load_commit (pull_data->repo, orig_rev,
                                        &orig_commit, NULL, error))
            return glnx_prefix_error (error, "Reading %s for timestamp-check", ref->ref_name);

          guint64 orig_ts = ostree_commit_get_timestamp (orig_commit);
          if (!_ostree_compare_timestamps (orig_rev, orig_ts, checksum, new_ts, error))
            return FALSE;
        }
    }
  if (pull_data->timestamp_check_from_rev)
    {
      g_autoptr(GVariant) timestamp_commit = NULL;
      if (!ostree_repo_load_commit (pull_data->repo, pull_data->timestamp_check_from_rev,
                                    &timestamp_commit, NULL, error))
        return glnx_prefix_error (error, "Reading %s for timestamp-check-from-rev",
                                  pull_data->timestamp_check_from_rev);

      guint64 ts = ostree_commit_get_timestamp (timestamp_commit);
      if (!_ostree_compare_timestamps (pull_data->timestamp_check_from_rev, ts, checksum, new_ts, error))
        return FALSE;
    }

  /* If we found a legacy transaction flag, assume all commits are partial */
  gboolean is_partial = commitstate_is_partial (pull_data, commitstate);

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_autoptr(GVariant) parent_csum = NULL;
  const guchar *parent_csum_bytes = NULL;
  g_variant_get_child (commit, 1, "@ay", &parent_csum);
  if (g_variant_n_children (parent_csum) > 0)
    {
      parent_csum_bytes = ostree_checksum_bytes_peek_validate (parent_csum, error);
      if (parent_csum_bytes == NULL)
        return FALSE;
    }

  if (parent_csum_bytes != NULL && (pull_data->maxdepth == -1 || depth > 0))
    {
      char parent_checksum[OSTREE_SHA256_STRING_LEN+1];
      ostree_checksum_inplace_from_bytes (parent_csum_bytes, parent_checksum);

      int parent_depth = (depth > 0) ? depth - 1 : -1;
      g_hash_table_insert (pull_data->commit_to_depth, g_strdup (parent_checksum),
                           GINT_TO_POINTER (parent_depth));
      queue_scan_one_metadata_object_c (pull_data, parent_csum_bytes,
                                        OSTREE_OBJECT_TYPE_COMMIT,
                                        NULL,
                                        recursion_depth + 1,
                                        NULL);
    }

  /* We only recurse to looking whether we need dirtree/dirmeta
   * objects if the commit is partial, and we're not doing a
   * commit-only fetch.
   */
  if (is_partial && !pull_data->is_commit_only)
    {
      g_autoptr(GVariant) tree_contents_csum = NULL;
      g_autoptr(GVariant) tree_meta_csum = NULL;
      const guchar *tree_contents_csum_bytes;
      const guchar *tree_meta_csum_bytes;

      g_variant_get_child (commit, 6, "@ay", &tree_contents_csum);
      g_variant_get_child (commit, 7, "@ay", &tree_meta_csum);

      tree_contents_csum_bytes = ostree_checksum_bytes_peek_validate (tree_contents_csum, error);
      if (tree_contents_csum_bytes == NULL)
        return FALSE;

      tree_meta_csum_bytes = ostree_checksum_bytes_peek_validate (tree_meta_csum, error);
      if (tree_meta_csum_bytes == NULL)
        return FALSE;

      queue_scan_one_metadata_object_c (pull_data, tree_contents_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_TREE, "/", recursion_depth + 1, NULL);

      queue_scan_one_metadata_object_c (pull_data, tree_meta_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_META, NULL, recursion_depth + 1, NULL);
    }

  return TRUE;
}

static void
queue_scan_one_metadata_object (OtPullData                *pull_data,
                                const char                *csum,
                                OstreeObjectType           objtype,
                                const char                *path,
                                guint                      recursion_depth,
                                const OstreeCollectionRef *ref)
{
  guchar buf[OSTREE_SHA256_DIGEST_LEN];
  ostree_checksum_inplace_to_bytes (csum, buf);
  queue_scan_one_metadata_object_c (pull_data, buf, objtype, path, recursion_depth, ref);
}

static void
queue_scan_one_metadata_object_s (OtPullData          *pull_data,
                                  ScanObjectQueueData *scan_data)
{
  g_queue_push_tail (&pull_data->scan_object_queue, scan_data);
  ensure_idle_queued (pull_data);
}

static void
queue_scan_one_metadata_object_c (OtPullData                *pull_data,
                                  const guchar              *csum,
                                  OstreeObjectType           objtype,
                                  const char                *path,
                                  guint                      recursion_depth,
                                  const OstreeCollectionRef *ref)
{
  ScanObjectQueueData *scan_data = g_new0 (ScanObjectQueueData, 1);

  memcpy (scan_data->csum, csum, sizeof (scan_data->csum));
  scan_data->objtype = objtype;
  scan_data->path = g_strdup (path);
  scan_data->recursion_depth = recursion_depth;
  scan_data->requested_ref = (ref != NULL) ? ostree_collection_ref_dup (ref) : NULL;

  queue_scan_one_metadata_object_s (pull_data, g_steal_pointer (&scan_data));
}

/* Called out of the main loop to look at metadata objects which can have
 * further references (commit, dirtree). See also idle_worker() which drives
 * execution of this function.
 */
static gboolean
scan_one_metadata_object (OtPullData                 *pull_data,
                          const char                 *checksum,
                          OstreeObjectType            objtype,
                          const char                 *path,
                          guint                       recursion_depth,
                          const OstreeCollectionRef  *ref,
                          GCancellable               *cancellable,
                          GError                    **error)
{
  g_autoptr(GVariant) object = ostree_object_name_serialize (checksum, objtype);

  /* It may happen that we've already looked at this object (think shared
   * dirtree subtrees), if that's the case, we're done */
  if (g_hash_table_lookup (pull_data->scanned_metadata, object))
    return TRUE;

  gboolean is_requested = g_hash_table_lookup (pull_data->requested_metadata, object) != NULL;
  /* Determine if we already have the object */
  gboolean is_stored;
  if (!ostree_repo_has_object (pull_data->repo, objtype, checksum, &is_stored,
                               cancellable, error))
    return FALSE;

  /* Are we pulling an object we don't have from a local repo? */
  if (!is_stored && pull_data->remote_repo_local)
    {
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          /* mark as partial to ensure we scan the commit below */
          if (!ostree_repo_mark_commit_partial (pull_data->repo, checksum, TRUE, error))
            return FALSE;
        }

      g_autoptr(GError) local_error = NULL;
      if (!_ostree_repo_import_object (pull_data->repo, pull_data->remote_repo_local,
                                       objtype, checksum, pull_data->importflags,
                                       cancellable, &local_error))
        {
          /* When traversing parents, do not fail on a missing commit.
           * We may be pulling from a partial repository that ends in a
           * dangling parent reference. This logic should match the
           * remote case in meta_fetch_on_complete.
           *
           * Note early return.
           */
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
              objtype == OSTREE_OBJECT_TYPE_COMMIT &&
              pull_data->maxdepth != 0 &&
              is_parent_commit (pull_data, checksum))
            {
              g_clear_error (&local_error);

              /* If the remote repo supports tombstone commits, check if
               * the commit was intentionally deleted.
               */
              if (pull_data->has_tombstone_commits)
                {
                  if (!_ostree_repo_import_object (pull_data->repo, pull_data->remote_repo_local,
                                                   OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT,
                                                   checksum, pull_data->importflags,
                                                   cancellable, error))
                    return FALSE;
                }

              return TRUE;
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      /* The import API will fetch both the commit and detached metadata, so
       * add it to the hash to avoid re-fetching it below.
       */
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        g_hash_table_insert (pull_data->fetched_detached_metadata, g_strdup (checksum), NULL);
      pull_data->n_imported_metadata++;
      is_stored = TRUE;
      is_requested = TRUE;
    }
  /* Do we have any localcache repos? */
  else if (!is_stored && pull_data->localcache_repos)
    {
      for (guint i = 0; i < pull_data->localcache_repos->len; i++)
        {
          OstreeRepo *refd_repo = pull_data->localcache_repos->pdata[i];
          gboolean localcache_repo_has_obj;

          if (!ostree_repo_has_object (refd_repo, objtype, checksum,
                                       &localcache_repo_has_obj, cancellable, error))
            return FALSE;
          if (!localcache_repo_has_obj)
            continue;
          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              /* mark as partial to ensure we scan the commit below */
              if (!ostree_repo_mark_commit_partial (pull_data->repo, checksum, TRUE, error))
                return FALSE;
            }
          if (!_ostree_repo_import_object (pull_data->repo, refd_repo,
                                           objtype, checksum, pull_data->importflags,
                                           cancellable, error))
            return FALSE;
          /* See comment above */
          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            g_hash_table_insert (pull_data->fetched_detached_metadata, g_strdup (checksum), NULL);
          is_stored = TRUE;
          is_requested = TRUE;
          pull_data->n_imported_metadata++;
          break;
        }
    }

  if (!is_stored && !is_requested)
    {
      gboolean do_fetch_detached;

      g_hash_table_add (pull_data->requested_metadata, g_variant_ref (object));

      do_fetch_detached = (objtype == OSTREE_OBJECT_TYPE_COMMIT);
      enqueue_one_object_request (pull_data, checksum, objtype, path, do_fetch_detached, FALSE, ref);
    }
  else if (is_stored && objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      /* Even though we already have the commit, we always try to (re)fetch the
       * detached metadata before scanning it, in case new signatures appear.
       * https://github.com/projectatomic/rpm-ostree/issues/630 */
      if (!g_hash_table_contains (pull_data->fetched_detached_metadata, checksum))
        enqueue_one_object_request (pull_data, checksum, objtype, path, TRUE, TRUE, ref);
      else
        {
          if (!scan_commit_object (pull_data, checksum, recursion_depth, ref,
                                   pull_data->cancellable, error))
            return FALSE;

          g_hash_table_add (pull_data->scanned_metadata, g_variant_ref (object));
          pull_data->n_scanned_metadata++;
        }
    }
  else if (is_stored && objtype == OSTREE_OBJECT_TYPE_DIR_TREE)
    {
      if (!scan_dirtree_object (pull_data, checksum, path, recursion_depth,
                                pull_data->cancellable, error))
        return glnx_prefix_error (error, "Validating dirtree %s (%s)", checksum, path);

      g_hash_table_add (pull_data->scanned_metadata, g_variant_ref (object));
      pull_data->n_scanned_metadata++;
    }

  return TRUE;
}

static void
enqueue_one_object_request_s (OtPullData      *pull_data,
                              FetchObjectData *fetch_data)
{
  const char *checksum;
  OstreeObjectType objtype;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  gboolean is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);

  /* Are too many requests are in flight? */
  if (fetcher_queue_is_full (pull_data))
    {
      g_debug ("queuing fetch of %s.%s%s", checksum,
               ostree_object_type_to_string (objtype),
               fetch_data->is_detached_meta ? " (detached)" : "");

      if (is_meta)
        {
          g_hash_table_insert (pull_data->pending_fetch_metadata, g_variant_ref (fetch_data->object), fetch_data);
        }
      else
        {
          g_hash_table_insert (pull_data->pending_fetch_content, g_strdup (checksum), fetch_data);
        }
    }
  else
    {
      start_fetch (pull_data, fetch_data);
    }
}

static void
enqueue_one_object_request (OtPullData                *pull_data,
                            const char                *checksum,
                            OstreeObjectType           objtype,
                            const char                *path,
                            gboolean                   is_detached_meta,
                            gboolean                   object_is_stored,
                            const OstreeCollectionRef *ref)
{
  FetchObjectData *fetch_data;

  fetch_data = g_new0 (FetchObjectData, 1);
  fetch_data->pull_data = pull_data;
  fetch_data->object = ostree_object_name_serialize (checksum, objtype);
  fetch_data->path = g_strdup (path);
  fetch_data->is_detached_meta = is_detached_meta;
  fetch_data->object_is_stored = object_is_stored;
  fetch_data->requested_ref = (ref != NULL) ? ostree_collection_ref_dup (ref) : NULL;
  fetch_data->n_retries_remaining = pull_data->n_network_retries;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    pull_data->n_requested_metadata++;
  else
    pull_data->n_requested_content++;

  enqueue_one_object_request_s (pull_data, g_steal_pointer (&fetch_data));
}

static void
start_fetch (OtPullData *pull_data,
             FetchObjectData *fetch)
{
  g_autofree char *obj_subpath = NULL;
  guint64 *expected_max_size_p;
  guint64 expected_max_size;
  const char *expected_checksum;
  OstreeObjectType objtype;
  GPtrArray *mirrorlist = NULL;

  ostree_object_name_deserialize (fetch->object, &expected_checksum, &objtype);

  g_debug ("starting fetch of %s.%s%s", expected_checksum,
           ostree_object_type_to_string (objtype),
           fetch->is_detached_meta ? " (detached)" : "");

  gboolean is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);
  if (is_meta)
    pull_data->n_outstanding_metadata_fetches++;
  else
    pull_data->n_outstanding_content_fetches++;

  OstreeFetcherRequestFlags flags = 0;
  /* Override the path if we're trying to fetch the .commitmeta file first */
  if (fetch->is_detached_meta)
    {
      char buf[_OSTREE_LOOSE_PATH_MAX];
      _ostree_loose_path (buf, expected_checksum, OSTREE_OBJECT_TYPE_COMMIT_META, pull_data->remote_mode);
      obj_subpath = g_build_filename ("objects", buf, NULL);
      mirrorlist = pull_data->meta_mirrorlist;
      flags |= OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT;
    }
  else
    {
      obj_subpath = _ostree_get_relative_object_path (expected_checksum, objtype, TRUE);
      mirrorlist = pull_data->content_mirrorlist;
    }

  /* We may have determined maximum sizes from the summary file content; if so,
   * honor it. Otherwise, metadata has a baseline max size.
   */
  expected_max_size_p = fetch->is_detached_meta ? NULL : g_hash_table_lookup (pull_data->expected_commit_sizes, expected_checksum);
  if (expected_max_size_p)
    expected_max_size = *expected_max_size_p;
  else if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    expected_max_size = pull_data->max_metadata_size;
  else
    expected_max_size = 0;

  if (!is_meta && pull_data->trusted_http_direct)
    flags |= OSTREE_FETCHER_REQUEST_LINKABLE;
  _ostree_fetcher_request_to_tmpfile (pull_data->fetcher, mirrorlist,
                                      obj_subpath, flags, NULL, 0, expected_max_size,
                                      is_meta ? OSTREE_REPO_PULL_METADATA_PRIORITY
                                      : OSTREE_REPO_PULL_CONTENT_PRIORITY,
                                      pull_data->cancellable,
                                      is_meta ? meta_fetch_on_complete : content_fetch_on_complete, fetch);
}

/* Deprecated: code should load options from the `summary` file rather than
 * downloading the remote’s `config` file, to save on network round trips. */
static gboolean
load_remote_repo_config (OtPullData    *pull_data,
                         GKeyFile     **out_keyfile,
                         GCancellable  *cancellable,
                         GError       **error)
{
  g_autofree char *contents = NULL;

  if (!fetch_mirrored_uri_contents_utf8_sync (pull_data->fetcher,
                                              pull_data->meta_mirrorlist,
                                              "config", pull_data->n_network_retries,
                                              &contents, cancellable, error))
    return FALSE;

  g_autoptr(GKeyFile) ret_keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (ret_keyfile, contents, strlen (contents),
                                  0, error))
    return glnx_prefix_error (error, "Parsing config");

  ot_transfer_out_value (out_keyfile, &ret_keyfile);
  return TRUE;
}

static gboolean
process_one_static_delta_fallback (OtPullData   *pull_data,
                                   gboolean      delta_byteswap,
                                   GVariant     *fallback_object,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  guint8 objtype_y;
  g_autoptr(GVariant) csum_v = NULL;
  guint64 compressed_size, uncompressed_size;

  g_variant_get (fallback_object, "(y@aytt)",
                 &objtype_y, &csum_v, &compressed_size, &uncompressed_size);

  if (!ostree_validate_structureof_objtype (objtype_y, error))
    return FALSE;
  if (!ostree_validate_structureof_csum_v (csum_v, error))
    return FALSE;

  compressed_size = maybe_swap_endian_u64 (delta_byteswap, compressed_size);
  uncompressed_size = maybe_swap_endian_u64 (delta_byteswap, uncompressed_size);

  pull_data->n_total_delta_fallbacks += 1;
  pull_data->total_deltapart_size += compressed_size;
  pull_data->total_deltapart_usize += uncompressed_size;

  OstreeObjectType objtype = (OstreeObjectType)objtype_y;
  g_autofree char *checksum = ostree_checksum_from_bytes_v (csum_v);

  gboolean is_stored;
  if (!ostree_repo_has_object (pull_data->repo, objtype, checksum,
                               &is_stored,
                               cancellable, error))
    return FALSE;

  if (is_stored)
    pull_data->fetched_deltapart_size += compressed_size;

  if (pull_data->dry_run)
    return TRUE; /* Note early return */

  if (!is_stored)
    {
      /* The delta compiler never did this, there's no reason to support it */
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        return glnx_throw (error, "Found metadata object as fallback: %s.%s", checksum,
                           ostree_object_type_to_string (objtype));
      else
        {
          if (!g_hash_table_lookup (pull_data->requested_content, checksum))
            {
              /* Mark this as requested, like we do in the non-delta path */
              g_hash_table_add (pull_data->requested_content, checksum);
              /* But also record it's a delta fallback object, so we can account
               * for it as logically part of the delta fetch.
               */
              g_hash_table_add (pull_data->requested_fallback_content, g_strdup (checksum));
              enqueue_one_object_request (pull_data, checksum, OSTREE_OBJECT_TYPE_FILE, NULL, FALSE, FALSE, NULL);
              checksum = NULL;  /* We transferred ownership to the requested_content hash */
            }
        }
    }

  return TRUE;
}

static void
enqueue_one_static_delta_part_request_s (OtPullData           *pull_data,
                                         FetchStaticDeltaData *fetch_data)
{
  if (fetcher_queue_is_full (pull_data))
    {
      g_debug ("queuing fetch of static delta %s-%s part %u",
               fetch_data->from_revision ?: "empty",
               fetch_data->to_revision, fetch_data->i);

      g_hash_table_add (pull_data->pending_fetch_deltaparts, fetch_data);
    }
  else
    {
      start_fetch_deltapart (pull_data, fetch_data);
    }
}

static void
start_fetch_deltapart (OtPullData *pull_data,
                       FetchStaticDeltaData *fetch)
{
  g_autofree char *deltapart_path = _ostree_get_relative_static_delta_part_path (fetch->from_revision, fetch->to_revision, fetch->i);
  g_debug ("starting fetch of deltapart %s", deltapart_path);
  pull_data->n_outstanding_deltapart_fetches++;
  g_assert_cmpint (pull_data->n_outstanding_deltapart_fetches, <=, _OSTREE_MAX_OUTSTANDING_DELTAPART_REQUESTS);
  _ostree_fetcher_request_to_tmpfile (pull_data->fetcher,
                                      pull_data->content_mirrorlist,
                                      deltapart_path, 0, NULL, 0, fetch->size,
                                      OSTREE_FETCHER_DEFAULT_PRIORITY,
                                      pull_data->cancellable,
                                      static_deltapart_fetch_on_complete,
                                      fetch);
}

static gboolean
process_one_static_delta (OtPullData                 *pull_data,
                          const char                 *from_revision,
                          const char                 *to_revision,
                          GVariant                   *delta_superblock,
                          const OstreeCollectionRef  *ref,
                          GCancellable               *cancellable,
                          GError                    **error)
{
  gboolean delta_byteswap = _ostree_delta_needs_byteswap (delta_superblock);

  /* Parsing OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */
  g_autoptr(GVariant) metadata = g_variant_get_child_value (delta_superblock, 0);
  g_autoptr(GVariant) headers = g_variant_get_child_value (delta_superblock, 6);
  g_autoptr(GVariant) fallback_objects = g_variant_get_child_value (delta_superblock, 7);

  /* Gather free space so we can do a check below */
  struct statvfs stvfsbuf;
  if (TEMP_FAILURE_RETRY (fstatvfs (pull_data->repo->repo_dir_fd, &stvfsbuf)) < 0)
    return glnx_throw_errno_prefix (error, "fstatvfs");

  /* First process the fallbacks */
  guint n = g_variant_n_children (fallback_objects);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr(GVariant) fallback_object =
        g_variant_get_child_value (fallback_objects, i);

      if (!process_one_static_delta_fallback (pull_data, delta_byteswap,
                                              fallback_object,
                                              cancellable, error))
        return FALSE;
    }

  /* Write the to-commit object */
  if (!pull_data->dry_run)
    {
      g_autoptr(GVariant) to_csum_v = g_variant_get_child_value (delta_superblock, 3);
      if (!ostree_validate_structureof_csum_v (to_csum_v, error))
        return FALSE;
      g_autofree char *to_checksum = ostree_checksum_from_bytes_v (to_csum_v);

      gboolean have_to_commit;
      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                   &have_to_commit, cancellable, error))
        return FALSE;

      if (!have_to_commit)
        {
          g_autoptr(GVariant) to_commit = g_variant_get_child_value (delta_superblock, 4);
          g_autofree char *detached_path = _ostree_get_relative_static_delta_path (from_revision, to_revision, "commitmeta");
          g_autoptr(GVariant) detached_data = g_variant_lookup_value (metadata, detached_path, G_VARIANT_TYPE("a{sv}"));

          if (!_verify_unwritten_commit (pull_data, to_revision, to_commit, detached_data,
                                         ref, cancellable, error))
            return FALSE;

          if (!ostree_repo_mark_commit_partial (pull_data->repo, to_revision, TRUE, error))
            return FALSE;

          if (detached_data && !ostree_repo_write_commit_detached_metadata (pull_data->repo,
                                                                            to_revision,
                                                                            detached_data,
                                                                            cancellable,
                                                                            error))
            return FALSE;

          FetchObjectData *fetch_data = g_new0 (FetchObjectData, 1);
          fetch_data->pull_data = pull_data;
          fetch_data->object = ostree_object_name_serialize (to_checksum, OSTREE_OBJECT_TYPE_COMMIT);
          fetch_data->is_detached_meta = FALSE;
          fetch_data->object_is_stored = FALSE;
          fetch_data->requested_ref = (ref != NULL) ? ostree_collection_ref_dup (ref) : NULL;
          fetch_data->n_retries_remaining = pull_data->n_network_retries;

          ostree_repo_write_metadata_async (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                            to_commit,
                                            pull_data->cancellable,
                                            on_metadata_written, fetch_data);
          pull_data->n_outstanding_metadata_write_requests++;
        }
    }

  n = g_variant_n_children (headers);
  pull_data->n_total_deltaparts += n;

  for (guint i = 0; i < n; i++)
    {
      gboolean have_all = FALSE;

      g_autoptr(GVariant) header = g_variant_get_child_value (headers, i);
      g_autoptr(GVariant) csum_v = NULL;
      g_autoptr(GVariant) objects = NULL;
      g_autoptr(GBytes) inline_part_bytes = NULL;
      guint32 version;
      guint64 size, usize;
      g_variant_get (header, "(u@aytt@ay)", &version, &csum_v, &size, &usize, &objects);
      version = maybe_swap_endian_u32 (delta_byteswap, version);
      size = maybe_swap_endian_u64 (delta_byteswap, size);
      usize = maybe_swap_endian_u64 (delta_byteswap, usize);

      if (version > OSTREE_DELTAPART_VERSION)
        return glnx_throw (error, "Delta part has too new version %u", version);

      const guchar *csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        return FALSE;

      if (!_ostree_repo_static_delta_part_have_all_objects (pull_data->repo,
                                                            objects,
                                                            &have_all,
                                                            cancellable, error))
        return FALSE;

      pull_data->total_deltapart_size += size;
      pull_data->total_deltapart_usize += usize;

      if (have_all)
        {
          g_debug ("Have all objects from static delta %s-%s part %u",
                   from_revision ?: "empty", to_revision,
                   i);
          pull_data->fetched_deltapart_size += size;
          pull_data->n_fetched_deltaparts++;
          continue;
        }

      g_autofree char *deltapart_path = _ostree_get_relative_static_delta_part_path (from_revision, to_revision, i);

      { g_autoptr(GVariant) part_datav =
          g_variant_lookup_value (metadata, deltapart_path, G_VARIANT_TYPE ("(yay)"));

        if (part_datav)
          inline_part_bytes = g_variant_get_data_as_bytes (part_datav);
      }

      if (pull_data->dry_run)
        continue;

      FetchStaticDeltaData *fetch_data = g_new0 (FetchStaticDeltaData, 1);
      fetch_data->from_revision = g_strdup (from_revision);
      fetch_data->to_revision = g_strdup (to_revision);
      fetch_data->pull_data = pull_data;
      fetch_data->objects = g_variant_ref (objects);
      fetch_data->expected_checksum = ostree_checksum_from_bytes_v (csum_v);
      fetch_data->size = size;
      fetch_data->i = i;
      fetch_data->n_retries_remaining = pull_data->n_network_retries;

      if (inline_part_bytes != NULL)
        {
          g_autoptr(GInputStream) memin = g_memory_input_stream_new_from_bytes (inline_part_bytes);
          g_autoptr(GVariant) inline_delta_part = NULL;

          /* For inline parts we are relying on per-commit GPG, so don't bother checksumming. */
          if (!_ostree_static_delta_part_open (memin, inline_part_bytes,
                                               OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM,
                                               NULL, &inline_delta_part,
                                               cancellable, error))
            {
              fetch_static_delta_data_free (fetch_data);
              return FALSE;
            }

          _ostree_static_delta_part_execute_async (pull_data->repo,
                                                   fetch_data->objects,
                                                   inline_delta_part,
                                                   pull_data->cancellable,
                                                   on_static_delta_written,
                                                   fetch_data);
          pull_data->n_outstanding_deltapart_write_requests++;
        }
      else
        {
          enqueue_one_static_delta_part_request_s (pull_data, g_steal_pointer (&fetch_data));
        }
    }

  /* The free space check is here since at this point we've parsed the delta not
   * only the total size of the parts, but also whether or not we already have
   * them. TODO: Ideally this free space check would be above, but we'd have to
   * walk everything twice and keep track of state.
   */
  const guint64 delta_required_blocks = (pull_data->total_deltapart_usize / stvfsbuf.f_bsize);
  if (delta_required_blocks > stvfsbuf.f_bfree)
    {
      g_autofree char *formatted_required = g_format_size (pull_data->total_deltapart_usize);
      g_autofree char *formatted_avail = g_format_size (((guint64)stvfsbuf.f_bsize) * stvfsbuf.f_bfree);
      return glnx_throw (error, "Delta requires %s free space, but only %s available",
                         formatted_required, formatted_avail);
    }

  return TRUE;
}

/*
 * DELTA_SEARCH_RESULT_UNCHANGED:
 * We already have the commit.
 *
 * DELTA_SEARCH_RESULT_NO_MATCH:
 * No deltas were found.
 *
 * DELTA_SEARCH_RESULT_FROM:
 * A regular delta was found, and the "from" revision will be
 * set in `from_revision`.
 *
 * DELTA_SEARCH_RESULT_SCRATCH:
 * There is a %NULL → @to_revision delta, also known as
 * a "from scratch" delta.
 */
typedef struct {
  enum {
    DELTA_SEARCH_RESULT_UNCHANGED,
    DELTA_SEARCH_RESULT_NO_MATCH,
    DELTA_SEARCH_RESULT_FROM,
    DELTA_SEARCH_RESULT_SCRATCH,
  } result;
  char from_revision[OSTREE_SHA256_STRING_LEN+1];
} DeltaSearchResult;

/* Loop over the static delta data we got from the summary,
 * and find the a delta path (if available) that goes to @to_revision.
 * See the enum in `DeltaSearchResult` for available result types.
 */
static gboolean
get_best_static_delta_start_for (OtPullData *pull_data,
                                 const char *to_revision,
                                 DeltaSearchResult   *out_result,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  /* Array<char*> of possible from checksums */
  g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func (g_free);
  const char *newest_candidate = NULL;
  guint64 newest_candidate_timestamp = 0;

  g_assert (pull_data->summary_deltas_checksums != NULL);

  out_result->result = DELTA_SEARCH_RESULT_NO_MATCH;
  out_result->from_revision[0] = '\0';

  /* First, do we already have this commit completely downloaded? */
  gboolean have_to_rev;
  if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT,
                               to_revision, &have_to_rev,
                               cancellable, error))
    return FALSE;
  if (have_to_rev)
    {
      OstreeRepoCommitState to_rev_state;
      if (!ostree_repo_load_commit (pull_data->repo, to_revision,
                                    NULL, &to_rev_state, error))
        return FALSE;
      if (!(commitstate_is_partial(pull_data, to_rev_state)))
        {
          /* We already have this commit, we're done! */
          out_result->result = DELTA_SEARCH_RESULT_UNCHANGED;
          return TRUE;  /* Early return */
        }
    }

  /* Loop over all deltas known from the summary file,
   * finding ones which go to to_revision */
  GLNX_HASH_TABLE_FOREACH (pull_data->summary_deltas_checksums, const char*, delta_name)
    {
      g_autofree char *cur_from_rev = NULL;
      g_autofree char *cur_to_rev = NULL;

      /* Gracefully handle corrupted (or malicious) summary files */
      if (!_ostree_parse_delta_name (delta_name, &cur_from_rev, &cur_to_rev, error))
        return FALSE;

      /* Is this the checksum we want? */
      if (strcmp (cur_to_rev, to_revision) != 0)
        continue;

      if (cur_from_rev)
        {
          g_ptr_array_add (candidates, g_steal_pointer (&cur_from_rev));
        }
      else
        {
          /* We note that we have a _SCRATCH delta here, but we'll prefer using
           * "from" deltas (obviously, they'll be smaller) where possible if we
           * find one below.
           */
          out_result->result = DELTA_SEARCH_RESULT_SCRATCH;
        }
    }

  /* Loop over our candidates, find the newest one */
  for (guint i = 0; i < candidates->len; i++)
    {
      const char *candidate = candidates->pdata[i];
      guint64 candidate_ts = 0;
      g_autoptr(GVariant) commit = NULL;
      OstreeRepoCommitState state;
      gboolean have_candidate;

      /* Do we have this commit at all?  If not, skip it */
      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                   candidate, &have_candidate,
                                   NULL, error))
        return FALSE;
      if (!have_candidate)
        continue;

      /* Load it */
      if (!ostree_repo_load_commit (pull_data->repo, candidate,
                                    &commit, &state, error))
        return FALSE;

      /* Ignore partial commits, we can't use them */
      if (state & OSTREE_REPO_COMMIT_STATE_PARTIAL)
        continue;

      /* Is it newer? */
      candidate_ts = ostree_commit_get_timestamp (commit);
      if (newest_candidate == NULL ||
          candidate_ts > newest_candidate_timestamp)
        {
          newest_candidate = candidate;
          newest_candidate_timestamp = candidate_ts;
        }
    }

  if (newest_candidate)
    {
      out_result->result = DELTA_SEARCH_RESULT_FROM;
      memcpy (out_result->from_revision, newest_candidate, OSTREE_SHA256_STRING_LEN+1);
    }
  return TRUE;
}

static void
fetch_delta_super_data_free (FetchDeltaSuperData *fetch_data)
{
  g_free (fetch_data->from_revision);
  g_free (fetch_data->to_revision);
  if (fetch_data->requested_ref)
    ostree_collection_ref_free (fetch_data->requested_ref);
  g_free (fetch_data);
}

static void
fetch_delta_index_data_free (FetchDeltaIndexData *fetch_data)
{
  g_free (fetch_data->from_revision);
  g_free (fetch_data->to_revision);
  if (fetch_data->requested_ref)
    ostree_collection_ref_free (fetch_data->requested_ref);
  g_free (fetch_data);
}

static void
set_required_deltas_error (GError **error,
                           const char *from_revision,
                           const char *to_revision)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Static deltas required, but none found for %s to %s",
               from_revision, to_revision);
}

static void
on_superblock_fetched (GObject   *src,
                       GAsyncResult *res,
                       gpointer      data)

{
  FetchDeltaSuperData *fetch_data = data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  g_autoptr(GBytes) delta_superblock_data = NULL;
  const char *from_revision = fetch_data->from_revision;
  const char *to_revision = fetch_data->to_revision;

  if (!_ostree_fetcher_request_to_membuf_finish ((OstreeFetcher*)src,
                                                 res,
                                                 &delta_superblock_data,
                                                 NULL, NULL, NULL,
                                                 error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        goto out;
      g_clear_error (&local_error);

      if (pull_data->require_static_deltas)
        {
          set_required_deltas_error (error, from_revision, to_revision);
          goto out;
        }

      queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0, fetch_data->requested_ref);
    }
  else
    {
      g_autoptr(GVariant) delta_superblock = NULL;
      g_autofree gchar *delta = g_strconcat (from_revision ?: "", from_revision ? "-" : "", to_revision, NULL);
      const guchar *expected_summary_digest = g_hash_table_lookup (pull_data->summary_deltas_checksums, delta);
      guint8 actual_summary_digest[OSTREE_SHA256_DIGEST_LEN];

      ot_checksum_bytes (delta_superblock_data, actual_summary_digest);

#ifndef OSTREE_DISABLE_GPGME
      /* At this point we've GPG verified the data, so in theory
       * could trust that they provided the right data, but let's
       * make this a hard error.
       */
      if (pull_data->gpg_verify_summary && !expected_summary_digest)
        {
          g_set_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE,
                       "GPG verification enabled, but no summary signatures found (use gpg-verify-summary=false in remote config to disable)");
          goto out;
        }
#endif /* OSTREE_DISABLE_GPGME */

      if (expected_summary_digest && memcmp (expected_summary_digest, actual_summary_digest, sizeof (actual_summary_digest)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid checksum for static delta %s", delta);
          goto out;
        }

      delta_superblock = g_variant_ref_sink (g_variant_new_from_bytes ((GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                                                                       delta_superblock_data, FALSE));

      g_ptr_array_add (pull_data->static_delta_superblocks, g_variant_ref (delta_superblock));
      if (!process_one_static_delta (pull_data, from_revision, to_revision, delta_superblock, fetch_data->requested_ref,
                                     pull_data->cancellable, error))
        goto out;
    }

 out:
  g_assert (pull_data->n_outstanding_metadata_fetches > 0);
  pull_data->n_outstanding_metadata_fetches--;

  if (local_error == NULL)
    pull_data->n_fetched_metadata++;

  if (_ostree_fetcher_should_retry_request (local_error, fetch_data->n_retries_remaining--))
    enqueue_one_static_delta_superblock_request_s (pull_data, g_steal_pointer (&fetch_data));
  else
    check_outstanding_requests_handle_error (pull_data, &local_error);

  g_clear_pointer (&fetch_data, fetch_delta_super_data_free);
}

static void
start_fetch_delta_superblock (OtPullData          *pull_data,
                              FetchDeltaSuperData *fetch_data)
{
  g_autofree char *delta_name =
    _ostree_get_relative_static_delta_superblock_path (fetch_data->from_revision,
                                                       fetch_data->to_revision);
  g_debug ("starting fetch of delta superblock %s", delta_name);
  _ostree_fetcher_request_to_membuf (pull_data->fetcher,
                                     pull_data->content_mirrorlist,
                                     delta_name, OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                     NULL, 0,
                                     OSTREE_MAX_METADATA_SIZE,
                                     0, pull_data->cancellable,
                                     on_superblock_fetched,
                                     g_steal_pointer (&fetch_data));
  pull_data->n_outstanding_metadata_fetches++;
  pull_data->n_requested_metadata++;
}

static void
enqueue_one_static_delta_superblock_request_s (OtPullData          *pull_data,
                                               FetchDeltaSuperData *fetch_data)
{
  if (fetcher_queue_is_full (pull_data))
    {
      g_debug ("queuing fetch of static delta superblock %s-%s",
               fetch_data->from_revision ?: "empty",
               fetch_data->to_revision);

      g_hash_table_add (pull_data->pending_fetch_delta_superblocks,
                        g_steal_pointer (&fetch_data));
    }
  else
    {
      start_fetch_delta_superblock (pull_data, g_steal_pointer (&fetch_data));
    }
}

/* Start a request for a static delta */
static void
enqueue_one_static_delta_superblock_request (OtPullData                *pull_data,
                                             const char                *from_revision,
                                             const char                *to_revision,
                                             const OstreeCollectionRef *ref)
{
  FetchDeltaSuperData *fdata = g_new0(FetchDeltaSuperData, 1);
  fdata->pull_data = pull_data;
  fdata->from_revision = g_strdup (from_revision);
  fdata->to_revision = g_strdup (to_revision);
  fdata->requested_ref = (ref != NULL) ? ostree_collection_ref_dup (ref) : NULL;
  fdata->n_retries_remaining = pull_data->n_network_retries;

  enqueue_one_static_delta_superblock_request_s (pull_data, g_steal_pointer (&fdata));
}

static gboolean
validate_variant_is_csum (GVariant       *csum,
                          GError        **error)
{
  if (!g_variant_is_of_type (csum, G_VARIANT_TYPE ("ay")))
    return glnx_throw (error, "Invalid checksum variant of type '%s', expected 'ay'",
                       g_variant_get_type_string (csum));

  return ostree_validate_structureof_csum_v (csum, error);
}

static gboolean
collect_available_deltas_for_pull (OtPullData *pull_data,
                                   GVariant   *deltas,
                                   GError    **error)
{
  gsize n;

  n = deltas ? g_variant_n_children (deltas) : 0;
  for (gsize i = 0; i < n; i++)
    {
      const char *delta;
      g_autoptr(GVariant) csum_v = NULL;
      g_autoptr(GVariant) ref = g_variant_get_child_value (deltas, i);

      g_variant_get_child (ref, 0, "&s", &delta);
      g_variant_get_child (ref, 1, "v", &csum_v);

      if (!validate_variant_is_csum (csum_v, error))
        return FALSE;

      guchar *csum_data = g_malloc (OSTREE_SHA256_DIGEST_LEN);
      memcpy (csum_data, ostree_checksum_bytes_peek (csum_v), 32);
      g_hash_table_insert (pull_data->summary_deltas_checksums,
                           g_strdup (delta),
                           csum_data);
    }

  return TRUE;
}

static void
on_delta_index_fetched (GObject   *src,
                        GAsyncResult *res,
                        gpointer      data)

{
  FetchDeltaIndexData *fetch_data = data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  g_autoptr(GBytes) delta_index_data = NULL;
  const char *from_revision = fetch_data->from_revision;
  const char *to_revision = fetch_data->to_revision;

  if (!_ostree_fetcher_request_to_membuf_finish ((OstreeFetcher*)src,
                                                 res,
                                                 &delta_index_data,
                                                 NULL, NULL, NULL,
                                                 error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        goto out;
      g_clear_error (&local_error);

      /* below call to initiate_delta_request() will fail finding the delta and fall back to commit */
    }
  else
    {
      g_autoptr(GVariant) delta_index = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE_VARDICT, delta_index_data, FALSE));
      g_autoptr(GVariant) deltas = g_variant_lookup_value (delta_index, OSTREE_SUMMARY_STATIC_DELTAS, G_VARIANT_TYPE ("a{sv}"));

      if (!collect_available_deltas_for_pull (pull_data, deltas, error))
        goto out;
    }

  if (!initiate_delta_request (pull_data,
                               fetch_data->requested_ref,
                               to_revision,
                               from_revision,
                               &local_error))
    goto out;

 out:
  g_assert (pull_data->n_outstanding_metadata_fetches > 0);
  pull_data->n_outstanding_metadata_fetches--;

  if (local_error == NULL)
    pull_data->n_fetched_metadata++;

  if (_ostree_fetcher_should_retry_request (local_error, fetch_data->n_retries_remaining--))
    enqueue_one_static_delta_index_request_s (pull_data, g_steal_pointer (&fetch_data));
  else
    check_outstanding_requests_handle_error (pull_data, &local_error);

  g_clear_pointer (&fetch_data, fetch_delta_index_data_free);
}

static void
start_fetch_delta_index (OtPullData          *pull_data,
                         FetchDeltaIndexData *fetch_data)
{
  g_autofree char *delta_name =
    _ostree_get_relative_static_delta_index_path (fetch_data->to_revision);
  g_debug ("starting fetch of delta index %s", delta_name);
  _ostree_fetcher_request_to_membuf (pull_data->fetcher,
                                     pull_data->content_mirrorlist,
                                     delta_name, OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                     NULL, 0,
                                     OSTREE_MAX_METADATA_SIZE,
                                     0, pull_data->cancellable,
                                     on_delta_index_fetched,
                                     g_steal_pointer (&fetch_data));
  pull_data->n_outstanding_metadata_fetches++;
  pull_data->n_requested_metadata++;
}

static void
enqueue_one_static_delta_index_request_s (OtPullData          *pull_data,
                                          FetchDeltaIndexData *fetch_data)
{
  if (fetcher_queue_is_full (pull_data))
    {
      g_debug ("queuing fetch of static delta index to %s",
               fetch_data->to_revision);

      g_hash_table_add (pull_data->pending_fetch_delta_indexes,
                        g_steal_pointer (&fetch_data));
    }
  else
    {
      start_fetch_delta_index (pull_data, g_steal_pointer (&fetch_data));
    }
}

/* Start a request for a static delta index */
static void
enqueue_one_static_delta_index_request (OtPullData                *pull_data,
                                        const char                *to_revision,
                                        const char                *from_revision,
                                        const OstreeCollectionRef *ref)
{
  FetchDeltaIndexData *fdata = g_new0(FetchDeltaIndexData, 1);
  fdata->pull_data = pull_data;
  fdata->from_revision = g_strdup (from_revision);
  fdata->to_revision = g_strdup (to_revision);
  fdata->requested_ref = (ref != NULL) ? ostree_collection_ref_dup (ref) : NULL;
  fdata->n_retries_remaining = pull_data->n_network_retries;

  enqueue_one_static_delta_index_request_s (pull_data, g_steal_pointer (&fdata));
}

static gboolean
_ostree_repo_verify_summary (OstreeRepo   *self,
                             const char   *name,
                             gboolean      gpg_verify_summary,
                             GPtrArray    *signapi_summary_verifiers,
                             GBytes       *summary,
                             GBytes       *signatures,
                             GCancellable *cancellable,
                             GError      **error)
{
  if (gpg_verify_summary)
    {
      if (summary == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "GPG verification enabled, but no summary found (check that the configured URL in remote config is correct)");
          return FALSE;
        }

      if (signatures == NULL)
        {
          g_set_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE,
                       "GPG verification enabled, but no summary signatures found (use gpg-verify-summary=false in remote config to disable)");
          return FALSE;
        }

      /* Verify any summary signatures. */
      if (summary != NULL && signatures != NULL)
        {
          g_autoptr(OstreeGpgVerifyResult) result = NULL;

          result = ostree_repo_verify_summary (self,
                                               name,
                                               summary,
                                               signatures,
                                               cancellable,
                                               error);
          if (!ostree_gpg_verify_result_require_valid_signature (result, error))
            return FALSE;
        }
    }

  if (signapi_summary_verifiers)
    {
      if (summary == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Signature verification enabled, but no summary found (check that the configured URL in remote config is correct)");
          return FALSE;
        }

      if (signatures == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Signature verification enabled, but no summary signatures found (use sign-verify-summary=false in remote config to disable)");
          return FALSE;
        }

      /* Verify any summary signatures. */
      if (summary != NULL && signatures != NULL)
        {
          g_autoptr(GVariant) sig_variant = NULL;

          sig_variant = g_variant_new_from_bytes (OSTREE_SUMMARY_SIG_GVARIANT_FORMAT,
                                                  signatures, FALSE);

          if (!_sign_verify_for_remote (signapi_summary_verifiers, summary, sig_variant, NULL, error))
            return FALSE;
        }
    }

  return TRUE;
}

static void
_ostree_repo_load_cache_summary_properties (OstreeRepo  *self,
                                            const char  *filename,
                                            const char  *extension,
                                            char       **out_etag,
                                            guint64     *out_last_modified)
{
  const char *file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", filename, extension);
  glnx_autofd int fd = -1;

  if (self->cache_dir_fd == -1)
    return;

  if (!glnx_openat_rdonly (self->cache_dir_fd, file, TRUE, &fd, NULL))
    return;

  if (out_etag != NULL)
    {
      g_autoptr(GBytes) etag_bytes = glnx_fgetxattr_bytes (fd, "user.etag", NULL);
      if (etag_bytes != NULL)
        {
          const guint8 *buf;
          gsize buf_len;

          buf = g_bytes_get_data (etag_bytes, &buf_len);

          /* Loosely validate against https://tools.ietf.org/html/rfc7232#section-2.3
           * by checking there are no embedded nuls. */
          for (gsize i = 0; i < buf_len; i++)
            {
              if (buf[i] == 0)
                {
                  buf_len = 0;
                  break;
                }
            }

          /* Nul-terminate and return */
          if (buf_len > 0)
            *out_etag = g_strndup ((const char *) buf, buf_len);
          else
            *out_etag = NULL;
        }
      else
        *out_etag = NULL;
    }

  if (out_last_modified != NULL)
    {
      struct stat statbuf;

      if (glnx_fstatat (fd, "", &statbuf, AT_EMPTY_PATH, NULL))
        *out_last_modified = statbuf.st_mtim.tv_sec;
      else
        *out_last_modified = 0;
    }
}

static gboolean
_ostree_repo_load_cache_summary_file (OstreeRepo        *self,
                                      const char        *filename,
                                      const char        *extension,
                                      GBytes           **out_data,
                                      GCancellable      *cancellable,
                                      GError           **error)
{
  const char *file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", filename, extension);
  glnx_autofd int fd = -1;
  g_autoptr(GBytes) data = NULL;

  *out_data = NULL;

  if (self->cache_dir_fd == -1)
    return TRUE;

  fd = openat (self->cache_dir_fd, file, O_CLOEXEC | O_RDONLY);
  if (fd < 0)
    {
      if (errno == ENOENT)
        return TRUE;
      return glnx_throw_errno_prefix (error, "openat(%s)", file);
    }

  data = ot_fd_readall_or_mmap (fd, 0, error);
  if (!data)
    return FALSE;

  *out_data =g_steal_pointer (&data);
  return TRUE;
}

/* Load the summary from the cache if the provided .sig file is the same as the
   cached version.  */
static gboolean
_ostree_repo_load_cache_summary_if_same_sig (OstreeRepo        *self,
                                             const char        *remote,
                                             GBytes            *summary_sig,
                                             GBytes           **out_summary,
                                             GCancellable      *cancellable,
                                             GError           **error)
{
  g_autoptr(GBytes) old_sig_contents = NULL;

  *out_summary = NULL;

  if (!_ostree_repo_load_cache_summary_file (self, remote, ".sig",
                                             &old_sig_contents,
                                             cancellable, error))
    return FALSE;

  if (old_sig_contents != NULL &&
      g_bytes_compare (old_sig_contents, summary_sig) == 0)
    {
      g_autoptr(GBytes) summary_data = NULL;

      if (!_ostree_repo_load_cache_summary_file (self, remote, NULL,
                                                 &summary_data,
                                                 cancellable, error))
        return FALSE;

      if (summary_data == NULL)
        {
          /* Cached signature without cached summary, remove the signature */
          const char *summary_cache_sig_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote, ".sig");
          (void) unlinkat (self->cache_dir_fd, summary_cache_sig_file, 0);
        }
      else
        *out_summary = g_steal_pointer (&summary_data);
    }

  return TRUE;
}

static void
store_file_cache_properties (int         dir_fd,
                             const char *filename,
                             const char *etag,
                             guint64     last_modified)
{
  glnx_autofd int fd = -1;
  struct timespec time_vals[] =
    {
      { .tv_sec = last_modified, .tv_nsec = UTIME_OMIT },  /* access, leave unchanged */
      { .tv_sec = last_modified, .tv_nsec = 0 },  /* modification */
    };

  if (!glnx_openat_rdonly (dir_fd, filename, TRUE, &fd, NULL))
    return;

  if (etag != NULL)
    TEMP_FAILURE_RETRY (fsetxattr (fd, "user.etag", etag, strlen (etag), 0));
  else
    TEMP_FAILURE_RETRY (fremovexattr (fd, "user.etag"));

  if (last_modified > 0)
    TEMP_FAILURE_RETRY (futimens (fd, time_vals));
}

static gboolean
_ostree_repo_save_cache_summary_file (OstreeRepo        *self,
                                      const char        *filename,
                                      const char        *extension,
                                      GBytes            *data,
                                      const char        *etag,
                                      guint64            last_modified,
                                      GCancellable      *cancellable,
                                      GError           **error)
{
  const char *file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", filename, extension);
  glnx_autofd int fd = -1;

  if (self->cache_dir_fd == -1)
    return TRUE;

  if (!glnx_shutil_mkdir_p_at (self->cache_dir_fd, _OSTREE_SUMMARY_CACHE_DIR, DEFAULT_DIRECTORY_MODE, cancellable, error))
    return FALSE;

  if (!glnx_file_replace_contents_at (self->cache_dir_fd,
                                      file,
                                      g_bytes_get_data (data, NULL),
                                      g_bytes_get_size (data),
                                      self->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    return FALSE;

  /* Store the caching properties. This is non-fatal on failure. */
  store_file_cache_properties (self->cache_dir_fd, file, etag, last_modified);

  return TRUE;
}

/* Replace the current summary+signature with new versions */
static gboolean
_ostree_repo_cache_summary (OstreeRepo        *self,
                            const char        *remote,
                            GBytes            *summary,
                            const char        *summary_etag,
                            guint64            summary_last_modified,
                            GBytes            *summary_sig,
                            const char        *summary_sig_etag,
                            guint64            summary_sig_last_modified,
                            GCancellable      *cancellable,
                            GError           **error)
{
  if (!_ostree_repo_save_cache_summary_file (self, remote, NULL,
                                             summary,
                                             summary_etag, summary_last_modified,
                                             cancellable, error))
    return FALSE;

  if (!_ostree_repo_save_cache_summary_file (self, remote, ".sig",
                                             summary_sig,
                                             summary_sig_etag, summary_sig_last_modified,
                                             cancellable, error))
    return FALSE;

  return TRUE;
}

static OstreeFetcher *
_ostree_repo_remote_new_fetcher (OstreeRepo  *self,
                                 const char  *remote_name,
                                 gboolean     gzip,
                                 GVariant    *extra_headers,
                                 const char  *append_user_agent,
                                 OstreeFetcherSecurityState *out_state,
                                 GError     **error)
{
  OstreeFetcher *fetcher = NULL;
  OstreeFetcherConfigFlags fetcher_flags = 0;
  gboolean tls_permissive = FALSE;
  OstreeFetcherSecurityState ret_state = OSTREE_FETCHER_SECURITY_STATE_TLS;
  gboolean success = FALSE;

  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (remote_name != NULL, NULL);

  if (!ostree_repo_get_remote_boolean_option (self, remote_name,
                                              "tls-permissive", FALSE,
                                              &tls_permissive, error))
    goto out;

  if (tls_permissive)
    {
      fetcher_flags |= OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE;
      ret_state = OSTREE_FETCHER_SECURITY_STATE_INSECURE;
    }

  if (gzip)
    fetcher_flags |= OSTREE_FETCHER_FLAGS_TRANSFER_GZIP;

  { gboolean http2_default = TRUE;
#ifndef BUILDOPT_HTTP2
    http2_default = FALSE;
#endif
    gboolean http2;

    if (!ostree_repo_get_remote_boolean_option (self, remote_name,
                                                "http2", http2_default,
                                                &http2, error))
      goto out;
    if (!http2)
      fetcher_flags |= OSTREE_FETCHER_FLAGS_DISABLE_HTTP2;
  }

  fetcher = _ostree_fetcher_new (self->tmp_dir_fd, remote_name, fetcher_flags);

  {
    g_autofree char *tls_client_cert_path = NULL;
    g_autofree char *tls_client_key_path = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-client-cert-path", NULL,
                                        &tls_client_cert_path, error))
      goto out;
    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-client-key-path", NULL,
                                        &tls_client_key_path, error))
      goto out;

    if ((tls_client_cert_path != NULL) != (tls_client_key_path != NULL))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Remote \"%s\" must specify both "
                     "\"tls-client-cert-path\" and \"tls-client-key-path\"",
                     remote_name);
        goto out;
      }
    else if (tls_client_cert_path != NULL)
      {
        _ostree_fetcher_set_client_cert (fetcher, tls_client_cert_path, tls_client_key_path);
      }
  }

  {
    g_autofree char *tls_ca_path = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-ca-path", NULL,
                                        &tls_ca_path, error))
      goto out;

    if (tls_ca_path != NULL)
      {
        _ostree_fetcher_set_tls_database (fetcher, tls_ca_path);

        /* Don't change if it's already _INSECURE */
        if (ret_state == OSTREE_FETCHER_SECURITY_STATE_TLS)
          ret_state = OSTREE_FETCHER_SECURITY_STATE_CA_PINNED;
      }
  }

  {
    g_autofree char *http_proxy = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "proxy", NULL,
                                        &http_proxy, error))
      goto out;

    if (http_proxy != NULL && http_proxy[0] != '\0')
      _ostree_fetcher_set_proxy (fetcher, http_proxy);
  }

  if (!_ostree_repo_remote_name_is_file (remote_name))
    {
      g_autofree char *cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
      /* TODO; port away from this; a bit hard since both libsoup and libcurl
       * expect a file. Doing ot_fdrel_to_gfile() works for now though.
       */
      GFile*repo_path = ostree_repo_get_path (self);
      g_autofree char *jar_path =
        g_build_filename (gs_file_get_path_cached (repo_path), cookie_file, NULL);

      if (g_file_test (jar_path, G_FILE_TEST_IS_REGULAR))
        _ostree_fetcher_set_cookie_jar (fetcher, jar_path);
    }

  if (extra_headers)
    _ostree_fetcher_set_extra_headers (fetcher, extra_headers);

  if (append_user_agent)
    _ostree_fetcher_set_extra_user_agent (fetcher, append_user_agent);

  success = TRUE;

out:
  if (!success)
    g_clear_object (&fetcher);
  if (out_state)
    *out_state = ret_state;

  return fetcher;
}

static gboolean
_ostree_preload_metadata_file (OstreeRepo    *self,
                               OstreeFetcher *fetcher,
                               GPtrArray     *mirrorlist,
                               const char    *filename,
                               gboolean      is_metalink,
                               const char   *if_none_match,
                               guint64       if_modified_since,
                               guint         n_network_retries,
                               GBytes        **out_bytes,
                               gboolean      *out_not_modified,
                               char          **out_etag,
                               guint64       *out_last_modified,
                               GCancellable  *cancellable,
                               GError        **error)
{
  if (is_metalink)
    {
      GError *local_error = NULL;

      /* the metalink uri is buried in the mirrorlist as the first (and only)
       * element */
      g_autoptr(OstreeMetalink) metalink =
        _ostree_metalink_new (fetcher, filename,
                              OSTREE_MAX_METADATA_SIZE,
                              mirrorlist->pdata[0], n_network_retries);

      _ostree_metalink_request_sync (metalink, NULL, out_bytes,
                                     cancellable, &local_error);

      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          *out_bytes = NULL;
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          return FALSE;
        }

      return TRUE;
    }
  else
    {
      return _ostree_fetcher_mirrored_request_to_membuf (fetcher, mirrorlist, filename,
                                                         OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                                         if_none_match, if_modified_since,
                                                         n_network_retries,
                                                         out_bytes, out_not_modified, out_etag, out_last_modified,
                                                         OSTREE_MAX_METADATA_SIZE,
                                                         cancellable, error);
    }
}

static gboolean
fetch_mirrorlist (OstreeFetcher  *fetcher,
                  const char     *mirrorlist_url,
                  guint           n_network_retries,
                  GPtrArray     **out_mirrorlist,
                  GCancellable   *cancellable,
                  GError        **error)
{
  g_autoptr(GPtrArray) ret_mirrorlist =
    g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);

  g_autoptr(OstreeFetcherURI) mirrorlist = _ostree_fetcher_uri_parse (mirrorlist_url, error);
  if (!mirrorlist)
    return FALSE;

  g_autofree char *contents = NULL;
  if (!fetch_uri_contents_utf8_sync (fetcher, mirrorlist, n_network_retries,
                                     &contents, cancellable, error))
    return glnx_prefix_error (error, "While fetching mirrorlist '%s'",
                              mirrorlist_url);

  /* go through each mirror in mirrorlist and do a quick sanity check that it
   * works so that we don't waste the fetcher's time when it goes through them
   * */
  g_auto(GStrv) lines = g_strsplit (contents, "\n", -1);
  g_debug ("Scanning mirrorlist from '%s'", mirrorlist_url);
  for (char **iter = lines; iter && *iter; iter++)
    {
      const char *mirror_uri_str = *iter;
      g_autoptr(OstreeFetcherURI) mirror_uri = NULL;
      g_autofree char *scheme = NULL;

      /* let's be nice and support empty lines and comments */
      if (*mirror_uri_str == '\0' || *mirror_uri_str == '#')
        continue;

      mirror_uri = _ostree_fetcher_uri_parse (mirror_uri_str, NULL);
      if (!mirror_uri)
        {
          g_debug ("Can't parse mirrorlist line '%s'", mirror_uri_str);
          continue;
        }

      scheme = _ostree_fetcher_uri_get_scheme (mirror_uri);
      if (!(g_str_equal (scheme, "http") || (g_str_equal (scheme, "https"))))
        {
          /* let's not support mirrorlists that contain non-http based URIs for
           * now (e.g. local URIs) -- we need to think about if and how we want
           * to support this since we set up things differently depending on
           * whether we're pulling locally or not */
          g_debug ("Ignoring non-http/s mirrorlist entry '%s'", mirror_uri_str);
          continue;
        }

      /* We keep sanity checking until we hit a working mirror; there's no need
       * to waste resources checking the remaining ones. At the same time,
       * guaranteeing that the first mirror in the list works saves the fetcher
       * time from always iterating through a few bad first mirrors. */
      if (ret_mirrorlist->len == 0)
        {
          GError *local_error = NULL;
          g_autoptr(OstreeFetcherURI) config_uri = _ostree_fetcher_uri_new_subpath (mirror_uri, "config");

          if (fetch_uri_contents_utf8_sync (fetcher, config_uri, n_network_retries,
                                            NULL, cancellable, &local_error))
            g_ptr_array_add (ret_mirrorlist, g_steal_pointer (&mirror_uri));
          else
            {
              g_debug ("Failed to fetch config from mirror '%s': %s",
                       mirror_uri_str, local_error->message);
              g_clear_error (&local_error);
            }
        }
      else
        {
          g_ptr_array_add (ret_mirrorlist, g_steal_pointer (&mirror_uri));
        }
    }

  if (ret_mirrorlist->len == 0)
    return glnx_throw (error, "No valid mirrors were found in mirrorlist '%s'",
                       mirrorlist_url);

  *out_mirrorlist = g_steal_pointer (&ret_mirrorlist);
  return TRUE;
}

static gboolean
compute_effective_mirrorlist (OstreeRepo    *self,
                              const char    *remote_name_or_baseurl,
                              const char    *url_override,
                              OstreeFetcher *fetcher,
                              guint          n_network_retries,
                              GPtrArray    **out_mirrorlist,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autofree char *baseurl = NULL;

  if (url_override != NULL)
    baseurl = g_strdup (url_override);
  else if (!ostree_repo_remote_get_url (self, remote_name_or_baseurl, &baseurl, error))
    return FALSE;

  if (g_str_has_prefix (baseurl, "mirrorlist="))
    {
      if (!fetch_mirrorlist (fetcher,
                             baseurl + strlen ("mirrorlist="),
                             n_network_retries,
                             out_mirrorlist,
                             cancellable, error))
        return FALSE;
    }
  else
    {
      g_autoptr(OstreeFetcherURI) baseuri = _ostree_fetcher_uri_parse (baseurl, error);

      if (!baseuri)
        return FALSE;

      if (!_ostree_fetcher_uri_validate (baseuri, error))
        return FALSE;

      *out_mirrorlist =
        g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
      g_ptr_array_add (*out_mirrorlist, g_steal_pointer (&baseuri));
    }
  return TRUE;
}

/* Create the fetcher by unioning options from the remote config, plus
 * any options specific to this pull (such as extra headers).
 */
static gboolean
reinitialize_fetcher (OtPullData *pull_data, const char *remote_name,
                      GError **error)
{
  g_clear_object (&pull_data->fetcher);
  pull_data->fetcher = _ostree_repo_remote_new_fetcher (pull_data->repo, remote_name, FALSE,
                                                        pull_data->extra_headers,
                                                        pull_data->append_user_agent,
                                                        &pull_data->fetcher_security_state,
                                                        error);
  if (pull_data->fetcher == NULL)
    return FALSE;

  return TRUE;
}

static gboolean
initiate_delta_request (OtPullData                *pull_data,
                        const OstreeCollectionRef *ref,
                        const char                *to_revision,
                        const char                 *delta_from_revision,
                        GError                    **error)
{
  DeltaSearchResult deltares;

  /* Look for a delta to @to_revision in the summary data */
  if (!get_best_static_delta_start_for (pull_data, to_revision, &deltares,
                                        pull_data->cancellable, error))
    return FALSE;

  switch (deltares.result)
    {
    case DELTA_SEARCH_RESULT_NO_MATCH:
      {
        if (pull_data->require_static_deltas) /* No deltas found; are they required? */
          {
            set_required_deltas_error (error, (ref != NULL) ? ref->ref_name : "", to_revision);
            return FALSE;
          }
        else /* No deltas, fall back to object fetches. */
          queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0, ref);
      }
      break;
    case DELTA_SEARCH_RESULT_FROM:
      enqueue_one_static_delta_superblock_request (pull_data, deltares.from_revision, to_revision, ref);
      break;
    case DELTA_SEARCH_RESULT_SCRATCH:
      {
        /* If a from-scratch delta is available, we don’t want to use it if
         * the ref already exists locally, since we are likely only a few
         * commits out of date; so doing an object pull is likely more
         * bandwidth efficient. */
        if (delta_from_revision != NULL)
          queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0, ref);
        else
          enqueue_one_static_delta_superblock_request (pull_data, NULL, to_revision, ref);
      }
      break;
    case DELTA_SEARCH_RESULT_UNCHANGED:
      {
        /* If we already have the commit, here things get a little special; we've historically
         * fetched detached metadata, so let's keep doing that.  But in the --require-static-deltas
         * path, we don't, under the assumption the user wants as little network traffic as
         * possible.
         */
        if (pull_data->require_static_deltas)
          break;
        else
          queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0, ref);
      }
    }

  return TRUE;
}

/*
 * initiate_request:
 * @ref: Optional ref name and collection ID
 * @to_revision: Target commit revision we want to fetch
 *
 * Start a request for either a ref or a commit.  In the
 * ref case, we know both the name and the target commit.
 *
 * This function primarily handles the semantics around
 * `disable_static_deltas` and `require_static_deltas`.
 */
static gboolean
initiate_request (OtPullData                 *pull_data,
                  const OstreeCollectionRef  *ref,
                  const char                 *to_revision,
                  GError                    **error)
{
  g_autofree char *delta_from_revision = NULL;

  /* Are deltas disabled?  OK, just start an object fetch and be done */
  if (pull_data->disable_static_deltas)
    {
      queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0, ref);
      return TRUE;
    }

  /* If doing a delta from a ref, look up the from-revision, since we need it
   * on most paths below. */
  if (ref != NULL)
    {
      g_autofree char *refspec = NULL;
      if (pull_data->remote_name != NULL)
        refspec = g_strdup_printf ("%s:%s", pull_data->remote_name, ref->ref_name);
      if (!ostree_repo_resolve_rev (pull_data->repo,
                                    refspec ?: ref->ref_name, TRUE,
                                    &delta_from_revision, error))
        return FALSE;
    }

  /* If we have a summary or delta index, we can use the newer logic.
   * We prefer the index as it might have more deltas than the summary
   * (i.e. leave some deltas out of summary to make it smaller). */
  if (pull_data->has_indexed_deltas)
    {
      enqueue_one_static_delta_index_request (pull_data, to_revision, delta_from_revision, ref);
    }
  else if (pull_data->summary_has_deltas)
    {
      if (!initiate_delta_request (pull_data, ref, to_revision, delta_from_revision, error))
        return FALSE;
    }
  else if (ref != NULL)
    {
      /* Are we doing a delta via a ref?  In that case we can fall back to the older
       * logic of just using the current tip of the ref as a delta FROM source. */

      /* Determine whether the from revision we have is partial; this
       * can happen if e.g. one uses `ostree pull --commit-metadata-only`.
       * This mirrors the logic in get_best_static_delta_start_for().
       */
      if (delta_from_revision)
        {
          OstreeRepoCommitState from_commitstate;

          if (!ostree_repo_load_commit (pull_data->repo, delta_from_revision, NULL,
                                        &from_commitstate, error))
            return FALSE;

          /* Was it partial?  Then we can't use it. */
          if (commitstate_is_partial (pull_data, from_commitstate))
            g_clear_pointer (&delta_from_revision, g_free);
        }

      /* If the current ref is the same, we don't do a delta request, just a
       * scan. Otherise, use the previous commit if available, or a scratch
       * delta.
       */
      if (delta_from_revision && g_str_equal (delta_from_revision, to_revision))
        queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0, ref);
      else
        enqueue_one_static_delta_superblock_request (pull_data, delta_from_revision ?: NULL, to_revision, ref);
    }
  else
    {
      /* Legacy path without a summary file - let's try a scratch delta, if that
       * doesn't work, it'll drop down to object requests.
       */
      enqueue_one_static_delta_superblock_request (pull_data, NULL, to_revision, NULL);
    }

  return TRUE;
}

static gboolean
all_requested_refs_have_commit (GHashTable *requested_refs /* (element-type OstreeCollectionRef utf8) */)
{
  GLNX_HASH_TABLE_FOREACH_KV (requested_refs, const OstreeCollectionRef*, ref,
                              const char*, override_commitid)
    {
      /* Note: "" override means whatever is latest */
      if (override_commitid == NULL || *override_commitid == 0)
        return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------------------------------
 * Below is the libsoup-invariant API; these should match
 * the stub functions in the #else clause
 * ------------------------------------------------------------------------------------------
 */

/**
 * ostree_repo_pull_with_options:
 * @self: Repo
 * @remote_name_or_baseurl: Name of remote or file:// url
 * @options: A GVariant a{sv} with an extensible set of flags.
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_repo_pull(), but supports an extensible set of flags.
 * The following are currently defined:
 *
 *   * `refs` (`as`): Array of string refs
 *   * `collection-refs` (`a(sss)`): Array of (collection ID, ref name, checksum) tuples to pull;
 *     mutually exclusive with `refs` and `override-commit-ids`. Checksums may be the empty
 *     string to pull the latest commit for that ref
 *   * `flags` (`i`): An instance of #OstreeRepoPullFlags
 *   * `subdir` (`s`): Pull just this subdirectory
 *   * `subdirs` (`as`): Pull just these subdirectories
 *   * `override-remote-name` (`s`): If local, add this remote to refspec
 *   * `gpg-verify` (`b`): GPG verify commits
 *   * `gpg-verify-summary` (`b`): GPG verify summary
 *   * `disable-sign-verify` (`b`): Disable signapi verification of commits
 *   * `disable-sign-verify-summary` (`b`): Disable signapi verification of the summary
 *   * `depth` (`i`): How far in the history to traverse; default is 0, -1 means infinite
 *   * `per-object-fsync` (`b`): Perform disk writes more slowly, avoiding a single large I/O sync
 *   * `disable-static-deltas` (`b`): Do not use static deltas
 *   * `require-static-deltas` (`b`): Require static deltas
 *   * `override-commit-ids` (`as`): Array of specific commit IDs to fetch for refs
 *   * `timestamp-check` (`b`): Verify commit timestamps are newer than current (when pulling via ref); Since: 2017.11
 *   * `timestamp-check-from-rev` (`s`): Verify that all fetched commit timestamps are newer than timestamp of given rev; Since: 2020.4
 *   * `metadata-size-restriction` (`t`): Restrict metadata objects to a maximum number of bytes; 0 to disable.  Since: 2018.9
 *   * `dry-run` (`b`): Only print information on what will be downloaded (requires static deltas)
 *   * `override-url` (`s`): Fetch objects from this URL if remote specifies no metalink in options
 *   * `inherit-transaction` (`b`): Don't initiate, finish or abort a transaction, useful to do multiple pulls in one transaction.
 *   * `http-headers` (`a(ss)`): Additional headers to add to all HTTP requests
 *   * `update-frequency` (`u`): Frequency to call the async progress callback in milliseconds, if any; only values higher than 0 are valid
 *   * `localcache-repos` (`as`): File paths for local repos to use as caches when doing remote fetches
 *   * `append-user-agent` (`s`): Additional string to append to the user agent
 *   * `n-network-retries` (`u`): Number of times to retry each download on receiving
 *     a transient network error, such as a socket timeout; default is 5, 0
 *     means return errors without retrying. Since: 2018.6
 *   * `ref-keyring-map` (`a(sss)`): Array of (collection ID, ref name, keyring
 *     remote name) tuples specifying which remote's keyring should be used when
 *     doing GPG verification of each collection-ref. This is useful to prevent a
 *     remote from serving malicious updates to refs which did not originate from
 *     it. This can be a subset or superset of the refs being pulled; any ref
 *     not being pulled will be ignored and any ref without a keyring remote
 *     will be verified with the keyring of the remote being pulled from.
 *     Since: 2019.2
 *   * `summary-bytes` (`ay'): Contents of the `summary` file to use. If this is
 *     specified, `summary-sig-bytes` must also be specified. This is
 *     useful if doing multiple pull operations in a transaction, using
 *     ostree_repo_remote_fetch_summary_with_options() beforehand to download
 *     the `summary` and `summary.sig` once for the entire transaction. If not
 *     specified, the `summary` will be downloaded from the remote. Since: 2020.5
 *   * `summary-sig-bytes` (`ay`): Contents of the `summary.sig` file. If this
 *     is specified, `summary-bytes` must also be specified. Since: 2020.5
 *   * `disable-verify-bindings` (`b`): Disable verification of commit bindings.
 *     Since: 2020.9
 */
gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name_or_baseurl,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  gboolean ret = FALSE;
  g_autoptr(GBytes) bytes_summary = NULL;
  gboolean summary_not_modified = FALSE;
  g_autofree char *summary_etag = NULL;
  guint64 summary_last_modified = 0;
  g_autofree char *metalink_url_str = NULL;
  g_autoptr(GHashTable) requested_refs_to_fetch = NULL;  /* (element-type OstreeCollectionRef utf8) */
  g_autoptr(GHashTable) commits_to_fetch = NULL;
  g_autofree char *remote_mode_str = NULL;
  g_autoptr(OstreeMetalink) metalink = NULL;
  OtPullData pull_data_real = { 0, };
  OtPullData *pull_data = &pull_data_real;
  GKeyFile *remote_config = NULL;
  char **configured_branches = NULL;
  guint64 bytes_transferred;
  guint64 end_time;
  guint update_frequency = 0;
  OstreeRepoPullFlags flags = 0;
  const char *dir_to_pull = NULL;
  g_autofree char **dirs_to_pull = NULL;
  g_autofree char **refs_to_fetch = NULL;
  g_autoptr(GVariantIter) collection_refs_iter = NULL;
  g_autofree char **override_commit_ids = NULL;
  g_autoptr(GSource) update_timeout = NULL;
  gboolean opt_per_object_fsync = FALSE;
  gboolean opt_gpg_verify_set = FALSE;
  gboolean opt_gpg_verify_summary_set = FALSE;
  gboolean opt_collection_refs_set = FALSE;
  gboolean opt_n_network_retries_set = FALSE;
  gboolean opt_ref_keyring_map_set = FALSE;
  gboolean disable_sign_verify = FALSE;
  gboolean disable_sign_verify_summary = FALSE;
  gboolean need_summary = FALSE;
  const char *main_collection_id = NULL;
  const char *url_override = NULL;
  gboolean inherit_transaction = FALSE;
  gboolean require_summary_for_mirror = FALSE;
  g_autoptr(GHashTable) updated_requested_refs_to_fetch = NULL;  /* (element-type OstreeCollectionRef utf8) */
  gsize i;
  g_autofree char **opt_localcache_repos = NULL;
  g_autoptr(GVariantIter) ref_keyring_map_iter = NULL;
  g_autoptr(GVariant) summary_bytes_v = NULL;
  g_autoptr(GVariant) summary_sig_bytes_v = NULL;
  /* If refs or collection-refs has exactly one value, this will point to that
   * value, otherwise NULL. Used for logging.
   */
  const char *the_ref_to_fetch = NULL;
  OstreeRepoTransactionStats tstats = { 0, };
  gboolean remote_mode_loaded = FALSE;

  /* Default */
  pull_data->max_metadata_size = OSTREE_MAX_METADATA_SIZE;

  if (options)
    {
      int flags_i = OSTREE_REPO_PULL_FLAGS_NONE;
      (void) g_variant_lookup (options, "refs", "^a&s", &refs_to_fetch);
      opt_collection_refs_set =
        g_variant_lookup (options, "collection-refs", "a(sss)", &collection_refs_iter);
      (void) g_variant_lookup (options, "flags", "i", &flags_i);
      /* Reduce risk of issues if enum happens to be 64 bit for some reason */
      flags = flags_i;
      (void) g_variant_lookup (options, "subdir", "&s", &dir_to_pull);
      (void) g_variant_lookup (options, "subdirs", "^a&s", &dirs_to_pull);
      (void) g_variant_lookup (options, "override-remote-name", "s", &pull_data->remote_refspec_name);
      opt_gpg_verify_set =
        g_variant_lookup (options, "gpg-verify", "b", &pull_data->gpg_verify);
      opt_gpg_verify_summary_set =
        g_variant_lookup (options, "gpg-verify-summary", "b", &pull_data->gpg_verify_summary);
      g_variant_lookup (options, "disable-sign-verify", "b", &disable_sign_verify);
      g_variant_lookup (options, "disable-sign-verify-summary", "b", &disable_sign_verify_summary);
      (void) g_variant_lookup (options, "depth", "i", &pull_data->maxdepth);
      (void) g_variant_lookup (options, "disable-static-deltas", "b", &pull_data->disable_static_deltas);
      (void) g_variant_lookup (options, "require-static-deltas", "b", &pull_data->require_static_deltas);
      (void) g_variant_lookup (options, "override-commit-ids", "^a&s", &override_commit_ids);
      (void) g_variant_lookup (options, "dry-run", "b", &pull_data->dry_run);
      (void) g_variant_lookup (options, "per-object-fsync", "b", &opt_per_object_fsync);
      (void) g_variant_lookup (options, "override-url", "&s", &url_override);
      (void) g_variant_lookup (options, "inherit-transaction", "b", &inherit_transaction);
      (void) g_variant_lookup (options, "http-headers", "@a(ss)", &pull_data->extra_headers);
      (void) g_variant_lookup (options, "update-frequency", "u", &update_frequency);
      (void) g_variant_lookup (options, "localcache-repos", "^a&s", &opt_localcache_repos);
      (void) g_variant_lookup (options, "timestamp-check", "b", &pull_data->timestamp_check);
      (void) g_variant_lookup (options, "timestamp-check-from-rev", "s", &pull_data->timestamp_check_from_rev);
      (void) g_variant_lookup (options, "max-metadata-size", "t", &pull_data->max_metadata_size);
      (void) g_variant_lookup (options, "append-user-agent", "s", &pull_data->append_user_agent);
      opt_n_network_retries_set =
        g_variant_lookup (options, "n-network-retries", "u", &pull_data->n_network_retries);
      opt_ref_keyring_map_set =
	g_variant_lookup (options, "ref-keyring-map", "a(sss)", &ref_keyring_map_iter);
      (void) g_variant_lookup (options, "summary-bytes", "@ay", &summary_bytes_v);
      (void) g_variant_lookup (options, "summary-sig-bytes", "@ay", &summary_sig_bytes_v);
      (void) g_variant_lookup (options, "disable-verify-bindings", "b", &pull_data->disable_verify_bindings);

      if (pull_data->remote_refspec_name != NULL)
        pull_data->remote_name = g_strdup (pull_data->remote_refspec_name);
    }

#ifdef OSTREE_DISABLE_GPGME
  /* Explicitly fail here if gpg verification is requested and we have no GPG support */
  if (pull_data->gpg_verify || pull_data->gpg_verify_summary)
  {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
              "'%s': GPG feature is disabled at build time",
              __FUNCTION__);
      goto out;
  }
#endif

  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (pull_data->maxdepth >= -1, FALSE);
  g_return_val_if_fail (!pull_data->timestamp_check || pull_data->maxdepth == 0, FALSE);
  g_return_val_if_fail (!opt_collection_refs_set ||
                        (refs_to_fetch == NULL && override_commit_ids == NULL), FALSE);
  if (refs_to_fetch && override_commit_ids)
    g_return_val_if_fail (g_strv_length (refs_to_fetch) == g_strv_length (override_commit_ids), FALSE);

  if (dir_to_pull)
    g_return_val_if_fail (dir_to_pull[0] == '/', FALSE);

  for (i = 0; dirs_to_pull != NULL && dirs_to_pull[i] != NULL; i++)
    g_return_val_if_fail (dirs_to_pull[i][0] == '/', FALSE);

  g_return_val_if_fail (!(pull_data->disable_static_deltas && pull_data->require_static_deltas), FALSE);

  /* We only do dry runs with static deltas, because we don't really have any
   * in-advance information for bare fetches.
   */
  g_return_val_if_fail (!pull_data->dry_run || pull_data->require_static_deltas, FALSE);

  /* summary-bytes and summary-sig-bytes must both be specified, or neither be
   * specified, so we know they’re consistent */
  g_return_val_if_fail ((summary_bytes_v == NULL) == (summary_sig_bytes_v == NULL), FALSE);

  pull_data->is_mirror = (flags & OSTREE_REPO_PULL_FLAGS_MIRROR) > 0;
  pull_data->is_commit_only = (flags & OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY) > 0;
  /* See our processing of OSTREE_REPO_PULL_FLAGS_UNTRUSTED below */
  if ((flags & OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES) > 0)
    pull_data->importflags |= _OSTREE_REPO_IMPORT_FLAGS_VERIFY_BAREUSERONLY;
  pull_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  if (error)
    pull_data->async_error = &pull_data->cached_async_error;
  else
    pull_data->async_error = NULL;

  /* Note we're using the thread default (or global) context here, so it may outlive the
   * OtPullData object if there's another ref on it. Thus, always detach/destroy sources
   * local to the `ostree_repo_pull*` operation rather than trying to transfer ownership. */
  pull_data->main_context = g_main_context_ref_thread_default ();
  pull_data->flags = flags;

  /* TODO: Avoid mutating the repo object */
  if (opt_per_object_fsync)
    self->per_object_fsync = TRUE;

  if (!opt_n_network_retries_set)
    pull_data->n_network_retries = DEFAULT_N_NETWORK_RETRIES;

  pull_data->repo = self;
  pull_data->progress = progress;

  pull_data->expected_commit_sizes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            (GDestroyNotify)g_free,
                                                            (GDestroyNotify)g_free);
  pull_data->commit_to_depth = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free,
                                                      NULL);
  pull_data->summary_deltas_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                               (GDestroyNotify)g_free,
                                                               (GDestroyNotify)g_free);
  pull_data->ref_original_commits = g_hash_table_new_full (ostree_collection_ref_hash, ostree_collection_ref_equal,
                                                           (GDestroyNotify)NULL,
                                                           (GDestroyNotify)g_free);
  pull_data->verified_commits = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                       (GDestroyNotify)g_free, NULL);
  pull_data->signapi_verified_commits = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                               (GDestroyNotify)g_free, NULL);
  pull_data->ref_keyring_map = g_hash_table_new_full (ostree_collection_ref_hash, ostree_collection_ref_equal,
                                                      (GDestroyNotify)ostree_collection_ref_free, (GDestroyNotify)g_free);
  pull_data->scanned_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                       (GDestroyNotify)g_variant_unref, NULL);
  pull_data->fetched_detached_metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                (GDestroyNotify)g_free, (GDestroyNotify)variant_or_null_unref);
  pull_data->requested_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        (GDestroyNotify)g_free, NULL);
  pull_data->requested_fallback_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                 (GDestroyNotify)g_free, NULL);
  pull_data->requested_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                         (GDestroyNotify)g_variant_unref, NULL);
  pull_data->pending_fetch_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            (GDestroyNotify)g_free,
                                                            (GDestroyNotify)fetch_object_data_free);
  pull_data->pending_fetch_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                             (GDestroyNotify)g_variant_unref,
                                                             (GDestroyNotify)fetch_object_data_free);
  pull_data->pending_fetch_delta_indexes = g_hash_table_new_full (NULL, NULL, (GDestroyNotify) fetch_delta_index_data_free, NULL);
  pull_data->pending_fetch_delta_superblocks = g_hash_table_new_full (NULL, NULL, (GDestroyNotify) fetch_delta_super_data_free, NULL);
  pull_data->pending_fetch_deltaparts = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)fetch_static_delta_data_free, NULL);

  if (opt_localcache_repos && *opt_localcache_repos)
    {
      pull_data->localcache_repos = g_ptr_array_new_with_free_func (g_object_unref);
      for (char **it = opt_localcache_repos; it && *it; it++)
        {
          const char *localcache_path = *it;
          g_autoptr(GFile) localcache_file = g_file_new_for_path (localcache_path);
          g_autoptr(OstreeRepo) cacherepo = ostree_repo_new (localcache_file);
          if (!ostree_repo_open (cacherepo, cancellable, error))
            goto out;
          g_ptr_array_add (pull_data->localcache_repos, g_steal_pointer (&cacherepo));
        }
    }

  if (dir_to_pull != NULL || dirs_to_pull != NULL)
    {
      pull_data->dirs = g_ptr_array_new_with_free_func (g_free);
      if (dir_to_pull != NULL)
        g_ptr_array_add (pull_data->dirs, g_strdup (dir_to_pull));

      if (dirs_to_pull != NULL)
        {
          for (i = 0; dirs_to_pull[i] != NULL; i++)
            g_ptr_array_add (pull_data->dirs, g_strdup (dirs_to_pull[i]));
        }
    }

  g_queue_init (&pull_data->scan_object_queue);

  pull_data->start_time = g_get_monotonic_time ();

  if (_ostree_repo_remote_name_is_file (remote_name_or_baseurl))
    {
      /* For compatibility with pull-local, don't gpg verify local
       * pulls by default.
       */
      if ((pull_data->gpg_verify ||
           pull_data->gpg_verify_summary
          ) &&
          pull_data->remote_name == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Must specify remote name to enable gpg verification");
          goto out;
        }
    }
  else
    {
      g_autofree char *unconfigured_state = NULL;
      g_autofree char *custom_backend = NULL;

      g_free (pull_data->remote_name);
      pull_data->remote_name = g_strdup (remote_name_or_baseurl);

      /* Fetch GPG verification settings from remote if it wasn't already
       * explicitly set in the options. */
      if (!opt_gpg_verify_set)
        if (!ostree_repo_remote_get_gpg_verify (self, pull_data->remote_name,
                                                &pull_data->gpg_verify, error))
          goto out;

      if (!opt_gpg_verify_summary_set)
        if (!ostree_repo_remote_get_gpg_verify_summary (self, pull_data->remote_name,
                                                        &pull_data->gpg_verify_summary, error))
          goto out;

      /* NOTE: If changing this, see the matching implementation in
       * ostree-sysroot-upgrader.c
       */
      if (!ostree_repo_get_remote_option (self, pull_data->remote_name,
                                          "unconfigured-state", NULL,
                                          &unconfigured_state,
                                          error))
        goto out;

      if (unconfigured_state)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "remote unconfigured-state: %s", unconfigured_state);
          goto out;
        }

      if (!ostree_repo_get_remote_option (self, pull_data->remote_name,
                                          "custom-backend", NULL,
                                          &custom_backend,
                                          error))
        goto out;

      if (custom_backend)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Cannot fetch via libostree - remote '%s' uses custom backend '%s'", 
                       pull_data->remote_name, custom_backend);
          goto out;
        }
    }

  if (pull_data->remote_name && !(disable_sign_verify && disable_sign_verify_summary))
    {
      if (!_signapi_init_for_remote (pull_data->repo, pull_data->remote_name,
                                     &pull_data->signapi_commit_verifiers,
                                     &pull_data->signapi_summary_verifiers,
                                     error))
        goto out;
    }

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_REFS;

  if (!reinitialize_fetcher (pull_data, remote_name_or_baseurl, error))
    goto out;

  pull_data->tmpdir_dfd = pull_data->repo->tmp_dir_fd;
  requested_refs_to_fetch = g_hash_table_new_full (ostree_collection_ref_hash,
                                                   ostree_collection_ref_equal,
                                                   (GDestroyNotify) ostree_collection_ref_free,
                                                   g_free);
  commits_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!ostree_repo_get_remote_option (self,
                                      remote_name_or_baseurl, "metalink",
                                      NULL, &metalink_url_str, error))
    goto out;

  if (!metalink_url_str)
    {
      if (!compute_effective_mirrorlist (self, remote_name_or_baseurl,
                                         url_override,
                                         pull_data->fetcher,
                                         pull_data->n_network_retries,
                                         &pull_data->meta_mirrorlist,
                                         cancellable, error))
        goto out;
    }
  else
    {
      g_autoptr(GBytes) summary_bytes = NULL;
      g_autoptr(OstreeFetcherURI) metalink_uri = _ostree_fetcher_uri_parse (metalink_url_str, error);
      g_autoptr(OstreeFetcherURI) target_uri = NULL;

      if (!metalink_uri)
        goto out;

      /* FIXME: Use summary_bytes_v/summary_sig_bytes_v to avoid unnecessary
       * re-downloads here. Would require additional support for caching the
       * metalink file or mirror list. */

      metalink = _ostree_metalink_new (pull_data->fetcher, "summary",
                                       OSTREE_MAX_METADATA_SIZE, metalink_uri,
                                       pull_data->n_network_retries);

      if (! _ostree_metalink_request_sync (metalink,
                                           &target_uri,
                                           &summary_bytes,
                                           cancellable,
                                           error))
        goto out;

      /* XXX: would be interesting to implement metalink as another source of
       * mirrors here since we use it as such anyway (rather than the "usual"
       * use case of metalink, which is only for a single target filename) */
      {
        g_autofree char *path = _ostree_fetcher_uri_get_path (target_uri);
        g_autofree char *basepath = g_path_get_dirname (path);
        g_autoptr(OstreeFetcherURI) new_target_uri = _ostree_fetcher_uri_new_path (target_uri, basepath);
        pull_data->meta_mirrorlist =
          g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
        g_ptr_array_add (pull_data->meta_mirrorlist, g_steal_pointer (&new_target_uri));
      }

      pull_data->summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                     summary_bytes, FALSE);
    }

  {
    g_autofree char *contenturl = NULL;

    if (metalink_url_str == NULL && url_override != NULL)
      contenturl = g_strdup (url_override);
    else if (!ostree_repo_get_remote_option (self, remote_name_or_baseurl,
                                             "contenturl", NULL,
                                             &contenturl, error))
      goto out;

    if (contenturl == NULL)
      {
        pull_data->content_mirrorlist =
          g_ptr_array_ref (pull_data->meta_mirrorlist);
      }
    else
      {
        if (!compute_effective_mirrorlist (self, remote_name_or_baseurl,
                                           contenturl,
                                           pull_data->fetcher,
                                           pull_data->n_network_retries,
                                           &pull_data->content_mirrorlist,
                                           cancellable, error))
          goto out;
      }
  }

  /* FIXME: Do we want an analogue of this which supports collection IDs? */
  if (!ostree_repo_get_remote_list_option (self,
                                           remote_name_or_baseurl, "branches",
                                           &configured_branches, error))
    goto out;

  /* Handle file:// URIs */
  {
    OstreeFetcherURI *first_uri = pull_data->meta_mirrorlist->pdata[0];
    g_autofree char *first_scheme = _ostree_fetcher_uri_get_scheme (first_uri);

    /* NB: we don't support local mirrors in mirrorlists, so if this passes, it
     * means that we're not using mirrorlists (see also fetch_mirrorlist())
     * Also, we explicitly disable the "local repo" path if static deltas
     * were explicitly requested to be required; this is going to happen
     * most often for testing deltas without setting up a HTTP server.
     */
    if (g_str_equal (first_scheme, "file") && !pull_data->require_static_deltas)
      {
        g_autofree char *uri = _ostree_fetcher_uri_to_string (first_uri);
        g_autoptr(GFile) remote_repo_path = g_file_new_for_uri (uri);
        pull_data->remote_repo_local = ostree_repo_new (remote_repo_path);
        if (!ostree_repo_open (pull_data->remote_repo_local, cancellable, error))
          goto out;
      }
  }

  /* Change some option defaults if we're actually pulling from a local
   * (filesystem accessible) repo.
   */
  if (pull_data->remote_repo_local)
    {
      /* For local pulls, default to disabling static deltas so that the
       * exact object files are copied.
       */
      if (!pull_data->require_static_deltas)
        pull_data->disable_static_deltas = TRUE;

      /* Note the inversion here; PULL_FLAGS_UNTRUSTED is converted to
       * IMPORT_FLAGS_TRUSTED only if it's unset (and just for local repos).
       */
      if ((flags & OSTREE_REPO_PULL_FLAGS_UNTRUSTED) == 0)
        pull_data->importflags |= _OSTREE_REPO_IMPORT_FLAGS_TRUSTED;

      /* Shouldn't be referenced in this path, but just in case.  See below
       * for more information.
       */
      pull_data->trusted_http_direct = FALSE;
    }
  else
    {
      /* For non-local repos, we require the TRUSTED_HTTP pull flag to map to
       * the TRUSTED object import flag. In practice we don't do object imports
       * for HTTP, but it's easiest to use one set of flags between HTTP and
       * local imports.
       */
      if (flags & OSTREE_REPO_PULL_FLAGS_TRUSTED_HTTP)
        pull_data->importflags |= _OSTREE_REPO_IMPORT_FLAGS_TRUSTED;

      const gboolean verifying_bareuseronly =
        (pull_data->importflags & _OSTREE_REPO_IMPORT_FLAGS_VERIFY_BAREUSERONLY) > 0;
      /* If we're mirroring and writing into an archive repo, and both checksum and
       * bareuseronly are turned off, we can directly copy the content rather than
       * paying the cost of exploding it, checksumming, and re-gzip.
       */
      const gboolean mirroring_into_archive =
        pull_data->is_mirror && pull_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE;
      const gboolean import_trusted = !verifying_bareuseronly &&
        (pull_data->importflags & _OSTREE_REPO_IMPORT_FLAGS_TRUSTED) > 0;
      pull_data->trusted_http_direct = mirroring_into_archive && import_trusted;
    }

  /* We can't use static deltas if pulling into an archive repo. */
  if (self->mode == OSTREE_REPO_MODE_ARCHIVE)
    {
      if (pull_data->require_static_deltas)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Can't use static deltas in an archive repo");
          goto out;
        }
      pull_data->disable_static_deltas = TRUE;
    }

  /* It's not efficient to use static deltas if all we want is the commit
   * metadata. */
  if (pull_data->is_commit_only)
    pull_data->disable_static_deltas = TRUE;

  /* Compute the set of collection-refs (and optional commit id) to fetch */

  if (pull_data->is_mirror && !refs_to_fetch && !opt_collection_refs_set && !configured_branches)
    {
      require_summary_for_mirror = TRUE;
    }
  else if (opt_collection_refs_set)
    {
      const gchar *collection_id, *ref_name, *checksum;

      while (g_variant_iter_loop (collection_refs_iter, "(&s&s&s)", &collection_id, &ref_name, &checksum))
        {
          if (!ostree_validate_rev (ref_name, error))
            goto out;
          g_hash_table_insert (requested_refs_to_fetch,
                               ostree_collection_ref_new (collection_id, ref_name),
                               (*checksum != '\0') ? g_strdup (checksum) : NULL);
        }
    }
  else if (refs_to_fetch != NULL)
    {
      char **strviter = refs_to_fetch;
      char **commitid_strviter = override_commit_ids ?: NULL;

      while (*strviter)
        {
          const char *branch = *strviter;

          if (ostree_validate_checksum_string (branch, NULL))
            {
              char *key = g_strdup (branch);
              g_hash_table_add (commits_to_fetch, key);
            }
          else
            {
              if (!ostree_validate_rev (branch, error))
                goto out;
              char *commitid = commitid_strviter ? g_strdup (*commitid_strviter) : NULL;
              g_hash_table_insert (requested_refs_to_fetch,
                                   ostree_collection_ref_new (NULL, branch), commitid);
            }

          strviter++;
          if (commitid_strviter)
            commitid_strviter++;
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

          g_hash_table_insert (requested_refs_to_fetch,
                               ostree_collection_ref_new (NULL, branch), NULL);
        }
    }

  /* Deltas are necessary when mirroring or resolving a requested ref to a commit.
   * We try to avoid loading the potentially large summary if it is not needed. */
  need_summary = require_summary_for_mirror || !all_requested_refs_have_commit (requested_refs_to_fetch) || summary_sig_bytes_v != NULL;

  /* If we don't have indexed deltas, we need the summary for deltas, so check
   * the config file for support.
   * NOTE: Avoid download if we don't need deltas */
  if (!need_summary && !pull_data->disable_static_deltas)
    {
      if (!load_remote_repo_config (pull_data, &remote_config, cancellable, error))
        goto out;

      /* Check if remote has delta indexes outside summary */
      if (!ot_keyfile_get_boolean_with_default (remote_config, "core", "indexed-deltas", FALSE,
                                                &pull_data->has_indexed_deltas, error))
        goto out;

      if (!pull_data->has_indexed_deltas)
        need_summary = TRUE;
   }

  pull_data->static_delta_superblocks = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  if (need_summary)
    {
      g_autoptr(GBytes) bytes_sig = NULL;
      gboolean summary_sig_not_modified = FALSE;
      g_autofree char *summary_sig_etag = NULL;
      guint64 summary_sig_last_modified = 0;
      gsize n;
      g_autoptr(GVariant) refs = NULL;
      g_autoptr(GVariant) deltas = NULL;
      g_autoptr(GVariant) additional_metadata = NULL;
      gboolean summary_from_cache = FALSE;
      gboolean tombstone_commits = FALSE;

      if (summary_sig_bytes_v)
        {
          /* Must both be specified */
          g_assert (summary_bytes_v);

          bytes_sig = g_variant_get_data_as_bytes (summary_sig_bytes_v);
          bytes_summary = g_variant_get_data_as_bytes (summary_bytes_v);

          if (!bytes_sig || !bytes_summary)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "summary-bytes or summary-sig-bytes set to invalid value");
              goto out;
            }

          g_debug ("Loaded %s summary from options", remote_name_or_baseurl);
        }

      if (!bytes_sig)
        {
          g_autofree char *summary_sig_if_none_match = NULL;
          guint64 summary_sig_if_modified_since = 0;

          /* Load the summary.sig from the network, but send its ETag and
           * Last-Modified from the on-disk cache (if it exists) to reduce the
           * download size if nothing’s changed. */
          _ostree_repo_load_cache_summary_properties (self, remote_name_or_baseurl, ".sig",
                                                      &summary_sig_if_none_match, &summary_sig_if_modified_since);

          g_clear_pointer (&summary_sig_etag, g_free);
          summary_sig_last_modified = 0;
          if (!_ostree_fetcher_mirrored_request_to_membuf (pull_data->fetcher,
                                                           pull_data->meta_mirrorlist,
                                                           "summary.sig", OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                                           summary_sig_if_none_match, summary_sig_if_modified_since,
                                                           pull_data->n_network_retries,
                                                           &bytes_sig,
                                                           &summary_sig_not_modified, &summary_sig_etag, &summary_sig_last_modified,
                                                           OSTREE_MAX_METADATA_SIZE,
                                                           cancellable, error))
            goto out;

          /* The server returned HTTP status 304 Not Modified, so we’re clear to
           * load summary.sig from the cache. Also load summary, since
           * `_ostree_repo_load_cache_summary_if_same_sig()` would just do that anyway. */
          if (summary_sig_not_modified)
            {
              g_clear_pointer (&bytes_sig, g_bytes_unref);
              g_clear_pointer (&bytes_summary, g_bytes_unref);
              if (!_ostree_repo_load_cache_summary_file (self, remote_name_or_baseurl, ".sig",
                                                         &bytes_sig,
                                                         cancellable, error))
                goto out;

              if (!bytes_summary &&
                  !pull_data->remote_repo_local &&
                  !_ostree_repo_load_cache_summary_file (self, remote_name_or_baseurl, NULL,
                                                         &bytes_summary,
                                                         cancellable, error))
                goto out;
            }
        }

      if (bytes_sig &&
          !bytes_summary &&
          !pull_data->remote_repo_local &&
          !_ostree_repo_load_cache_summary_if_same_sig (self,
                                                        remote_name_or_baseurl,
                                                        bytes_sig,
                                                        &bytes_summary,
                                                        cancellable,
                                                        error))
        goto out;

      if (bytes_summary && !summary_bytes_v)
        {
          g_debug ("Loaded %s summary from cache", remote_name_or_baseurl);
          summary_from_cache = TRUE;
        }

      if (!pull_data->summary && !bytes_summary)
        {
          g_autofree char *summary_if_none_match = NULL;
          guint64 summary_if_modified_since = 0;

          _ostree_repo_load_cache_summary_properties (self, remote_name_or_baseurl, NULL,
                                                      &summary_if_none_match, &summary_if_modified_since);

          g_clear_pointer (&summary_etag, g_free);
          summary_last_modified = 0;

          if (!_ostree_fetcher_mirrored_request_to_membuf (pull_data->fetcher,
                                                           pull_data->meta_mirrorlist,
                                                           "summary", OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                                           summary_if_none_match, summary_if_modified_since,
                                                           pull_data->n_network_retries,
                                                           &bytes_summary,
                                                           &summary_not_modified, &summary_etag, &summary_last_modified,
                                                           OSTREE_MAX_METADATA_SIZE,
                                                           cancellable, error))
            goto out;

          /* The server returned HTTP status 304 Not Modified, so we’re clear to
           * load summary from the cache. */
          if (summary_not_modified)
            {
              g_clear_pointer (&bytes_summary, g_bytes_unref);
              if (!_ostree_repo_load_cache_summary_file (self, remote_name_or_baseurl, NULL,
                                                         &bytes_summary,
                                                         cancellable, error))
                goto out;
            }
        }

#ifndef OSTREE_DISABLE_GPGME
      if (!bytes_summary && pull_data->gpg_verify_summary)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "GPG verification enabled, but no summary found (use gpg-verify-summary=false in remote config to disable)");
          goto out;
        }
#endif /* OSTREE_DISABLE_GPGME */

      if (!bytes_summary && require_summary_for_mirror)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Fetching all refs was requested in mirror mode, but remote repository does not have a summary");
          goto out;
        }

#ifndef OSTREE_DISABLE_GPGME
      if (!bytes_sig && pull_data->gpg_verify_summary)
        {
          g_set_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE,
                       "GPG verification enabled, but no summary.sig found (use gpg-verify-summary=false in remote config to disable)");
          goto out;
        }

      if (pull_data->gpg_verify_summary && bytes_summary && bytes_sig)
        {
          g_autoptr(OstreeGpgVerifyResult) result = NULL;
          g_autoptr(GError) temp_error = NULL;

          result = ostree_repo_verify_summary (self, pull_data->remote_name,
                                               bytes_summary, bytes_sig,
                                               cancellable, &temp_error);
          if (!ostree_gpg_verify_result_require_valid_signature (result, &temp_error))
            {
              if (summary_from_cache)
                {
                  /* The cached summary doesn't match, fetch a new one and verify again.
                   * Don’t set the cache headers in the HTTP request, to force a
                   * full download. */
                  if ((self->test_error_flags & OSTREE_REPO_TEST_ERROR_INVALID_CACHE) > 0)
                    {
                      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Remote %s cached summary invalid and "
                                   "OSTREE_REPO_TEST_ERROR_INVALID_CACHE specified",
                                   pull_data->remote_name);
                      goto out;
                    }
                  else
                    g_debug ("Remote %s cached summary invalid, pulling new version",
                             pull_data->remote_name);

                  summary_from_cache = FALSE;
                  g_clear_pointer (&bytes_summary, g_bytes_unref);
                  g_clear_pointer (&summary_etag, g_free);
                  summary_last_modified = 0;
                  if (!_ostree_fetcher_mirrored_request_to_membuf (pull_data->fetcher,
                                                                   pull_data->meta_mirrorlist,
                                                                   "summary",
                                                                   OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                                                   NULL, 0,  /* no cache headers */
                                                                   pull_data->n_network_retries,
                                                                   &bytes_summary,
                                                                   &summary_not_modified, &summary_etag, &summary_last_modified,
                                                                   OSTREE_MAX_METADATA_SIZE,
                                                                   cancellable, error))
                    goto out;

                  g_autoptr(OstreeGpgVerifyResult) retry =
                    ostree_repo_verify_summary (self, pull_data->remote_name,
                                                bytes_summary, bytes_sig,
                                                cancellable, error);
                  if (!ostree_gpg_verify_result_require_valid_signature (retry, error))
                    goto out;
                }
              else
                {
                  g_propagate_error (error, g_steal_pointer (&temp_error));
                  goto out;
                }
            }
        }
#endif /* OSTREE_DISABLE_GPGME */

      if (pull_data->signapi_summary_verifiers)
        {
          if (!bytes_sig && pull_data->signapi_summary_verifiers)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Signatures verification enabled, but no summary.sig found (use sign-verify-summary=false in remote config to disable)");
              goto out;
            }
          if (bytes_summary && bytes_sig)
            {
              g_autoptr(GVariant) signatures = NULL;
              g_autoptr(GError) temp_error = NULL;

              signatures = g_variant_new_from_bytes (OSTREE_SUMMARY_SIG_GVARIANT_FORMAT,
                                                     bytes_sig, FALSE);

              g_assert (pull_data->signapi_summary_verifiers);
              if (!_sign_verify_for_remote (pull_data->signapi_summary_verifiers, bytes_summary, signatures, NULL, &temp_error))
                {
                  if (summary_from_cache)
                    {
                      /* The cached summary doesn't match, fetch a new one and verify again.
                       * Don’t set the cache headers in the HTTP request, to force a
                       * full download. */
                      if ((self->test_error_flags & OSTREE_REPO_TEST_ERROR_INVALID_CACHE) > 0)
                        {
                          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "Remote %s cached summary invalid and "
                                       "OSTREE_REPO_TEST_ERROR_INVALID_CACHE specified",
                                       pull_data->remote_name);
                          goto out;
                        }
                      else
                        g_debug ("Remote %s cached summary invalid, pulling new version",
                                 pull_data->remote_name);

                      summary_from_cache = FALSE;
                      g_clear_pointer (&bytes_summary, g_bytes_unref);
                      g_clear_pointer (&summary_etag, g_free);
                      summary_last_modified = 0;
                      if (!_ostree_fetcher_mirrored_request_to_membuf (pull_data->fetcher,
                                                                       pull_data->meta_mirrorlist,
                                                                       "summary",
                                                                       OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                                                       NULL, 0,  /* no cache headers */
                                                                       pull_data->n_network_retries,
                                                                       &bytes_summary,
                                                                       &summary_not_modified, &summary_etag, &summary_last_modified,
                                                                       OSTREE_MAX_METADATA_SIZE,
                                                                       cancellable, error))
                        goto out;

                      if (!_sign_verify_for_remote (pull_data->signapi_summary_verifiers, bytes_summary, signatures, NULL, error))
                        goto out;
                    }
                  else
                    {
                      g_propagate_error (error, g_steal_pointer (&temp_error));
                      goto out;
                    }
                }
            }
        }

      if (bytes_summary)
        {
          pull_data->summary_data = g_bytes_ref (bytes_summary);
          pull_data->summary_etag = g_strdup (summary_etag);
          pull_data->summary_last_modified = summary_last_modified;
          pull_data->summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes_summary, FALSE);

          if (!g_variant_is_normal_form (pull_data->summary))
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Not normal form");
              goto out;
            }
          if (!g_variant_is_of_type (pull_data->summary, OSTREE_SUMMARY_GVARIANT_FORMAT))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Doesn't match variant type '%s'",
                           (char *)OSTREE_SUMMARY_GVARIANT_FORMAT);
              goto out;
            }

          if (bytes_sig)
            {
              pull_data->summary_data_sig = g_bytes_ref (bytes_sig);
              pull_data->summary_sig_etag = g_strdup (summary_sig_etag);
              pull_data->summary_sig_last_modified = summary_sig_last_modified;
            }
        }

      if (!summary_from_cache && bytes_summary && bytes_sig && summary_sig_bytes_v == NULL)
        {
          if (!pull_data->remote_repo_local &&
              !_ostree_repo_cache_summary (self,
                                           remote_name_or_baseurl,
                                           bytes_summary,
                                           summary_etag, summary_last_modified,
                                           bytes_sig,
                                           summary_sig_etag, summary_sig_last_modified,
                                           cancellable,
                                           error))
            goto out;
        }

      if (pull_data->summary)
        {
          additional_metadata = g_variant_get_child_value (pull_data->summary, 1);

          if (!g_variant_lookup (additional_metadata, OSTREE_SUMMARY_COLLECTION_ID, "&s", &main_collection_id))
            main_collection_id = NULL;
          else if (!ostree_validate_collection_id (main_collection_id, error))
            goto out;

          refs = g_variant_get_child_value (pull_data->summary, 0);
          for (i = 0, n = g_variant_n_children (refs); i < n; i++)
            {
              const char *refname;
              g_autoptr(GVariant) ref = g_variant_get_child_value (refs, i);

              g_variant_get_child (ref, 0, "&s", &refname);

              if (!ostree_validate_rev (refname, error))
                goto out;

              if (pull_data->is_mirror && !refs_to_fetch && !opt_collection_refs_set)
                {
                  g_hash_table_insert (requested_refs_to_fetch,
                                       ostree_collection_ref_new (main_collection_id, refname), NULL);
                }
            }

          g_autoptr(GVariant) collection_map = NULL;
          collection_map = g_variant_lookup_value (additional_metadata, OSTREE_SUMMARY_COLLECTION_MAP, G_VARIANT_TYPE ("a{sa(s(taya{sv}))}"));
          if (collection_map != NULL)
            {
              GVariantIter collection_map_iter;
              const char *collection_id;
              g_autoptr(GVariant) collection_refs = NULL;

              g_variant_iter_init (&collection_map_iter, collection_map);

              while (g_variant_iter_loop (&collection_map_iter, "{&s@a(s(taya{sv}))}", &collection_id, &collection_refs))
                {
                  if (!ostree_validate_collection_id (collection_id, error))
                    goto out;

                  for (i = 0, n = g_variant_n_children (collection_refs); i < n; i++)
                    {
                      const char *refname;
                      g_autoptr(GVariant) ref = g_variant_get_child_value (collection_refs, i);

                      g_variant_get_child (ref, 0, "&s", &refname);

                      if (!ostree_validate_rev (refname, error))
                        goto out;

                      if (pull_data->is_mirror && !refs_to_fetch && !opt_collection_refs_set)
                        {
                          g_hash_table_insert (requested_refs_to_fetch,
                                               ostree_collection_ref_new (collection_id, refname), NULL);
                        }
                    }
                }
            }

          deltas = g_variant_lookup_value (additional_metadata, OSTREE_SUMMARY_STATIC_DELTAS, G_VARIANT_TYPE ("a{sv}"));
          pull_data->summary_has_deltas = deltas != NULL && g_variant_n_children (deltas) > 0;
          if (!collect_available_deltas_for_pull (pull_data, deltas, error))
            goto out;

          g_variant_lookup (additional_metadata, OSTREE_SUMMARY_INDEXED_DELTAS, "b", &pull_data->has_indexed_deltas);
        }

      if (pull_data->summary &&
          g_variant_lookup (additional_metadata, OSTREE_SUMMARY_MODE, "s", &remote_mode_str) &&
          g_variant_lookup (additional_metadata, OSTREE_SUMMARY_TOMBSTONE_COMMITS, "b", &tombstone_commits))
        {
          if (!ostree_repo_mode_from_string (remote_mode_str, &pull_data->remote_mode, error))
            goto out;
          pull_data->has_tombstone_commits = tombstone_commits;
          remote_mode_loaded = TRUE;
        }
    }

  if (pull_data->require_static_deltas && !pull_data->has_indexed_deltas && !pull_data->summary_has_deltas)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Fetch configured to require static deltas, but no summary deltas or delta index found");
      goto out;
    }

  if (remote_mode_loaded && pull_data->remote_repo_local == NULL)
      {
        /* Fall-back path which loads the necessary config from the remote’s
         * `config` file (unless we already read it above). Doing so is deprecated since it means an
         * additional round trip to the remote for each pull. No need to do
         * it for local pulls. */
        if (remote_config == NULL &&
            !load_remote_repo_config (pull_data, &remote_config, cancellable, error))
          goto out;

        if (!ot_keyfile_get_value_with_default (remote_config, "core", "mode", "bare",
                                                &remote_mode_str, error))
          goto out;

        if (!ostree_repo_mode_from_string (remote_mode_str, &pull_data->remote_mode, error))
          goto out;

        if (!ot_keyfile_get_boolean_with_default (remote_config, "core", "tombstone-commits", FALSE,
                                                  &pull_data->has_tombstone_commits, error))
          goto out;

        remote_mode_loaded = TRUE;
      }

  if (remote_mode_loaded && pull_data->remote_repo_local == NULL && pull_data->remote_mode != OSTREE_REPO_MODE_ARCHIVE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't pull from archives with mode \"%s\"",
                   remote_mode_str);
      goto out;
    }

  /* Resolve the checksum for each ref. This has to be done into a new hash table,
   * since we can’t modify the keys of @requested_refs_to_fetch while iterating
   * over it, and we need to ensure the collection IDs are resolved too. */
  updated_requested_refs_to_fetch = g_hash_table_new_full (ostree_collection_ref_hash,
                                                           ostree_collection_ref_equal,
                                                           (GDestroyNotify) ostree_collection_ref_free,
                                                           g_free);
  GLNX_HASH_TABLE_FOREACH_KV (requested_refs_to_fetch, const OstreeCollectionRef*, ref,
                                                       const char*, override_commitid)
    {
      g_autofree char *checksum = NULL;
      g_autoptr(OstreeCollectionRef) ref_with_collection = NULL;

      /* Support specifying "" for an override commitid */
      if (override_commitid && *override_commitid)
        {
          ref_with_collection = ostree_collection_ref_dup (ref);
          checksum = g_strdup (override_commitid);
        }
      else
        {
          if (pull_data->summary)
            {
              gsize commit_size = 0;
              guint64 *malloced_size;
              g_autofree gchar *collection_id = NULL;

              if (!lookup_commit_checksum_and_collection_from_summary (pull_data, ref, &checksum, &commit_size, &collection_id, error))
                goto out;

              ref_with_collection = ostree_collection_ref_new (collection_id, ref->ref_name);

              malloced_size = g_new0 (guint64, 1);
              *malloced_size = commit_size;
              g_hash_table_insert (pull_data->expected_commit_sizes, g_strdup (checksum), malloced_size);
            }
          else
            {
              if (!fetch_ref_contents (pull_data, main_collection_id, ref, &checksum, cancellable, error))
                goto out;

              ref_with_collection = ostree_collection_ref_dup (ref);
            }
        }

      /* If we have timestamp checking enabled, find the current value of
       * the ref, and store its timestamp in the hash map, to check later.
       */
      if (pull_data->timestamp_check)
        {
          g_autofree char *from_rev = NULL;
          if (!ostree_repo_resolve_rev (pull_data->repo, ref_with_collection->ref_name, TRUE,
                                        &from_rev, error))
            goto out;
          /* Explicitly store NULL if there's no previous revision. We do
           * this so we can assert() if we somehow didn't find a ref in the
           * hash at all.  Note we don't copy the collection-ref, so the
           * lifetime of this hash must be equal to `requested_refs_to_fetch`.
           */
          g_hash_table_insert (pull_data->ref_original_commits, ref_with_collection,
                               g_steal_pointer (&from_rev));
        }

      g_hash_table_replace (updated_requested_refs_to_fetch,
                            g_steal_pointer (&ref_with_collection),
                            g_steal_pointer (&checksum));
    }

  /* Resolve refs to a checksum if necessary */
  if (pull_data->timestamp_check_from_rev &&
      !ostree_validate_checksum_string (pull_data->timestamp_check_from_rev, NULL))
    {
      g_autofree char *from_rev = NULL;
      if (!ostree_repo_resolve_rev (pull_data->repo, pull_data->timestamp_check_from_rev, FALSE,
                                    &from_rev, error))
        goto out;
      g_free (pull_data->timestamp_check_from_rev);
      pull_data->timestamp_check_from_rev = g_steal_pointer (&from_rev);
    }

  g_hash_table_unref (requested_refs_to_fetch);
  requested_refs_to_fetch = g_steal_pointer (&updated_requested_refs_to_fetch);
  if (g_hash_table_size (requested_refs_to_fetch) == 1)
    {
      GLNX_HASH_TABLE_FOREACH (requested_refs_to_fetch,
                               const OstreeCollectionRef *, ref)
        {
          the_ref_to_fetch = ref->ref_name;
          break;
        }
    }

  if (opt_ref_keyring_map_set)
    {
      const gchar *collection_id, *ref_name, *keyring_remote_name;

      while (g_variant_iter_loop (ref_keyring_map_iter, "(&s&s&s)", &collection_id, &ref_name, &keyring_remote_name))
        {
          g_autoptr(OstreeCollectionRef) c_r = NULL;

          if (!ostree_validate_collection_id (collection_id, error))
            goto out;
          if (!ostree_validate_rev (ref_name, error))
            goto out;
          if (!ostree_validate_remote_name (keyring_remote_name, error))
            goto out;

          c_r = ostree_collection_ref_new (collection_id, ref_name);
          if (!g_hash_table_contains (requested_refs_to_fetch, c_r))
            continue;
          g_hash_table_insert (pull_data->ref_keyring_map,
                               g_steal_pointer (&c_r),
                               g_strdup (keyring_remote_name));
        }
    }

  /* Create the state directory here - it's new with the commitpartial code,
   * and may not exist in older repositories.
   */
  if (mkdirat (pull_data->repo->repo_dir_fd, "state", 0777) != 0)
    {
      if (G_UNLIKELY (errno != EEXIST))
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_OBJECTS;

  /* Now discard the previous fetcher, as it was bound to a temporary main context
   * for synchronous requests.
   */
  if (!reinitialize_fetcher (pull_data, remote_name_or_baseurl, error))
    goto out;

  pull_data->legacy_transaction_resuming = FALSE;
  if (!inherit_transaction &&
      !ostree_repo_prepare_transaction (pull_data->repo, &pull_data->legacy_transaction_resuming,
                                        cancellable, error))
    goto out;

  if (pull_data->legacy_transaction_resuming)
    g_debug ("resuming legacy transaction");

  /* Initiate requests for explicit commit revisions */
  GLNX_HASH_TABLE_FOREACH_V (commits_to_fetch, const char*, commit)
    {
      if (!initiate_request (pull_data, NULL, commit, error))
        goto out;
    }

  /* Initiate requests for refs */
  GLNX_HASH_TABLE_FOREACH_KV (requested_refs_to_fetch, const OstreeCollectionRef*, ref,
                                                       const char*, to_revision)
    {
      if (!initiate_request (pull_data, ref, to_revision, error))
        goto out;
    }

  if (pull_data->progress)
    {
      /* Setup a custom frequency if set */
      if (update_frequency > 0)
        update_timeout = g_timeout_source_new (pull_data->dry_run ? 0 : update_frequency);
      else
        update_timeout = g_timeout_source_new_seconds (pull_data->dry_run ? 0 : 1);

      g_source_set_priority (update_timeout, G_PRIORITY_HIGH);
      g_source_set_callback (update_timeout, update_progress, pull_data, NULL);
      g_source_attach (update_timeout, pull_data->main_context);
    }

  /* Now await work completion */
  while (!pull_termination_condition (pull_data))
    g_main_context_iteration (pull_data->main_context, TRUE);

  if (pull_data->caught_error)
    goto out;

  if (pull_data->dry_run)
    {
      ret = TRUE;
      goto out;
    }

  g_assert_cmpint (pull_data->n_outstanding_metadata_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_metadata_write_requests, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_write_requests, ==, 0);

  GLNX_HASH_TABLE_FOREACH_KV (requested_refs_to_fetch, const OstreeCollectionRef*, ref,
                                                       const char*, checksum)
    {
      g_autofree char *remote_ref = NULL;
      g_autofree char *original_rev = NULL;

      if (pull_data->remote_name)
        remote_ref = g_strdup_printf ("%s:%s", pull_data->remote_name, ref->ref_name);
      else
        remote_ref = g_strdup (ref->ref_name);

      if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
        goto out;

      if (original_rev && strcmp (checksum, original_rev) == 0)
        {
        }
      else
        {
          if (pull_data->is_mirror)
            ostree_repo_transaction_set_collection_ref (pull_data->repo,
                                                    ref, checksum);
          else
            ostree_repo_transaction_set_ref (pull_data->repo,
                                             pull_data->remote_refspec_name ?: pull_data->remote_name,
                                             ref->ref_name, checksum);
        }
    }

  if (pull_data->is_mirror && pull_data->summary_data &&
      !refs_to_fetch && !opt_collection_refs_set && !configured_branches)
    {
      GLnxFileReplaceFlags replaceflag =
        pull_data->repo->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : 0;
      gsize len;
      const guint8 *buf = g_bytes_get_data (pull_data->summary_data, &len);

      if (!glnx_file_replace_contents_at (pull_data->repo->repo_dir_fd, "summary",
                                          buf, len, replaceflag,
                                          cancellable, error))
        goto out;

      store_file_cache_properties (pull_data->repo->repo_dir_fd, "summary",
                                   pull_data->summary_etag, pull_data->summary_last_modified);

      if (pull_data->summary_data_sig)
        {
          buf = g_bytes_get_data (pull_data->summary_data_sig, &len);
          if (!glnx_file_replace_contents_at (pull_data->repo->repo_dir_fd, "summary.sig",
                                              buf, len, replaceflag,
                                              cancellable, error))
            goto out;

          store_file_cache_properties (pull_data->repo->repo_dir_fd, "summary.sig",
                                       pull_data->summary_sig_etag, pull_data->summary_sig_last_modified);
        }
    }

  if (!inherit_transaction &&
      !ostree_repo_commit_transaction (pull_data->repo, &tstats, cancellable, error))
    goto out;

  end_time = g_get_monotonic_time ();

  bytes_transferred = _ostree_fetcher_bytes_transferred (pull_data->fetcher);
  if (pull_data->progress)
    {
      g_autoptr(GString) buf = g_string_new ("");

      /* Ensure the rest of the progress keys are set appropriately. */
      update_progress (pull_data);

      /* See if we did a local-only import */
      if (pull_data->remote_repo_local)
        g_string_append_printf (buf, "%u metadata, %u content objects imported",
                                pull_data->n_imported_metadata, pull_data->n_imported_content);
      else if (pull_data->n_fetched_deltaparts > 0)
        g_string_append_printf (buf, "%u delta parts, %u loose fetched",
                                pull_data->n_fetched_deltaparts,
                                pull_data->n_fetched_metadata + pull_data->n_fetched_content);
      else
        g_string_append_printf (buf, "%u metadata, %u content objects fetched",
                                pull_data->n_fetched_metadata, pull_data->n_fetched_content);
      if (!pull_data->remote_repo_local &&
          (pull_data->n_imported_metadata || pull_data->n_imported_content))
        g_string_append_printf (buf, " (%u meta, %u content local)",
                                pull_data->n_imported_metadata,
                                pull_data->n_imported_content);

      if (bytes_transferred > 0)
        {
          guint shift;
          if (bytes_transferred < 1024)
            shift = 1;
          else
            shift = 1024;
          g_string_append_printf (buf, "; %" G_GUINT64_FORMAT " %s transferred in %u seconds",
                                  (guint64)(bytes_transferred / shift),
                                  shift == 1 ? "B" : "KiB",
                                  (guint) ((end_time - pull_data->start_time) / G_USEC_PER_SEC));
        }
      if (!inherit_transaction)
        {
          g_autofree char *bytes_written = g_format_size (tstats.content_bytes_written);
          g_string_append_printf (buf, "; %s content written", bytes_written);
        }

      ostree_async_progress_set_status (pull_data->progress, buf->str);
    }

#ifdef HAVE_LIBSYSTEMD
  if (bytes_transferred > 0 && pull_data->remote_name)
    {
      g_autoptr(GString) msg = g_string_new ("");
      if (the_ref_to_fetch)
        g_string_append_printf (msg, "libostree pull from '%s' for %s complete",
                                pull_data->remote_name, the_ref_to_fetch);
      else
        g_string_append_printf (msg, "libostree pull from '%s' for %u refs complete",
                                pull_data->remote_name, g_hash_table_size (requested_refs_to_fetch));

      const char *gpg_verify_state;
#ifndef OSTREE_DISABLE_GPGME
      if (pull_data->gpg_verify_summary)
        {
          if (pull_data->gpg_verify)
            gpg_verify_state = "summary+commit";
          else
            gpg_verify_state = "summary-only";
        }
      else
        gpg_verify_state = (pull_data->gpg_verify ? "commit" : "disabled");

#else
      gpg_verify_state = "disabled";
#endif /* OSTREE_DISABLE_GPGME */
      g_string_append_printf (msg, "\nsecurity: GPG: %s ", gpg_verify_state);

      const char *sign_verify_state;
      sign_verify_state = (pull_data->signapi_commit_verifiers ? "commit" : "disabled");
      g_string_append_printf (msg, "\nsecurity: SIGN: %s ", sign_verify_state);

      OstreeFetcherURI *first_uri = pull_data->meta_mirrorlist->pdata[0];
      g_autofree char *first_scheme = _ostree_fetcher_uri_get_scheme (first_uri);
      if (g_str_has_prefix (first_scheme, "http"))
        {
          g_string_append (msg, "http: ");
          switch (pull_data->fetcher_security_state)
            {
            case OSTREE_FETCHER_SECURITY_STATE_CA_PINNED:
              g_string_append (msg, "CA-pinned");
              break;
            case OSTREE_FETCHER_SECURITY_STATE_TLS:
              g_string_append (msg, "TLS");
              break;
            case OSTREE_FETCHER_SECURITY_STATE_INSECURE:
              g_string_append (msg, "insecure");
              break;
            }
        }
      g_string_append (msg, "\n");

      if (pull_data->n_fetched_deltaparts > 0)
        g_string_append_printf (msg, "delta: parts: %u loose: %u",
                                pull_data->n_fetched_deltaparts,
                                pull_data->n_fetched_metadata + pull_data->n_fetched_content);
      else
        g_string_append_printf (msg, "non-delta: meta: %u content: %u",
                                pull_data->n_fetched_metadata, pull_data->n_fetched_content);
      const guint n_seconds = (guint) ((end_time - pull_data->start_time) / G_USEC_PER_SEC);
      g_autofree char *formatted_xferred = g_format_size (bytes_transferred);
      g_string_append_printf (msg, "\ntransfer: secs: %u size: %s", n_seconds, formatted_xferred);
      if (pull_data->signapi_commit_verifiers)
        {
          g_assert_cmpuint (g_hash_table_size (pull_data->signapi_verified_commits), >, 0);
        }

      ot_journal_send ("MESSAGE=%s", msg->str,
                       "MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(OSTREE_MESSAGE_FETCH_COMPLETE_ID),
                       "OSTREE_REMOTE=%s", pull_data->remote_name,
                       "OSTREE_SIGN=%s", sign_verify_state,
                       "OSTREE_GPG=%s", gpg_verify_state,
                       "OSTREE_SECONDS=%u", n_seconds,
                       "OSTREE_XFER_SIZE=%s", formatted_xferred,
                       NULL);
    }
#endif

  /* iterate over commits fetched and delete any commitpartial files */
  if (pull_data->dirs == NULL && !pull_data->is_commit_only)
    {
      GLNX_HASH_TABLE_FOREACH_V (requested_refs_to_fetch, const char*, checksum)
        {
          if (!ostree_repo_mark_commit_partial (pull_data->repo, checksum, FALSE, error))
            goto out;
        }

      GLNX_HASH_TABLE_FOREACH_V (commits_to_fetch, const char*, commit)
        {
          if (!ostree_repo_mark_commit_partial (pull_data->repo, commit, FALSE, error))
            goto out;
        }

      /* and finally any parent commits we might also have pulled because of depth>0 */
      GLNX_HASH_TABLE_FOREACH (pull_data->commit_to_depth, const char*, commit)
        {
          if (!ostree_repo_mark_commit_partial (pull_data->repo, commit, FALSE, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  /* This is pretty ugly - we have two error locations, because we
   * have a mix of synchronous and async code.  Mixing them gets messy
   * as we need to avoid overwriting errors.
   */
  if (pull_data->cached_async_error && error && !*error)
    g_propagate_error (error, pull_data->cached_async_error);
  else
    g_clear_error (&pull_data->cached_async_error);

  if (!inherit_transaction)
    ostree_repo_abort_transaction (pull_data->repo, cancellable, NULL);
  g_main_context_unref (pull_data->main_context);
  if (update_timeout)
    g_source_destroy (update_timeout);
  g_strfreev (configured_branches);
  g_clear_object (&pull_data->fetcher);
  g_clear_pointer (&pull_data->extra_headers, g_variant_unref);
  g_clear_object (&pull_data->cancellable);
  g_clear_pointer (&pull_data->localcache_repos, g_ptr_array_unref);
  g_clear_object (&pull_data->remote_repo_local);
  g_free (pull_data->remote_refspec_name);
  g_free (pull_data->remote_name);
  g_free (pull_data->append_user_agent);
  g_clear_pointer (&pull_data->signapi_commit_verifiers, g_ptr_array_unref);
  g_clear_pointer (&pull_data->signapi_summary_verifiers, g_ptr_array_unref);
  g_clear_pointer (&pull_data->meta_mirrorlist, g_ptr_array_unref);
  g_clear_pointer (&pull_data->content_mirrorlist, g_ptr_array_unref);
  g_clear_pointer (&pull_data->summary_data, g_bytes_unref);
  g_clear_pointer (&pull_data->summary_etag, g_free);
  g_clear_pointer (&pull_data->summary_data_sig, g_bytes_unref);
  g_clear_pointer (&pull_data->summary_sig_etag, g_free);
  g_clear_pointer (&pull_data->summary, g_variant_unref);
  g_clear_pointer (&pull_data->static_delta_superblocks, g_ptr_array_unref);
  g_clear_pointer (&pull_data->commit_to_depth, g_hash_table_unref);
  g_clear_pointer (&pull_data->expected_commit_sizes, g_hash_table_unref);
  g_clear_pointer (&pull_data->scanned_metadata, g_hash_table_unref);
  g_clear_pointer (&pull_data->fetched_detached_metadata, g_hash_table_unref);
  g_clear_pointer (&pull_data->summary_deltas_checksums, g_hash_table_unref);
  g_clear_pointer (&pull_data->ref_original_commits, g_hash_table_unref);
  g_free (pull_data->timestamp_check_from_rev);
  g_clear_pointer (&pull_data->verified_commits, g_hash_table_unref);
  g_clear_pointer (&pull_data->signapi_verified_commits, g_hash_table_unref);
  g_clear_pointer (&pull_data->ref_keyring_map, g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_fallback_content, g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_metadata, g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_content, g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_metadata, g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_delta_indexes, g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_delta_superblocks, g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_deltaparts, g_hash_table_unref);
  g_queue_foreach (&pull_data->scan_object_queue, (GFunc) scan_object_queue_data_free, NULL);
  g_queue_clear (&pull_data->scan_object_queue);
  g_clear_pointer (&pull_data->idle_src, g_source_destroy);
  g_clear_pointer (&pull_data->dirs, g_ptr_array_unref);
  g_clear_pointer (&remote_config, g_key_file_unref);
  return ret;
}

/* Structure used in ostree_repo_find_remotes_async() which stores metadata
 * about a given OSTree commit. This includes the metadata from the commit
 * #GVariant, plus some working state which is used to work out which remotes
 * have refs pointing to this commit. */
typedef struct
{
  gchar *checksum;  /* always set */
  guint64 commit_size;  /* always set */
  guint64 timestamp;  /* 0 for unknown */
  GVariant *additional_metadata;
  GArray *refs;  /* (element-type gsize), indexes to refs which point to this commit on at least one remote */
} CommitMetadata;

static void
commit_metadata_free (CommitMetadata *info)
{
  g_clear_pointer (&info->refs, g_array_unref);
  g_free (info->checksum);
  g_clear_pointer (&info->additional_metadata, g_variant_unref);
  g_free (info);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CommitMetadata, commit_metadata_free)

static CommitMetadata *
commit_metadata_new (const gchar *checksum,
                     guint64      commit_size,
                     guint64      timestamp,
                     GVariant    *additional_metadata)
{
  g_autoptr(CommitMetadata) info = NULL;

  info = g_new0 (CommitMetadata, 1);
  info->checksum = g_strdup (checksum);
  info->commit_size = commit_size;
  info->timestamp = timestamp;
  info->additional_metadata = (additional_metadata != NULL) ? g_variant_ref (additional_metadata) : NULL;
  info->refs = g_array_new (FALSE, FALSE, sizeof (gsize));

  return g_steal_pointer (&info);
}

/* Structure used in ostree_repo_find_remotes_async() to store a grid (or table)
 * of pointers, indexed by rows and columns. Basically an encapsulated 2D array.
 * See the comments in ostree_repo_find_remotes_async() for its semantics
 * there. */
typedef struct
{
  gsize width;  /* pointers */
  gsize height;  /* pointers */
  gconstpointer pointers[];  /* n_pointers = width * height */
} PointerTable;

static void
pointer_table_free (PointerTable *table)
{
  g_free (table);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PointerTable, pointer_table_free)

/* Both dimensions are in numbers of pointers. */
static PointerTable *
pointer_table_new (gsize width,
                   gsize height)
{
  g_autoptr(PointerTable) table = NULL;

  g_return_val_if_fail (width > 0, NULL);
  g_return_val_if_fail (height > 0, NULL);
  g_return_val_if_fail (width <= (G_MAXSIZE - sizeof (PointerTable)) / sizeof (gconstpointer) / height, NULL);

  table = g_malloc0 (sizeof (PointerTable) + sizeof (gconstpointer) * width * height);
  table->width = width;
  table->height = height;

  return g_steal_pointer (&table);
}

static gconstpointer
pointer_table_get (const PointerTable *table,
                   gsize               x,
                   gsize               y)
{
  g_return_val_if_fail (table != NULL, FALSE);
  g_return_val_if_fail (x < table->width, FALSE);
  g_return_val_if_fail (y < table->height, FALSE);

  return table->pointers[table->width * y + x];
}

static void
pointer_table_set (PointerTable  *table,
                   gsize          x,
                   gsize          y,
                   gconstpointer  value)
{
  g_return_if_fail (table != NULL);
  g_return_if_fail (x < table->width);
  g_return_if_fail (y < table->height);

  table->pointers[table->width * y + x] = value;
}

/* Validate the given struct contains a valid collection ID and ref name. */
static gboolean
is_valid_collection_ref (const OstreeCollectionRef *ref)
{
  return (ref != NULL &&
          ostree_validate_rev (ref->ref_name, NULL) &&
          ostree_validate_collection_id (ref->collection_id, NULL));
}

/* Validate @refs is non-%NULL, non-empty, and contains only valid collection
 * and ref names. */
static gboolean
is_valid_collection_ref_array (const OstreeCollectionRef * const *refs)
{
  gsize i;

  if (refs == NULL || *refs == NULL)
    return FALSE;

  for (i = 0; refs[i] != NULL; i++)
    {
      if (!is_valid_collection_ref (refs[i]))
        return FALSE;
    }

  return TRUE;
}

/* Validate @finders is non-%NULL, non-empty, and contains only valid
 * #OstreeRepoFinder instances. */
static gboolean
is_valid_finder_array (OstreeRepoFinder **finders)
{
  gsize i;

  if (finders == NULL || *finders == NULL)
    return FALSE;

  for (i = 0; finders[i] != NULL; i++)
    {
      if (!OSTREE_IS_REPO_FINDER (finders[i]))
        return FALSE;
    }

  return TRUE;
}

/* Closure used to carry inputs from ostree_repo_find_remotes_async() to
 * find_remotes_cb(). */
typedef struct
{
  OstreeCollectionRef **refs;
  GVariant *options;
  OstreeAsyncProgress *progress;
  OstreeRepoFinder *default_finder_avahi;
  guint n_network_retries;
} FindRemotesData;

static void
find_remotes_data_free (FindRemotesData *data)
{
  g_clear_object (&data->default_finder_avahi);
  g_clear_object (&data->progress);
  g_clear_pointer (&data->options, g_variant_unref);
  ostree_collection_ref_freev (data->refs);

  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FindRemotesData, find_remotes_data_free)

static FindRemotesData *
find_remotes_data_new (const OstreeCollectionRef * const *refs,
                       GVariant                      *options,
                       OstreeAsyncProgress           *progress,
                       OstreeRepoFinder              *default_finder_avahi,
                       guint                          n_network_retries)
{
  g_autoptr(FindRemotesData) data = NULL;

  data = g_new0 (FindRemotesData, 1);
  data->refs = ostree_collection_ref_dupv (refs);
  data->options = (options != NULL) ? g_variant_ref (options) : NULL;
  data->progress = (progress != NULL) ? g_object_ref (progress) : NULL;
  data->default_finder_avahi = (default_finder_avahi != NULL) ? g_object_ref (default_finder_avahi) : NULL;
  data->n_network_retries = n_network_retries;

  return g_steal_pointer (&data);
}

static gchar *
uint64_secs_to_iso8601 (guint64 secs)
{
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc (secs);

  if (dt != NULL)
    return g_date_time_format (dt, "%FT%TZ");
  else
    return g_strdup ("invalid");
}

static gint
sort_results_cb (gconstpointer a,
                 gconstpointer b)
{
  const OstreeRepoFinderResult **result_a = (const OstreeRepoFinderResult **) a;
  const OstreeRepoFinderResult **result_b = (const OstreeRepoFinderResult **) b;

  return ostree_repo_finder_result_compare (*result_a, *result_b);
}

static void
repo_finder_result_free0 (OstreeRepoFinderResult *result)
{
  if (result == NULL)
    return;

  ostree_repo_finder_result_free (result);
}

static void find_remotes_cb (GObject      *obj,
                             GAsyncResult *result,
                             gpointer      user_data);

/**
 * ostree_repo_find_remotes_async:
 * @self: an #OstreeRepo
 * @refs: (array zero-terminated=1): non-empty array of collection–ref pairs to find remotes for
 * @options: (nullable): a GVariant `a{sv}` with an extensible set of flags
 * @finders: (array zero-terminated=1) (transfer none): non-empty array of
 *    #OstreeRepoFinder instances to use, or %NULL to use the system defaults
 * @progress: (nullable): an #OstreeAsyncProgress to update with the operation’s
 *    progress, or %NULL
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: asynchronous completion callback
 * @user_data: data to pass to @callback
 *
 * Find reachable remote URIs which claim to provide any of the given named
 * @refs. This will search for configured remotes (#OstreeRepoFinderConfig),
 * mounted volumes (#OstreeRepoFinderMount) and (if enabled at compile time)
 * local network peers (#OstreeRepoFinderAvahi). In order to use a custom
 * configuration of #OstreeRepoFinder instances, call
 * ostree_repo_finder_resolve_all_async() on them individually.
 *
 * Any remote which is found and which claims to support any of the given @refs
 * will be returned in the results. It is possible that a remote claims to
 * support a given ref, but turns out not to — it is not possible to verify this
 * until ostree_repo_pull_from_remotes_async() is called.
 *
 * The returned results will be sorted with the most useful first — this is
 * typically the remote which claims to provide the most of @refs, at the lowest
 * latency.
 *
 * Each result contains a list of the subset of @refs it claims to provide. It
 * is possible for a non-empty list of results to be returned, but for some of
 * @refs to not be listed in any of the results. Callers must check for this.
 *
 * Pass the results to ostree_repo_pull_from_remotes_async() to pull the given @refs
 * from those remotes.
 *
 * The following @options are currently defined:
 *
 *   * `override-commit-ids` (`as`): Array of specific commit IDs to fetch. The nth
 *   commit ID applies to the nth ref, so this must be the same length as @refs, if
 *   provided.
 *   * `n-network-retries` (`u`): Number of times to retry each download on
 *   receiving a transient network error, such as a socket timeout; default is
 *   5, 0 means return errors without retrying. Since: 2018.6
 *
 * @finders must be a non-empty %NULL-terminated array of the #OstreeRepoFinder
 * instances to use, or %NULL to use the system default set of finders, which
 * will typically be all available finders using their default options (but
 * this is not guaranteed).
 *
 * GPG verification of commits will be used unconditionally.
 *
 * This will use the thread-default #GMainContext, but will not iterate it.
 *
 * Since: 2018.6
 */
void
ostree_repo_find_remotes_async (OstreeRepo                     *self,
                                const OstreeCollectionRef * const  *refs,
                                GVariant                       *options,
                                OstreeRepoFinder              **finders,
                                OstreeAsyncProgress            *progress,
                                GCancellable                   *cancellable,
                                GAsyncReadyCallback             callback,
                                gpointer                        user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(FindRemotesData) data = NULL;
  OstreeRepoFinder *default_finders[4] = { NULL, };
  g_autoptr(OstreeRepoFinder) finder_config = NULL;
  g_autoptr(OstreeRepoFinder) finder_mount = NULL;
  g_autoptr(OstreeRepoFinder) finder_avahi = NULL;
  g_autofree char **override_commit_ids = NULL;
  guint n_network_retries = DEFAULT_N_NETWORK_RETRIES;

  g_return_if_fail (OSTREE_IS_REPO (self));
  g_return_if_fail (is_valid_collection_ref_array (refs));
  g_return_if_fail (options == NULL ||
                    g_variant_is_of_type (options, G_VARIANT_TYPE_VARDICT));
  g_return_if_fail (finders == NULL || is_valid_finder_array (finders));
  g_return_if_fail (progress == NULL || OSTREE_IS_ASYNC_PROGRESS (progress));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  if (options)
    {
      (void) g_variant_lookup (options, "override-commit-ids", "^a&s", &override_commit_ids);
      g_return_if_fail (override_commit_ids == NULL || g_strv_length ((gchar **) refs) == g_strv_length (override_commit_ids));

      (void) g_variant_lookup (options, "n-network-retries", "u", &n_network_retries);
    }

  /* Set up a task for the whole operation. */
  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_find_remotes_async);

  /* Are we using #OstreeRepoFinders provided by the user, or the defaults? */
  if (finders == NULL)
    {
      guint finder_index = 0;
#ifdef HAVE_AVAHI
      guint avahi_index;
      GMainContext *context = g_main_context_get_thread_default ();
      g_autoptr(GError) local_error = NULL;
#endif  /* HAVE_AVAHI */

      if (g_strv_contains ((const char * const *)self->repo_finders, "config"))
        default_finders[finder_index++] = finder_config = OSTREE_REPO_FINDER (ostree_repo_finder_config_new ());

      if (g_strv_contains ((const char * const *)self->repo_finders, "mount"))
        default_finders[finder_index++] = finder_mount = OSTREE_REPO_FINDER (ostree_repo_finder_mount_new (NULL));

#ifdef HAVE_AVAHI
      if (g_strv_contains ((const char * const *)self->repo_finders, "lan"))
        {
          avahi_index = finder_index;
          default_finders[finder_index++] = finder_avahi = OSTREE_REPO_FINDER (ostree_repo_finder_avahi_new (context));
        }
#endif  /* HAVE_AVAHI */

      /* self->repo_finders is guaranteed to be non-empty */
      g_assert (default_finders != NULL);
      finders = default_finders;

#ifdef HAVE_AVAHI
      if (finder_avahi != NULL)
        {
          ostree_repo_finder_avahi_start (OSTREE_REPO_FINDER_AVAHI (finder_avahi),
                                          &local_error);

          if (local_error != NULL)
            {
              /* See ostree-repo-finder-avahi.c:ostree_repo_finder_avahi_start, we
               * intentionally throw this so as to distinguish between the Avahi
               * finder failing because the Avahi daemon wasn't running and
               * the Avahi finder failing because of some actual error.
               *
               * We need to distinguish between g_debug and g_warning here because
               * unit tests that use this code may set G_DEBUG=fatal-warnings which
               * would cause client code to abort if a warning were emitted.
               */
              if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                g_debug ("Avahi finder failed under normal operation; removing it: %s", local_error->message);
              else
                g_warning ("Avahi finder failed abnormally; removing it: %s", local_error->message);

              default_finders[avahi_index] = NULL;
              g_clear_object (&finder_avahi);
            }
        }
#endif  /* HAVE_AVAHI */
    }

  /* We need to keep a pointer to the default Avahi finder so we can stop it
   * again after the operation, which happens implicitly by dropping the final
   * ref. */
  data = find_remotes_data_new (refs, options, progress, finder_avahi, n_network_retries);
  g_task_set_task_data (task, g_steal_pointer (&data), (GDestroyNotify) find_remotes_data_free);

  /* Asynchronously resolve all possible remotes for the given refs. */
  ostree_repo_finder_resolve_all_async (finders, refs, self, cancellable,
                                        find_remotes_cb, g_steal_pointer (&task));
}

/* Find the first instance of (@collection_id, @ref_name) in @refs and return
 * its index; or return %FALSE if nothing’s found. */
static gboolean
collection_refv_contains (const OstreeCollectionRef * const *refs,
                          const gchar                       *collection_id,
                          const gchar                       *ref_name,
                          gsize                             *out_index)
{
  gsize i;

  for (i = 0; refs[i] != NULL; i++)
    {
      if (g_str_equal (refs[i]->collection_id, collection_id) &&
          g_str_equal (refs[i]->ref_name, ref_name))
        {
          *out_index = i;
          return TRUE;
        }
    }

  return FALSE;
}

/* For each ref from @refs which is listed in @summary_refs, cache its metadata
 * from the summary file entry into @commit_metadatas, and add the checksum it
 * points to into @refs_and_remotes_table at (@ref_index, @result_index).
 * @ref_index is the ref’s index in @refs. */
static gboolean
find_remotes_process_refs (OstreeRepo                        *self,
                           const OstreeCollectionRef * const *refs,
                           OstreeRepoFinderResult            *result,
                           gsize                              result_index,
                           const gchar                       *summary_collection_id,
                           GVariant                          *summary_refs,
                           GHashTable                        *commit_metadatas,
                           PointerTable                      *refs_and_remotes_table)
{
  gsize j, n;

  for (j = 0, n = g_variant_n_children (summary_refs); j < n; j++)
    {
      const guchar *csum_bytes;
      g_autoptr(GVariant) ref_v = NULL, csum_v = NULL, commit_metadata_v = NULL, stored_commit_v = NULL;
      guint64 commit_size, commit_timestamp;
      gchar tmp_checksum[OSTREE_SHA256_STRING_LEN + 1];
      gsize ref_index;
      g_autoptr(GDateTime) dt = NULL;
      g_autoptr(GError) error = NULL;
      const gchar *ref_name;
      CommitMetadata *commit_metadata;

      /* Check the ref name. */
      ref_v = g_variant_get_child_value (summary_refs, j);
      g_variant_get_child (ref_v, 0, "&s", &ref_name);

      if (!ostree_validate_rev (ref_name, &error))
        {
          g_debug ("%s: Summary for result ‘%s’ contained invalid ref name ‘%s’: %s",
                   G_STRFUNC, result->remote->name, ref_name, error->message);
          return FALSE;
        }

      /* Check the commit checksum. */
      g_variant_get_child (ref_v, 1, "(t@ay@a{sv})", &commit_size, &csum_v, &commit_metadata_v);

      csum_bytes = ostree_checksum_bytes_peek_validate (csum_v, &error);
      if (csum_bytes == NULL)
        {
          g_debug ("%s: Summary for result ‘%s’ contained invalid ref checksum: %s",
                   G_STRFUNC, result->remote->name, error->message);
          return FALSE;
        }

      ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

      /* Is this a ref we care about? */
      if (!collection_refv_contains (refs, summary_collection_id, ref_name, &ref_index))
        continue;

      /* Load the commit from disk if possible, for verification. */
      if (!ostree_repo_load_commit (self, tmp_checksum, &stored_commit_v, NULL, NULL))
        stored_commit_v = NULL;

      /* Check the additional metadata. */
      if (!g_variant_lookup (commit_metadata_v, OSTREE_COMMIT_TIMESTAMP, "t", &commit_timestamp))
        commit_timestamp = 0;  /* unknown */
      else
        commit_timestamp = GUINT64_FROM_BE (commit_timestamp);

      dt = g_date_time_new_from_unix_utc (commit_timestamp);

      if (dt == NULL)
        {
          g_debug ("%s: Summary for result ‘%s’ contained commit timestamp %" G_GUINT64_FORMAT " which is too far in the future. Resetting to 0.",
                   G_STRFUNC, result->remote->name, commit_timestamp);
          commit_timestamp = 0;
        }

      /* Check and store the commit metadata. */
      commit_metadata = g_hash_table_lookup (commit_metadatas, tmp_checksum);

      if (commit_metadata == NULL)
        {
          commit_metadata = commit_metadata_new (tmp_checksum, commit_size,
                                                 (stored_commit_v != NULL) ? ostree_commit_get_timestamp (stored_commit_v) : 0,
                                                 NULL);
          g_hash_table_insert (commit_metadatas, commit_metadata->checksum,
                               commit_metadata  /* transfer */);
        }

      /* Update the metadata if possible. */
      if (commit_metadata->timestamp == 0)
        {
          commit_metadata->timestamp = commit_timestamp;
        }
      else if (commit_timestamp != 0 && commit_metadata->timestamp != commit_timestamp)
        {
          g_debug ("%s: Summary for result ‘%s’ contained commit timestamp %" G_GUINT64_FORMAT " which did not match existing timestamp %" G_GUINT64_FORMAT ". Ignoring.",
                   G_STRFUNC, result->remote->name, commit_timestamp, commit_metadata->timestamp);
          return FALSE;
        }

      if (commit_size != commit_metadata->commit_size)
        {
          g_debug ("%s: Summary for result ‘%s’ contained commit size %" G_GUINT64_FORMAT "B which did not match existing size %" G_GUINT64_FORMAT "B. Ignoring.",
                   G_STRFUNC, result->remote->name, commit_size, commit_metadata->commit_size);
          return FALSE;
        }

      pointer_table_set (refs_and_remotes_table, ref_index, result_index, commit_metadata->checksum);
      g_array_append_val (commit_metadata->refs, ref_index);

      g_debug ("%s: Remote ‘%s’ lists ref ‘%s’ mapping to commit ‘%s’.",
               G_STRFUNC, result->remote->name, ref_name, commit_metadata->checksum);
    }

  return TRUE;
}

static void
find_remotes_cb (GObject      *obj,
                 GAsyncResult *async_result,
                 gpointer      user_data)
{
  OstreeRepo *self;
  g_autoptr(GTask) task = NULL;
  GCancellable *cancellable;
  const FindRemotesData *data;
  const OstreeCollectionRef * const *refs;
  /* FIXME: We currently do nothing with @progress. Comment out to assuage -Wunused-variable */
  /* OstreeAsyncProgress *progress; */
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) results = NULL;  /* (element-type OstreeRepoFinderResult) */
  gsize i;
  g_autoptr(PointerTable) refs_and_remotes_table = NULL;  /* (element-type commit-checksum) */
  g_autoptr(GHashTable) commit_metadatas = NULL;  /* (element-type commit-checksum CommitMetadata) */
  g_autoptr(OstreeFetcher) fetcher = NULL;
  g_autofree const gchar **ref_to_latest_commit = NULL;  /* indexed as @refs; (element-type commit-checksum) */
  g_autofree guint64 *ref_to_latest_timestamp = NULL;  /* indexed as @refs; (element-type commit-timestamp) */
  gsize n_refs;
  g_autofree char **override_commit_ids = NULL;
  g_autoptr(GPtrArray) remotes_to_remove = NULL;  /* (element-type OstreeRemote) */
  g_autoptr(GPtrArray) final_results = NULL;  /* (element-type OstreeRepoFinderResult) */

  task = G_TASK (user_data);
  self = OSTREE_REPO (g_task_get_source_object (task));
  cancellable = g_task_get_cancellable (task);
  data = g_task_get_task_data (task);

  refs = (const OstreeCollectionRef * const *) data->refs;
  /* progress = data->progress; */

  /* Finish finding the remotes. */
  results = ostree_repo_finder_resolve_all_finish (async_result, &error);

  if (results == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (results->len == 0)
    {
      g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
      return;
    }

  /* Throughout this function, we eliminate invalid results from @results by
   * clearing them to %NULL. We cannot remove them from the array, as that messes
   * up iteration and stored array indices. Accordingly, we need the free function
   * to be %NULL-safe. */
  g_ptr_array_set_free_func (results, (GDestroyNotify) repo_finder_result_free0);

  if (data->options)
    {
      (void) g_variant_lookup (data->options, "override-commit-ids", "^a&s", &override_commit_ids);
    }

  /* FIXME: In future, we also want to pull static delta superblocks in this
   * phase, so that we have all the metadata we need for accurate size
   * estimation for the actual pull operation. This should check the
   * disable-static-deltas option first. */

  /* Each key must be a pointer to the #CommitMetadata.checksum field of its value. */
  commit_metadatas = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) commit_metadata_free);

  /* X dimension is an index into @refs. Y dimension is an index into @results.
   * Each cell stores the commit checksum which that ref resolves to on that
   * remote, or %NULL if the remote doesn’t have that ref. */
  n_refs = g_strv_length ((gchar **) refs);  /* it’s not a GStrv, but this works */
  refs_and_remotes_table = pointer_table_new (n_refs, results->len);
  remotes_to_remove = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_remote_unref);

  /* Fetch and validate the summary file for each result. */
  /* FIXME: All these downloads could be parallelised; that requires the
   * ostree_repo_remote_fetch_summary_with_options() API to be async. */
  for (i = 0; i < results->len; i++)
    {
      OstreeRepoFinderResult *result = g_ptr_array_index (results, i);
      g_autoptr(GBytes) summary_bytes = NULL;
      g_autoptr(GVariant) summary_v = NULL;
      guint64 summary_last_modified;
      g_autoptr(GVariant) summary_refs = NULL;
      g_autoptr(GVariant) additional_metadata_v = NULL;
      g_autofree gchar *summary_collection_id = NULL;
      g_autoptr(GVariantIter) summary_collection_map = NULL;
      gboolean invalid_result = FALSE;

      /* Add the remote to our internal list of remotes, so other libostree
       * API can access it. */
      if (!_ostree_repo_add_remote (self, result->remote))
        g_ptr_array_add (remotes_to_remove, ostree_remote_ref (result->remote));

      g_debug ("%s: Fetching summary for remote ‘%s’ with keyring ‘%s’.",
               G_STRFUNC, result->remote->name, result->remote->keyring);

      /* Download the summary. This will load from the cache if possible. */
      ostree_repo_remote_fetch_summary_with_options (self,
                                                     result->remote->name,
                                                     NULL,  /* no options */
                                                     &summary_bytes,
                                                     NULL,
                                                     cancellable,
                                                     &error);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        goto error;
      else if (error != NULL)
        {
          g_debug ("%s: Failed to download summary for result ‘%s’. Ignoring. %s",
                   G_STRFUNC, result->remote->name, error->message);
          g_clear_pointer (&g_ptr_array_index (results, i), ostree_repo_finder_result_free);
          g_clear_error (&error);
          continue;
        }
      else if (summary_bytes == NULL)
        {
          g_debug ("%s: Failed to download summary for result ‘%s’. Ignoring. %s",
                   G_STRFUNC, result->remote->name,
                   "No summary file exists on server");
          g_clear_pointer (&g_ptr_array_index (results, i), ostree_repo_finder_result_free);
          continue;
        }

      /* Check the metadata in the summary file, especially whether it contains
       * all the @refs we are interested in. */
      summary_v = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                                summary_bytes, FALSE));

      /* Check the summary’s additional metadata and set up @commit_metadata
       * and @refs_and_remotes_table with the refs listed in the summary file,
       * filtered by the keyring associated with this result and the
       * intersection with @refs. */
      additional_metadata_v = g_variant_get_child_value (summary_v, 1);

      if (g_variant_lookup (additional_metadata_v, OSTREE_SUMMARY_COLLECTION_ID, "s", &summary_collection_id))
        {
          summary_refs = g_variant_get_child_value (summary_v, 0);

          if (!find_remotes_process_refs (self, refs, result, i, summary_collection_id, summary_refs,
                                          commit_metadatas, refs_and_remotes_table))
            {
              g_clear_pointer (&g_ptr_array_index (results, i), ostree_repo_finder_result_free);
              continue;
            }
        }

      if (!g_variant_lookup (additional_metadata_v, OSTREE_SUMMARY_COLLECTION_MAP, "a{sa(s(taya{sv}))}", &summary_collection_map))
        summary_collection_map = NULL;

      while (summary_collection_map != NULL &&
             g_variant_iter_loop (summary_collection_map, "{s@a(s(taya{sv}))}", &summary_collection_id, &summary_refs))
        {
          /* Exclude refs that don't use the associated keyring if this is a
           * dynamic remote, by comparing against the collection ID of the
           * remote this one inherits from */
          if (result->remote->refspec_name != NULL &&
              !check_remote_matches_collection_id (self, result->remote->refspec_name, summary_collection_id))
            continue;

          if (!find_remotes_process_refs (self, refs, result, i, summary_collection_id, summary_refs,
                                          commit_metadatas, refs_and_remotes_table))
            {
              g_clear_pointer (&g_ptr_array_index (results, i), ostree_repo_finder_result_free);
              invalid_result = TRUE;
              break;
            }
        }

      if (invalid_result)
        continue;

      /* Check the summary timestamp. */
      if (!g_variant_lookup (additional_metadata_v, OSTREE_SUMMARY_LAST_MODIFIED, "t", &summary_last_modified))
        summary_last_modified = 0;
      else
        summary_last_modified = GUINT64_FROM_BE (summary_last_modified);

      /* Update the stored result data. Clear the @ref_to_checksum map, since
       * it’s been moved to @refs_and_remotes_table and is now potentially out
       * of date. */
      g_clear_pointer (&result->ref_to_checksum, g_hash_table_unref);
      g_clear_pointer (&result->ref_to_timestamp, g_hash_table_unref);
      result->summary_last_modified = summary_last_modified;
    }

  /* Fill in any gaps in the metadata for the most recent commits by pulling
   * the commit metadata from the remotes. The ‘most recent commits’ are the
   * set of head commits pointed to by the refs we just resolved from the
   * summary files. */
  GLNX_HASH_TABLE_FOREACH_V (commit_metadatas, CommitMetadata*, commit_metadata)
    {
      char buf[_OSTREE_LOOSE_PATH_MAX];
      g_autofree gchar *commit_filename = NULL;
      g_autoptr(GPtrArray) mirrorlist = NULL;  /* (element-type OstreeFetcherURI) */
      g_autoptr(GBytes) commit_bytes = NULL;
      g_autoptr(GVariant) commit_v = NULL;
      guint64 commit_timestamp;
      g_autoptr(GDateTime) dt = NULL;

      /* Already complete? */
      if (commit_metadata->timestamp != 0)
        continue;

      _ostree_loose_path (buf, commit_metadata->checksum, OSTREE_OBJECT_TYPE_COMMIT, OSTREE_REPO_MODE_ARCHIVE);
      commit_filename = g_build_filename ("objects", buf, NULL);

      /* For each of the remotes whose summary files contain this ref, try
       * downloading the commit metadata until we succeed. Since the results are
       * in priority order, the most important remotes are tried first. */
      for (i = 0; i < commit_metadata->refs->len; i++)
        {
          gsize ref_index = g_array_index (commit_metadata->refs, gsize, i);
          gsize j;

          for (j = 0; j < results->len; j++)
            {
              OstreeRepoFinderResult *result = g_ptr_array_index (results, j);

              /* Previous error processing this result? */
              if (result == NULL)
                continue;

              if (pointer_table_get (refs_and_remotes_table, ref_index, j) != commit_metadata->checksum)
                continue;

              g_autofree gchar *uri = NULL;
              g_autoptr(OstreeFetcherURI) fetcher_uri = NULL;

              if (!ostree_repo_remote_get_url (self, result->remote->name,
                                               &uri, &error))
                goto error;

              fetcher_uri = _ostree_fetcher_uri_parse (uri, &error);
              if (fetcher_uri == NULL)
                goto error;

              fetcher = _ostree_repo_remote_new_fetcher (self, result->remote->name,
                                                         TRUE, NULL, NULL, NULL, &error);
              if (fetcher == NULL)
                goto error;

              g_debug ("%s: Fetching metadata for commit ‘%s’ from remote ‘%s’.",
                       G_STRFUNC, commit_metadata->checksum, result->remote->name);

              /* FIXME: Support remotes which have contenturl, mirrorlist, etc. */
              mirrorlist = g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
              g_ptr_array_add (mirrorlist, g_steal_pointer (&fetcher_uri));

              if (!_ostree_fetcher_mirrored_request_to_membuf (fetcher,
                                                               mirrorlist,
                                                               commit_filename,
                                                               OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT,
                                                               NULL, 0,
                                                               data->n_network_retries,
                                                               &commit_bytes,
                                                               NULL, NULL, NULL,
                                                               0,  /* no maximum size */
                                                               cancellable,
                                                               &error))
                goto error;

              g_autoptr(OstreeGpgVerifyResult) verify_result = NULL;

              verify_result = ostree_repo_verify_commit_for_remote (self,
                                                                    commit_metadata->checksum,
                                                                    result->remote->name,
                                                                    cancellable,
                                                                    &error);
              if (verify_result == NULL)
                {
                  g_prefix_error (&error, "Commit %s: ", commit_metadata->checksum);
                  goto error;
                }

              if (!ostree_gpg_verify_result_require_valid_signature (verify_result, &error))
                {
                  g_prefix_error (&error, "Commit %s: ", commit_metadata->checksum);
                  goto error;
                }

              if (commit_bytes != NULL)
                break;
            }

          if (commit_bytes != NULL)
            break;
        }

      if (commit_bytes == NULL)
        {
          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Metadata not found for commit ‘%s’", commit_metadata->checksum);
          goto error;
        }

      /* Parse the commit metadata. */
      commit_v = g_variant_new_from_bytes (OSTREE_COMMIT_GVARIANT_FORMAT,
                                           commit_bytes, FALSE);
      g_variant_get_child (commit_v, 5, "t", &commit_timestamp);
      commit_timestamp = GUINT64_FROM_BE (commit_timestamp);
      dt = g_date_time_new_from_unix_utc (commit_timestamp);

      if (dt == NULL)
        {
          g_debug ("%s: Commit ‘%s’ metadata contained timestamp %" G_GUINT64_FORMAT " which is too far in the future. Resetting to 0.",
                   G_STRFUNC, commit_metadata->checksum, commit_timestamp);
          commit_timestamp = 0;
        }

      /* Update the #CommitMetadata. */
      commit_metadata->timestamp = commit_timestamp;
    }

  /* Find the latest commit for each ref. This is where we resolve the
   * differences between remotes: two remotes could both contain ref R, but one
   * remote could be outdated compared to the other, and point to an older
   * commit. For each ref, we want to find the most recent commit any remote
   * points to for it (unless override-commit-ids was used).
   *
   * @ref_to_latest_commit is indexed by @ref_index, and its values are the
   * latest checksum for each ref. If override-commit-ids was used,
   * @ref_to_latest_commit won't be initialized or used.
   *
   * @ref_to_latest_timestamp is also indexed by @ref_index, and its values are
   * the latest timestamp for each ref, when available.*/
  ref_to_latest_commit = g_new0 (const gchar *, n_refs);
  ref_to_latest_timestamp = g_new0 (guint64, n_refs);

  for (i = 0; i < n_refs; i++)
    {
      gsize j;
      const gchar *latest_checksum = NULL;
      const CommitMetadata *latest_commit_metadata = NULL;
      g_autofree gchar *latest_commit_timestamp_str = NULL;

      if (override_commit_ids)
        {
          g_debug ("%s: Using specified commit ‘%s’ for ref (%s, %s).",
                   G_STRFUNC, override_commit_ids[i], refs[i]->collection_id, refs[i]->ref_name);
          continue;
        }

      for (j = 0; j < results->len; j++)
        {
          const CommitMetadata *candidate_commit_metadata;
          const gchar *candidate_checksum;

          candidate_checksum = pointer_table_get (refs_and_remotes_table, i, j);

          if (candidate_checksum == NULL)
            continue;

          candidate_commit_metadata = g_hash_table_lookup (commit_metadatas, candidate_checksum);
          g_assert (candidate_commit_metadata != NULL);

          if (latest_commit_metadata == NULL ||
              candidate_commit_metadata->timestamp > latest_commit_metadata->timestamp)
            {
              latest_checksum = candidate_checksum;
              latest_commit_metadata = candidate_commit_metadata;
            }
        }

      /* @latest_checksum could be %NULL here if there was an error downloading
       * the summary or commit metadata files above. */
      ref_to_latest_commit[i] = latest_checksum;

      if (latest_checksum != NULL && latest_commit_metadata != NULL)
        ref_to_latest_timestamp[i] = latest_commit_metadata->timestamp;
      else
        ref_to_latest_timestamp[i] = 0;

      if (latest_commit_metadata != NULL)
        {
          latest_commit_timestamp_str = uint64_secs_to_iso8601 (latest_commit_metadata->timestamp);
          g_debug ("%s: Latest commit for ref (%s, %s) across all remotes is ‘%s’ with timestamp %s.",
                   G_STRFUNC, refs[i]->collection_id, refs[i]->ref_name,
                   latest_checksum, latest_commit_timestamp_str);
        }
      else
        {
          g_debug ("%s: Latest commit for ref (%s, %s) is unknown due to failure to download metadata.",
                   G_STRFUNC, refs[i]->collection_id, refs[i]->ref_name);
        }
    }

  /* Recombine @commit_metadatas and @results so that each
   * #OstreeRepoFinderResult.refs lists the refs for which that remote has the
   * latest commits (i.e. it’s not out of date compared to some other remote). */
  final_results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);

  for (i = 0; i < results->len; i++)
    {
      OstreeRepoFinderResult *result = g_ptr_array_index (results, i);
      g_autoptr(GHashTable) validated_ref_to_checksum = NULL;  /* (element-type OstreeCollectionRef utf8) */
      g_autoptr(GHashTable) validated_ref_to_timestamp = NULL;  /* (element-type OstreeCollectionRef guint64) */
      gsize j, n_latest_refs;

      /* Previous error processing this result? */
      if (result == NULL)
        continue;

      /* Map of refs to checksums provided by this result. The checksums should
       * be %NULL for each ref unless this result provides the latest checksum. */
      validated_ref_to_checksum = g_hash_table_new_full (ostree_collection_ref_hash,
                                                         ostree_collection_ref_equal,
                                                         (GDestroyNotify) ostree_collection_ref_free,
                                                         g_free);

      validated_ref_to_timestamp = g_hash_table_new_full (ostree_collection_ref_hash,
                                                          ostree_collection_ref_equal,
                                                          (GDestroyNotify) ostree_collection_ref_free,
                                                          g_free);
      if (override_commit_ids)
        {
          for (j = 0; refs[j] != NULL; j++)
            {
              guint64 *timestamp_ptr;

              g_hash_table_insert (validated_ref_to_checksum, ostree_collection_ref_dup (refs[j]),
                                   g_strdup (override_commit_ids[j]));

              timestamp_ptr = g_malloc (sizeof (guint64));
              *timestamp_ptr = 0;
              g_hash_table_insert (validated_ref_to_timestamp, ostree_collection_ref_dup (refs[j]),
                                   timestamp_ptr);
            }
        }
      else
        {
          n_latest_refs = 0;

          for (j = 0; refs[j] != NULL; j++)
            {
              const gchar *latest_commit_for_ref = ref_to_latest_commit[j];
              guint64 *timestamp_ptr;

              if (pointer_table_get (refs_and_remotes_table, j, i) != latest_commit_for_ref)
                latest_commit_for_ref = NULL;
              if (latest_commit_for_ref != NULL)
                n_latest_refs++;

              g_hash_table_insert (validated_ref_to_checksum, ostree_collection_ref_dup (refs[j]),
                                   g_strdup (latest_commit_for_ref));

              timestamp_ptr = g_malloc (sizeof (guint64));
              if (latest_commit_for_ref != NULL)
                *timestamp_ptr = GUINT64_TO_BE (ref_to_latest_timestamp[j]);
              else
                *timestamp_ptr = 0;
              g_hash_table_insert (validated_ref_to_timestamp, ostree_collection_ref_dup (refs[j]),
                                   timestamp_ptr);
            }

          if (n_latest_refs == 0)
            {
              g_debug ("%s: Omitting remote ‘%s’ from results as none of its refs are new enough.",
                       G_STRFUNC, result->remote->name);
              ostree_repo_finder_result_free (g_steal_pointer (&g_ptr_array_index (results, i)));
              continue;
            }
        }

      result->ref_to_checksum = g_steal_pointer (&validated_ref_to_checksum);
      result->ref_to_timestamp = g_steal_pointer (&validated_ref_to_timestamp);
      g_ptr_array_add (final_results, g_steal_pointer (&g_ptr_array_index (results, i)));
    }

  /* Ensure the updated results are still in priority order. */
  g_ptr_array_sort (final_results, sort_results_cb);

  /* Remove the remotes we temporarily added.
   * FIXME: It would be so much better if we could pass #OstreeRemote pointers
   * around internally, to avoid serialising on the global table of them. */
  for (i = 0; i < remotes_to_remove->len; i++)
    {
      OstreeRemote *remote = g_ptr_array_index (remotes_to_remove, i);
      _ostree_repo_remove_remote (self, remote);
    }

  g_task_return_pointer (task, g_steal_pointer (&final_results), (GDestroyNotify) g_ptr_array_unref);

  return;

error:
  /* Remove the remotes we temporarily added. */
  for (i = 0; i < remotes_to_remove->len; i++)
    {
      OstreeRemote *remote = g_ptr_array_index (remotes_to_remove, i);
      _ostree_repo_remove_remote (self, remote);
    }

  g_task_return_error (task, g_steal_pointer (&error));
}

/**
 * ostree_repo_find_remotes_finish:
 * @self: an #OstreeRepo
 * @result: the asynchronous result
 * @error: return location for a #GError, or %NULL
 *
 * Finish an asynchronous pull operation started with
 * ostree_repo_find_remotes_async().
 *
 * Returns: (transfer full) (array zero-terminated=1): a potentially empty array
 *    of #OstreeRepoFinderResults, followed by a %NULL terminator element; or
 *    %NULL on error
 * Since: 2018.6
 */
OstreeRepoFinderResult **
ostree_repo_find_remotes_finish (OstreeRepo    *self,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_autoptr(GPtrArray) results = NULL;

  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, ostree_repo_find_remotes_async), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  results = g_task_propagate_pointer (G_TASK (result), error);

  if (results != NULL)
    {
      g_ptr_array_add (results, NULL);  /* NULL terminator */
      return (OstreeRepoFinderResult **) g_ptr_array_free (g_steal_pointer (&results), FALSE);
    }
  else
    return NULL;
}

static void
copy_option (GVariantDict       *master_options,
             GVariantDict       *slave_options,
             const gchar        *key,
             const GVariantType *expected_type)
{
  g_autoptr(GVariant) option_v = g_variant_dict_lookup_value (master_options, key, expected_type);
  if (option_v != NULL)
    g_variant_dict_insert_value (slave_options, key, option_v);
}

/**
 * ostree_repo_pull_from_remotes_async:
 * @self: an #OstreeRepo
 * @results: (array zero-terminated=1): %NULL-terminated array of remotes to
 *    pull from, including the refs to pull from each
 * @options: (nullable): A GVariant `a{sv}` with an extensible set of flags
 * @progress: (nullable): an #OstreeAsyncProgress to update with the operation’s
 *    progress, or %NULL
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: asynchronous completion callback
 * @user_data: data to pass to @callback
 *
 * Pull refs from multiple remotes which have been found using
 * ostree_repo_find_remotes_async().
 *
 * @results are expected to be in priority order, with the best remotes to pull
 * from listed first. ostree_repo_pull_from_remotes_async() will generally pull
 * from the remotes in order, but may parallelise its downloads.
 *
 * If an error is encountered when pulling from a given remote, that remote will
 * be ignored and another will be tried instead. If any refs have not been
 * downloaded successfully after all remotes have been tried, %G_IO_ERROR_FAILED
 * will be returned. The results of any successful downloads will remain cached
 * in the local repository.
 *
 * If @cancellable is cancelled, %G_IO_ERROR_CANCELLED will be returned
 * immediately. The results of any successfully completed downloads at that
 * point will remain cached in the local repository.
 *
 * GPG verification of commits will be used unconditionally.
 *
 * The following @options are currently defined:
 *
 *   * `flags` (`i`): #OstreeRepoPullFlags to apply to the pull operation
 *   * `inherit-transaction` (`b`): %TRUE to inherit an ongoing transaction on
 *     the #OstreeRepo, rather than encapsulating the pull in a new one
 *   * `depth` (`i`): How far in the history to traverse; default is 0, -1 means infinite
 *   * `disable-static-deltas` (`b`): Do not use static deltas
 *   * `http-headers` (`a(ss)`): Additional headers to add to all HTTP requests
 *   * `subdirs` (`as`): Pull just these subdirectories
 *   * `update-frequency` (`u`): Frequency to call the async progress callback in
 *     milliseconds, if any; only values higher than 0 are valid
 *   * `append-user-agent` (`s`): Additional string to append to the user agent
 *   * `n-network-retries` (`u`): Number of times to retry each download on receiving
 *     a transient network error, such as a socket timeout; default is 5, 0
 *     means return errors without retrying. Since: 2018.6
 *   * `ref-keyring-map` (`a(sss)`): Array of (collection ID, ref name, keyring
 *     remote name) tuples specifying which remote's keyring should be used when
 *     doing GPG verification of each collection-ref. This is useful to prevent a
 *     remote from serving malicious updates to refs which did not originate from
 *     it. This can be a subset or superset of the refs being pulled; any ref
 *     not being pulled will be ignored and any ref without a keyring remote
 *     will be verified with the keyring of the remote being pulled from.
 *     Since: 2019.2
 *
 * Since: 2018.6
 */
void
ostree_repo_pull_from_remotes_async (OstreeRepo                           *self,
                                     const OstreeRepoFinderResult * const *results,
                                     GVariant                             *options,
                                     OstreeAsyncProgress                  *progress,
                                     GCancellable                         *cancellable,
                                     GAsyncReadyCallback                   callback,
                                     gpointer                              user_data)
{
  g_return_if_fail (OSTREE_IS_REPO (self));
  g_return_if_fail (results != NULL && results[0] != NULL);
  g_return_if_fail (options == NULL || g_variant_is_of_type (options, G_VARIANT_TYPE ("a{sv}")));
  g_return_if_fail (progress == NULL || OSTREE_IS_ASYNC_PROGRESS (progress));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  g_autoptr(GTask) task = NULL;
  g_autoptr(GHashTable) refs_pulled = NULL;  /* (element-type OstreeCollectionRef gboolean) */
  gsize i, j;
  g_autoptr(GString) refs_unpulled_string = NULL;
  g_autoptr(GError) local_error = NULL;
  g_auto(GVariantDict) options_dict = OT_VARIANT_BUILDER_INITIALIZER;
  OstreeRepoPullFlags flags;
  gboolean inherit_transaction;

  /* Set up a task for the whole operation. */
  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_pull_from_remotes_async);

  /* Keep track of the set of refs we’ve pulled already. Value is %TRUE if the
   * ref has been pulled; %FALSE if it has not. */
  refs_pulled = g_hash_table_new_full (ostree_collection_ref_hash,
                                       ostree_collection_ref_equal, NULL, NULL);

  g_variant_dict_init (&options_dict, options);

  if (!g_variant_dict_lookup (&options_dict, "flags", "i", &flags))
    flags = OSTREE_REPO_PULL_FLAGS_NONE;
  if (!g_variant_dict_lookup (&options_dict, "inherit-transaction", "b", &inherit_transaction))
    inherit_transaction = FALSE;

  /* Run all the local pull operations in a single overall transaction. */
  if (!inherit_transaction &&
      !ostree_repo_prepare_transaction (self, NULL, cancellable, &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  /* FIXME: Rework this code to pull in parallel where possible. At the moment
   * we expect the (i == 0) iteration will do all the work (all the refs) and
   * subsequent iterations are only there in case of error.
   *
   * The code is currently all synchronous, too. Making it asynchronous requires
   * the underlying pull code to be asynchronous. */
  for (i = 0; results[i] != NULL; i++)
    {
      const OstreeRepoFinderResult *result = results[i];

      g_autoptr(GString) refs_to_pull_str = NULL;
      g_autoptr(GPtrArray) refs_to_pull = NULL;  /* (element-type OstreeCollectionRef) */
      g_auto(GVariantBuilder) refs_to_pull_builder = OT_VARIANT_BUILDER_INITIALIZER;
      g_auto(GVariantDict) local_options_dict = OT_VARIANT_BUILDER_INITIALIZER;
      g_autoptr(GVariant) local_options = NULL;
      gboolean remove_remote;

      refs_to_pull = g_ptr_array_new_with_free_func (NULL);
      refs_to_pull_str = g_string_new ("");
      g_variant_builder_init (&refs_to_pull_builder, G_VARIANT_TYPE ("a(sss)"));

      GLNX_HASH_TABLE_FOREACH_KV (result->ref_to_checksum, const OstreeCollectionRef*, ref,
                                                           const char*, checksum)
        {
          if (checksum != NULL &&
              !GPOINTER_TO_INT (g_hash_table_lookup (refs_pulled, ref)))
            {
              g_ptr_array_add (refs_to_pull, (gpointer) ref);
              g_variant_builder_add (&refs_to_pull_builder, "(sss)",
                                     ref->collection_id, ref->ref_name, checksum);

              if (refs_to_pull_str->len > 0)
                g_string_append (refs_to_pull_str, ", ");
              g_string_append_printf (refs_to_pull_str, "(%s, %s)",
                                      ref->collection_id, ref->ref_name);
            }
        }

      if (refs_to_pull->len == 0)
        {
          g_debug ("Ignoring remote ‘%s’ as it has no relevant refs or they "
                   "have already been pulled.",
                   result->remote->name);
          continue;
        }

      /* NULL terminators. */
      g_ptr_array_add (refs_to_pull, NULL);

      g_debug ("Pulling from remote ‘%s’: %s",
               result->remote->name, refs_to_pull_str->str);

      /* Set up the pull options. */
      g_variant_dict_init (&local_options_dict, NULL);

      g_variant_dict_insert (&local_options_dict, "flags", "i", OSTREE_REPO_PULL_FLAGS_UNTRUSTED | flags);
      g_variant_dict_insert_value (&local_options_dict, "collection-refs", g_variant_builder_end (&refs_to_pull_builder));
#ifndef OSTREE_DISABLE_GPGME
      g_variant_dict_insert (&local_options_dict, "gpg-verify", "b", TRUE);
#else
      g_variant_dict_insert (&local_options_dict, "gpg-verify", "b", FALSE);
#endif /* OSTREE_DISABLE_GPGME */
      g_variant_dict_insert (&local_options_dict, "gpg-verify-summary", "b", FALSE);
      g_variant_dict_insert (&local_options_dict, "sign-verify", "b", FALSE);
      g_variant_dict_insert (&local_options_dict, "sign-verify-summary", "b", FALSE);
      g_variant_dict_insert (&local_options_dict, "inherit-transaction", "b", TRUE);
      if (result->remote->refspec_name != NULL)
        g_variant_dict_insert (&local_options_dict, "override-remote-name", "s", result->remote->refspec_name);
      copy_option (&options_dict, &local_options_dict, "depth", G_VARIANT_TYPE ("i"));
      copy_option (&options_dict, &local_options_dict, "disable-static-deltas", G_VARIANT_TYPE ("b"));
      copy_option (&options_dict, &local_options_dict, "http-headers", G_VARIANT_TYPE ("a(ss)"));
      copy_option (&options_dict, &local_options_dict, "subdirs", G_VARIANT_TYPE ("as"));
      copy_option (&options_dict, &local_options_dict, "update-frequency", G_VARIANT_TYPE ("u"));
      copy_option (&options_dict, &local_options_dict, "append-user-agent", G_VARIANT_TYPE ("s"));
      copy_option (&options_dict, &local_options_dict, "n-network-retries", G_VARIANT_TYPE ("u"));
      copy_option (&options_dict, &local_options_dict, "ref-keyring-map", G_VARIANT_TYPE ("a(sss)"));

      local_options = g_variant_dict_end (&local_options_dict);

      /* FIXME: We do nothing useful with @progress at the moment. */
      remove_remote = !_ostree_repo_add_remote (self, result->remote);
      ostree_repo_pull_with_options (self, result->remote->name, local_options,
                                     progress, cancellable, &local_error);
      if (remove_remote)
        _ostree_repo_remove_remote (self, result->remote);

      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          if (!inherit_transaction)
            ostree_repo_abort_transaction (self, NULL, NULL);
          g_task_return_error (task, g_steal_pointer (&local_error));
          return;
        }

      for (j = 0; refs_to_pull->pdata[j] != NULL; j++)
        g_hash_table_replace (refs_pulled, refs_to_pull->pdata[j],
                              GINT_TO_POINTER (local_error == NULL));

      if (local_error != NULL)
        {
          g_debug ("Failed to pull refs from ‘%s’: %s",
                   result->remote->name, local_error->message);
          g_clear_error (&local_error);
          continue;
        }
      else
        {
          g_debug ("Pulled refs from ‘%s’.", result->remote->name);
        }
    }

  /* Commit the transaction. */
  if (!inherit_transaction &&
      !ostree_repo_commit_transaction (self, NULL, cancellable, &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  /* Any refs left un-downloaded? If so, we’ve failed. */
  GLNX_HASH_TABLE_FOREACH_KV (refs_pulled, const OstreeCollectionRef*, ref,
                                           gpointer, is_pulled_pointer)
    {
      gboolean is_pulled = GPOINTER_TO_INT (is_pulled_pointer);

      if (is_pulled)
        continue;

      if (refs_unpulled_string == NULL)
        refs_unpulled_string = g_string_new ("");
      else
        g_string_append (refs_unpulled_string, ", ");

      g_string_append_printf (refs_unpulled_string, "(%s, %s)",
                              ref->collection_id, ref->ref_name);
    }

  if (refs_unpulled_string != NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to pull some refs from the remotes: %s",
                               refs_unpulled_string->str);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

/**
 * ostree_repo_pull_from_remotes_finish:
 * @self: an #OstreeRepo
 * @result: the asynchronous result
 * @error: return location for a #GError, or %NULL
 *
 * Finish an asynchronous pull operation started with
 * ostree_repo_pull_from_remotes_async().
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: 2018.6
 */
gboolean
ostree_repo_pull_from_remotes_finish (OstreeRepo    *self,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, ostree_repo_pull_from_remotes_async), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * ostree_repo_remote_fetch_summary_with_options:
 * @self: Self
 * @name: name of a remote
 * @options: (nullable): A GVariant a{sv} with an extensible set of flags
 * @out_summary: (out) (optional): return location for raw summary data, or
 *               %NULL
 * @out_signatures: (out) (optional): return location for raw summary
 *                  signature data, or %NULL
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Like ostree_repo_remote_fetch_summary(), but supports an extensible set of flags.
 * The following are currently defined:
 *
 * - override-url (s): Fetch summary from this URL if remote specifies no metalink in options
 * - http-headers (a(ss)): Additional headers to add to all HTTP requests
 * - append-user-agent (s): Additional string to append to the user agent
 * - n-network-retries (u): Number of times to retry each download on receiving
 *   a transient network error, such as a socket timeout; default is 5, 0
 *   means return errors without retrying
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 2016.6
 */
gboolean
ostree_repo_remote_fetch_summary_with_options (OstreeRepo    *self,
                                               const char    *name,
                                               GVariant      *options,
                                               GBytes       **out_summary,
                                               GBytes       **out_signatures,
                                               GCancellable  *cancellable,
                                               GError       **error)
{
  g_autofree char *metalink_url_string = NULL;
  g_autoptr(GBytes) summary = NULL;
  g_autoptr(GBytes) signatures = NULL;
  gboolean gpg_verify_summary;
  g_autoptr(GPtrArray) signapi_summary_verifiers = NULL;
  gboolean summary_is_from_cache = FALSE;
  g_autoptr(OstreeFetcher) fetcher = NULL;
  g_autoptr(GMainContextPopDefault) mainctx = NULL;
  const char *url_override = NULL;
  g_autoptr(GVariant) extra_headers = NULL;
  g_autoptr(GPtrArray) mirrorlist = NULL;
  const char *append_user_agent = NULL;
  guint n_network_retries = DEFAULT_N_NETWORK_RETRIES;
  gboolean summary_sig_not_modified = FALSE;
  g_autofree char *summary_sig_if_none_match = NULL;
  g_autofree char *summary_sig_etag = NULL;
  gboolean summary_not_modified = FALSE;
  g_autofree char *summary_if_none_match = NULL;
  g_autofree char *summary_etag = NULL;
  guint64 summary_sig_if_modified_since = 0;
  guint64 summary_sig_last_modified = 0;
  guint64 summary_if_modified_since = 0;
  guint64 summary_last_modified = 0;

  g_return_val_if_fail (OSTREE_REPO (self), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  if (!ostree_repo_get_remote_option (self, name, "metalink", NULL,
                                      &metalink_url_string, error))
    return FALSE;

  if (options)
    {
      (void) g_variant_lookup (options, "override-url", "&s", &url_override);
      (void) g_variant_lookup (options, "http-headers", "@a(ss)", &extra_headers);
      (void) g_variant_lookup (options, "append-user-agent", "&s", &append_user_agent);
      (void) g_variant_lookup (options, "n-network-retries", "u", &n_network_retries);
    }

  if (!ostree_repo_remote_get_gpg_verify_summary (self, name, &gpg_verify_summary, error))
    return FALSE;

  if (!_signapi_init_for_remote (self, name, NULL,
                                 &signapi_summary_verifiers,
                                 error))
    return FALSE;

  mainctx = _ostree_main_context_new_default ();

  fetcher = _ostree_repo_remote_new_fetcher (self, name, TRUE, extra_headers, append_user_agent, NULL, error);
  if (fetcher == NULL)
    return FALSE;

  if (metalink_url_string)
    {
      g_autoptr(OstreeFetcherURI) uri = _ostree_fetcher_uri_parse (metalink_url_string, error);
      if (!uri)
        return FALSE;

      mirrorlist =
        g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
      g_ptr_array_add (mirrorlist, g_steal_pointer (&uri));
    }
  else if (!compute_effective_mirrorlist (self, name, url_override,
                                          fetcher, n_network_retries,
                                          &mirrorlist, cancellable, error))
    return FALSE;

  /* Send the ETag from the cache with the request for summary.sig to
   * avoid downloading summary.sig unnecessarily. This won’t normally provide
   * much benefit since summary.sig is typically 590B in size (vs a 0B HTTP 304
   * response). But if a repository has multiple keys, the signature file will
   * grow and this optimisation may be useful. */
  _ostree_repo_load_cache_summary_properties (self, name, ".sig",
                                              &summary_sig_if_none_match, &summary_sig_if_modified_since);
  _ostree_repo_load_cache_summary_properties (self, name, NULL,
                                              &summary_if_none_match, &summary_if_modified_since);

  if (!_ostree_preload_metadata_file (self,
                                      fetcher,
                                      mirrorlist,
                                      "summary.sig",
                                      metalink_url_string ? TRUE : FALSE,
                                      summary_sig_if_none_match, summary_sig_if_modified_since,
                                      n_network_retries,
                                      &signatures,
                                      &summary_sig_not_modified, &summary_sig_etag, &summary_sig_last_modified,
                                      cancellable,
                                      error))
    return FALSE;

  /* The server returned HTTP status 304 Not Modified, so we’re clear to
   * load summary.sig from the cache. Also load summary, since
   * `_ostree_repo_load_cache_summary_if_same_sig()` would just do that anyway. */
  if (summary_sig_not_modified)
    {
      g_clear_pointer (&signatures, g_bytes_unref);
      g_clear_pointer (&summary, g_bytes_unref);
      if (!_ostree_repo_load_cache_summary_file (self, name, ".sig",
                                                 &signatures,
                                                 cancellable, error))
        return FALSE;

      if (!summary &&
          !_ostree_repo_load_cache_summary_file (self, name, NULL,
                                                 &summary,
                                                 cancellable, error))
        return FALSE;
    }

  if (signatures && !summary)
    {
      if (!_ostree_repo_load_cache_summary_if_same_sig (self,
                                                        name,
                                                        signatures,
                                                        &summary,
                                                        cancellable,
                                                        error))
        return FALSE;
    }

  if (summary)
    summary_is_from_cache = TRUE;
  else
    {
      if (!_ostree_preload_metadata_file (self,
                                          fetcher,
                                          mirrorlist,
                                          "summary",
                                          metalink_url_string ? TRUE : FALSE,
                                          summary_if_none_match, summary_if_modified_since,
                                          n_network_retries,
                                          &summary,
                                          &summary_not_modified, &summary_etag, &summary_last_modified,
                                          cancellable,
                                          error))
        return FALSE;

      /* The server returned HTTP status 304 Not Modified, so we’re clear to
       * load summary.sig from the cache. Also load summary, since
       * `_ostree_repo_load_cache_summary_if_same_sig()` would just do that anyway. */
      if (summary_not_modified)
        {
          g_clear_pointer (&summary, g_bytes_unref);
          if (!_ostree_repo_load_cache_summary_file (self, name, NULL,
                                                     &summary,
                                                     cancellable, error))
            return FALSE;
        }
    }

  if (!_ostree_repo_verify_summary (self, name,
                                    gpg_verify_summary, signapi_summary_verifiers,
                                    summary, signatures,
                                    cancellable, error))
      return FALSE;

  if (!summary_is_from_cache && summary && signatures)
    {
      g_autoptr(GError) temp_error = NULL;

      if (!_ostree_repo_cache_summary (self,
                                       name,
                                       summary,
                                       summary_etag, summary_last_modified,
                                       signatures,
                                       summary_sig_etag, summary_sig_last_modified,
                                       cancellable,
                                       &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
            g_debug ("No permissions to save summary cache");
          else
            {
              g_propagate_error (error, g_steal_pointer (&temp_error));
              return FALSE;
            }
        }
    }

  if (out_summary != NULL)
    *out_summary = g_steal_pointer (&summary);

  if (out_signatures != NULL)
    *out_signatures = g_steal_pointer (&signatures);

  return TRUE;
}

#else /* HAVE_LIBCURL_OR_LIBSOUP */

gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name_or_baseurl,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup or libcurl, and cannot fetch over HTTP");
  return FALSE;
}

gboolean
ostree_repo_remote_fetch_summary_with_options (OstreeRepo    *self,
                                               const char    *name,
                                               GVariant      *options,
                                               GBytes       **out_summary,
                                               GBytes       **out_signatures,
                                               GCancellable  *cancellable,
                                               GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup or libcurl, and cannot fetch over HTTP");
  return FALSE;
}

void
ostree_repo_find_remotes_async (OstreeRepo                        *self,
                                const OstreeCollectionRef * const *refs,
                                GVariant                          *options,
                                OstreeRepoFinder                 **finders,
                                OstreeAsyncProgress               *progress,
                                GCancellable                      *cancellable,
                                GAsyncReadyCallback                callback,
                                gpointer                           user_data)
{
  g_return_if_fail (OSTREE_IS_REPO (self));

  g_task_report_new_error (self, callback, user_data, ostree_repo_find_remotes_async,
                           G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This version of ostree was built without libsoup or libcurl, and cannot fetch over HTTP");
}

OstreeRepoFinderResult **
ostree_repo_find_remotes_finish (OstreeRepo    *self,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_autoptr(GPtrArray) results = NULL;

  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, ostree_repo_find_remotes_async), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  results = g_task_propagate_pointer (G_TASK (result), error);
  g_assert (results == NULL);
  return NULL;
}

void
ostree_repo_pull_from_remotes_async (OstreeRepo                           *self,
                                     const OstreeRepoFinderResult * const *results,
                                     GVariant                             *options,
                                     OstreeAsyncProgress                  *progress,
                                     GCancellable                         *cancellable,
                                     GAsyncReadyCallback                   callback,
                                     gpointer                              user_data)
{
  g_return_if_fail (OSTREE_IS_REPO (self));

  g_task_report_new_error (self, callback, user_data, ostree_repo_find_remotes_async,
                           G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This version of ostree was built without libsoup or libcurl, and cannot fetch over HTTP");
}

gboolean
ostree_repo_pull_from_remotes_finish (OstreeRepo    *self,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  gboolean success;

  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, ostree_repo_pull_from_remotes_async), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  success = g_task_propagate_boolean (G_TASK (result), error);
  g_assert (!success);
  return FALSE;
}

#endif /* HAVE_LIBCURL_OR_LIBSOUP */
