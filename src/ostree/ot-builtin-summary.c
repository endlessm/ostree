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

#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

gboolean opt_detailed;

static GOptionEntry options[] = {
  { "detailed", 'd', 0, G_OPTION_ARG_NONE, &opt_detailed, "Emit detailed summary information", NULL },
  { NULL }
};

gboolean
ostree_builtin_summary (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gsize entries = 0;
  const char *remote = NULL;
  const char *ref = NULL;
  gs_unref_variant GVariant *index = NULL;
  gs_free gchar *refspec = NULL;
  gs_free gchar *revision = NULL;
  gint64 archived = 0;
  gint64 unpacked = 0;
  gsize  fetch_needed = 0;
  gint64 new_archived = 0;
  gint64 new_unpacked = 0;

  context = g_option_context_new("[REMOTE] BRANCH - Display the summary for branch");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ostree_repo_check (repo, error))
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

  // if there's a remote, we might not have pulled the metadata yet:
  if (!ostree_repo_resolve_rev (repo, refspec, remote != NULL, &revision, error))
    goto out;

  // nothing in the cache, but try and fetch it if it's a remote refspec:
  if (!revision && remote)
    {
      OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_METADATA;
      gchar *pullrefs[] = { (gchar *) ref, NULL };

      if (!ostree_repo_pull (repo, remote, pullrefs, flags, cancellable, error))
        goto out;

      if (!ostree_repo_resolve_rev (repo, refspec, FALSE, &revision, error))
        goto out;
    }

  if (!revision)
    {
      if (!*error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "Refspec '%s' not found", refspec);
      goto out;
    }

  // detailed path - got to break the details out by hand:
  // this is mostly to make the tests easier but might also
  // be handy for somebody debugging a messed up size cache:
  if (opt_detailed)
    {
      OstreeRepoCommitSizesIterator *iter =
        ostree_repo_commit_sizes_iterator_new (repo, refspec, cancellable, error);

      if (iter)
        {
          gint64 in, out;
          gchar *csum;
          OstreeObjectType objtype;

          g_print ("details:\n%-64s %16s %16s %16s %s\n",
                   "checksum", "type", "archived", "unpacked", "in-cache");

          while (ostree_repo_commit_sizes_iterator_next (repo, &iter,
                                                         &csum, &objtype,
                                                         &in, &out))
            {
              // we don't use -ve values to mean anything at present
              // but they are possible, ignore for now:
              gint64 asize = MAX (in, 0);
              gint64 usize = MAX (out, 0);
              const gchar *have;
              GError *err = NULL;

              archived += asize;
              unpacked += usize;
              entries++;

              if (asize && usize)
                {
                  gboolean exists;

                  if (ostree_repo_has_object (repo, objtype,
                                              csum, &exists,
                                              cancellable, &err))
                    {
                      if (exists)
                        {
                          have = "Yes";
                        }
                      else
                        {
                          have = "No";
                          new_archived += asize;
                          new_unpacked += usize;
                        }
                    }
                  else
                    {
                      have = err ? err->message : "Unknown Error";
                    }
                }
              else
                {
                  have = "N/a"; // non-file objects aren't tracked in the index
                }

              g_print ("%64s %16d %16ld %16ld %s\n", csum, objtype, asize, usize, have);
              g_clear_error (&err);
            }

          g_print ("details-end\n");
        }
    }
  else
    {
      entries = ostree_repo_get_commit_sizes (repo, refspec,
                                              &new_archived,
                                              &new_unpacked,
                                              &fetch_needed,
                                              &archived, &unpacked,
                                              cancellable, error);
    }

  if (entries > 0)
    {
      g_print ("Summary for refspec %s:\n"
               "  files: %lu/%lu entries\n"
               "  archived: %ld/%ld bytes\n"
               "  unpacked: %ld/%ld bytes\n",
               refspec,
               fetch_needed - entries, entries,
               archived - new_archived, archived,
               unpacked - new_unpacked, unpacked);
    }
  else if (!*error) // No error found, 0 entries => no index available
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Refspec '%s' has no index, cannot summarise", refspec);
      goto out;
    }

 out:
  if (context)
    g_option_context_free (context);

  return entries != 0;
}
