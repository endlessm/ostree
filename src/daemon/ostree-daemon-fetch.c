/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2013 Collabora Ltd.
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
 * Author: Vivek Dasmohapatra <vivek@etla.org>
 */

#include "ostree-daemon-fetch.h"

static void
content_fetch_finished (GObject *object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  OTDOSTree *ostree = OTD_OSTREE (object);
  GTask *task;
  GError *error = NULL;

  gs_unref_object OstreeRepo *repo = OSTREE_REPO (user_data);
  gboolean fetched = FALSE;

  if (!g_task_is_valid (res, object))
    goto invalid_task;

  task = G_TASK (res);
  fetched = g_task_propagate_boolean (task, &error);

  if (error)
    {
      ostree_daemon_set_error (ostree, error);
    }
  else if (!fetched) // bizarre, should not happen
    {
      otd_ostree_set_error_code (ostree, G_IO_ERROR_NOT_FOUND);
      otd_ostree_set_error_message (ostree, "Update not found on server");
      ostree_daemon_set_state (ostree, OTD_STATE_ERROR);
    }
  else
    {
      otd_ostree_set_error_code (ostree, 0);
      otd_ostree_set_error_message (ostree, "");
      ostree_daemon_set_state (ostree, OTD_STATE_UPDATE_READY);
    }

  return;

 invalid_task:
  // Either the threading or the memory management is shafted. Or both.
  // We're boned. Log an error and activate the self destruct mechanism:
  g_error ("Invalid async task object when returning from Poll() thread!");
  g_assert_not_reached ();
}

static void
content_fetch (GTask *task,
               gpointer object,
               gpointer task_data,
               GCancellable *cancel)
{
  OTDOSTree *ostree = OTD_OSTREE (object);
  OstreeRepo *repo = OSTREE_REPO (task_data);
  OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_NONE;
  GError *error = NULL;
  gs_free gchar *src = NULL;
  gs_free gchar *ref = NULL;
  gs_free gchar *sum = NULL;
  gchar *pullrefs[] = { NULL, NULL };
  GMainContext *task_context = g_main_context_new ();

  g_main_context_push_thread_default (task_context);

  if (!ostree_daemon_resolve_upgrade (ostree, repo, &src, &ref, &sum, &error))
    goto error;

  message ("Fetch: %s:%s resolved to: %s", src, ref, sum);
  message ("User asked us for commit: %s", otd_ostree_get_update_id (ostree));

  // rather than re-resolving the update, we get the las ID that the
  // user Poll()ed. We do this because that is the last update for which
  // we had size data: If there's been a new update since, then the
  // system hasn;t seen the download/unpack sizes for that so it cannot
  // be considered to have been approved.
  pullrefs[0] = (gchar *) otd_ostree_get_update_id (ostree);

  // FIXME: upstream ostree_repo_pull had an unbalanced
  // g_main_context_get_thread_default/g_main_context_unref
  // instead of
  // g_main_context_ref_thread_default/g_main_context_unref
  // patch has been accepted upstream, but double check when merging
  if (!ostree_repo_pull (repo, src, pullrefs, flags, cancel, &error))
    goto error;

  message ("Fetch: pull() completed");

  if (!ostree_repo_read_commit (repo, pullrefs[0], NULL, cancel, &error))
    {
      if (!error)
        g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "Failed to fetch update %s from %s", ref, src);
      goto error;
    }

  message ("Fetch: commit %s cached", pullrefs[0]);
  g_task_return_boolean (task, TRUE);
  goto cleanup;

 error:
  message ("Fetch returning ERROR");
  g_task_return_error (task, error);

 cleanup:
  g_main_context_pop_thread_default (task_context);
  g_main_context_unref (task_context);
  return;
}

gboolean
handle_fetch (OTDOSTree             *ostree,
              GDBusMethodInvocation *call,
              gpointer               user_data)
{
  OstreeRepo *repo = OSTREE_REPO (user_data);
  GTask *task = NULL;
  OTDState state = otd_ostree_get_state (ostree);
  const gchar *update_id = otd_ostree_get_update_id (ostree);
  gboolean fetch_ok = FALSE;
  GError *error = NULL;

  switch (state)
    {
    case OTD_STATE_READY:
    case OTD_STATE_ERROR:
    case OTD_STATE_UPDATE_AVAILABLE:
    case OTD_STATE_UPDATE_READY:
    case OTD_STATE_UPDATE_APPLIED:
      fetch_ok = TRUE;
      break;
    case OTD_STATE_POLLING:
      message ("Fetch() called while already polling for an update");
      break;
    case OTD_STATE_FETCHING:
      message ("Fetch() called while already fetching an update");
      break;
    case OTD_STATE_APPLYING_UPDATE:
      message ("Fetch() called while already applying an update");
      break;
    default:
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Impossible state %u (range: %u - %u) when Fetch() called",
                   state, OTD_STATE_MIN, OTD_STATE_MAX);
    }

  if (!ostree_validate_checksum_string (update_id, &error))
    goto out;

  if (!fetch_ok)
    goto out;

  ostree_daemon_set_state (ostree, OTD_STATE_FETCHING);
  task = g_task_new (ostree, NULL, content_fetch_finished, g_object_ref (repo));
  g_task_set_task_data (task, g_object_ref (repo), g_object_unref);
  g_task_run_in_thread (task, content_fetch);

 out:
  if (error)
    {
      ostree_daemon_set_error (ostree, error);
      g_clear_error (&error);
    }
  otd_ostree_complete_fetch (ostree, call);
  return TRUE;
}
