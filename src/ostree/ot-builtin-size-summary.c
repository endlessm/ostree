/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2013 Vivek Dasmohapatra <vivek@collabora.com>
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
 * Author: Vivek Dasmohapatra <vivek@collabora.com>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

gboolean
ostree_builtin_size_summary (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  OstreeRepo *repo;
  gsize entries = 0;
  const char *remote = NULL;
  const char *ref = NULL;
  g_autoptr(GVariant) index = NULL;
  g_autofree gchar *refspec = NULL;
  g_autofree gchar *revision = NULL;
  gint64 archived = 0;
  gint64 unpacked = 0;
  gsize  fetch_needed = 0;
  gint64 new_archived = 0;
  gint64 new_unpacked = 0;

  context = g_option_context_new ("[REMOTE] BRANCH - Display the summary for branch");

  if (!ostree_option_context_parse (context, NULL, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!ostree_repo_open (repo, cancellable, error))
    goto out;

  switch (argc)
    {
    case 3:
      remote = argv[1];
      ref = argv[2];
      refspec = g_strdup_printf ("%s/%s", remote, ref);
      break;

    case 2:
      ref = argv[1];
      refspec = g_strdup (ref);
      break;

    case 1:
      ot_util_usage_error (context, "BRANCH must be specified", error);
      goto out;

    default:
      ot_util_usage_error (context, "only one branch may be summarised", error);
      goto out;
    }

  /* if there's a remote, we might not have pulled the metadata yet */
  if (!ostree_repo_resolve_rev (repo, refspec, remote != NULL, &revision, error))
    goto out;

  /* nothing in the cache, but try and fetch it if it's a remote refspec */
  if (!revision && remote)
    {
      OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY;
      gchar *pullrefs[] = { (gchar *) ref, NULL };

      if (!ostree_repo_pull (repo, remote, pullrefs, flags, NULL, cancellable, error))
        goto out;

      if (!ostree_repo_resolve_rev (repo, refspec, FALSE, &revision, error))
        goto out;
    }

  if (!revision)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Refspec '%s' not found", refspec);
      goto out;
    }

  if (ostree_repo_get_commit_sizes (repo, revision,
                                    &new_archived,
                                    &new_unpacked,
                                    &fetch_needed,
                                    &archived, &unpacked,
                                    &entries,
                                    cancellable, error))
    {
      g_print ("Summary for refspec %s:\n"
               "  files: %" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT " entries\n"
               "  archived: %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "\n"
               "  unpacked: %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "\n",
               refspec,
               entries - fetch_needed, entries,
               archived - new_archived, archived,
               unpacked - new_unpacked, unpacked);
    }

 out:
  g_option_context_free (context);

  return entries != 0;
}
