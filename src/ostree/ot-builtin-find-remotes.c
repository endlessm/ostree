/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

#include "ostree-remote-private.h"

static gchar *opt_cache_dir = NULL;
static gchar *opt_finders = NULL;
static gboolean opt_disable_fsync = FALSE;
static gboolean opt_pull = FALSE;
static gboolean opt_mirror = FALSE;

static GOptionEntry options[] =
  {
    { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_cache_dir, "Use custom cache dir", NULL },
    { "disable-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
    { "finders", 0, 0, G_OPTION_ARG_STRING, &opt_finders, "Use the specified comma separated list of finders (e.g. config,lan,mount)", "FINDERS" },
    { "pull", 0, 0, G_OPTION_ARG_NONE, &opt_pull, "Pull the updates after finding them", NULL },
    { "mirror", 0, 0, G_OPTION_ARG_NONE, &opt_mirror, "Do a mirror pull (see ostree pull --mirror)", NULL},
    { NULL }
  };

static gchar *
uint64_secs_to_iso8601 (guint64 secs)
{
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc (secs);

  if (dt != NULL)
    return g_date_time_format (dt, "%FT%TZ");
  else
    return g_strdup ("invalid");
}

static gchar *
format_ref_to_checksum (GHashTable  *ref_to_checksum  /* (element-type OstreeCollectionRef utf8) */,
                        const gchar *line_prefix)
{
  GHashTableIter iter;
  const OstreeCollectionRef *ref;
  const gchar *checksum;
  g_autoptr(GString) out = NULL;

  g_hash_table_iter_init (&iter, ref_to_checksum);
  out = g_string_new ("");

  while (g_hash_table_iter_next (&iter, (gpointer *) &ref, (gpointer *) &checksum))
    g_string_append_printf (out, "%s - (%s, %s) = %s\n",
                            line_prefix, ref->collection_id, ref->ref_name,
                            (checksum != NULL) ? checksum : "(not found)");

  return g_string_free (g_steal_pointer (&out), FALSE);
}

static gchar *
remote_get_uri (OstreeRemote *remote)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *uri = NULL;

  uri = g_key_file_get_string (remote->options, remote->group, "url", &error);
  g_assert_no_error (error);

  return g_steal_pointer (&uri);
}

/* Add each key from @keys_input to @set iff its value is non-%NULL. */
static void
add_keys_to_set_if_non_null (GHashTable *set,
                             GHashTable *keys_input)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, keys_input);

  while (g_hash_table_iter_next (&iter, &key, &value))
    if (value != NULL)
      g_hash_table_add (set, key);
}

static void
get_result_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  GAsyncResult **result_out = user_data;
  *result_out = g_object_ref (result);
}

static void
collection_ref_free0 (OstreeCollectionRef *ref)
{
  if (ref == NULL)
    return;
  ostree_collection_ref_free (ref);
}

static gboolean
validate_finders_list (const char     **finders,
                       GOptionContext  *context,
                       GError         **error)
{
  typedef struct {
    gchar *finder_name;
    gboolean already_used;
  } Finder;
  Finder valid_finders[] = {
    {.finder_name = "config", .already_used = FALSE},
    {.finder_name = "lan", .already_used = FALSE},
    {.finder_name = "mount", .already_used = FALSE}
  };

  if (finders == NULL || *finders == NULL)
    {
      ot_util_usage_error (context, "List of finders in --finders option must not be empty", error);
      return FALSE;
    }

  for (const char **iter = finders; iter && *iter; iter++)
    {
      gboolean is_valid_finder = FALSE;
      for (unsigned int i = 0; i < G_N_ELEMENTS (valid_finders); i++)
        {
          if (valid_finders[i].already_used == TRUE)
            continue;
          if (g_strcmp0 (*iter, valid_finders[i].finder_name) == 0)
            {
              is_valid_finder = TRUE;
              valid_finders[i].already_used = TRUE;
            }
        }
      if (!is_valid_finder)
        {
          g_autofree gchar *error_msg = NULL;
          error_msg = g_strdup_printf ("Unknown or duplicate finder type given in --finders option: ‘%s’", *iter);
          ot_util_usage_error (context, error_msg, error);
          return FALSE;
        }
    }

  return TRUE;
}

gboolean
ostree_builtin_find_remotes (int            argc,
                             char         **argv,
                             OstreeCommandInvocation *invocation,
                             GCancellable  *cancellable,
                             GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GPtrArray) refs = NULL;  /* (element-type OstreeCollectionRef) */
  g_autoptr(GPtrArray) finders = NULL;  /* (element-type OstreeRepoFinder) */
  g_autoptr(OstreeRepoFinder) finder_config = NULL;
  g_autoptr(OstreeRepoFinder) finder_mount = NULL;
#ifdef HAVE_AVAHI
  g_autoptr(OstreeRepoFinder) finder_avahi = NULL;
#endif  /* HAVE_AVAHI */
  g_autoptr(OstreeAsyncProgress) progress = NULL;
  gsize i;
  g_autoptr(GAsyncResult) find_result = NULL, pull_result = NULL;
  g_auto(OstreeRepoFinderResultv) results = NULL;
  g_auto(GLnxConsoleRef) console = { 0, };
  g_autoptr(GHashTable) refs_found = NULL;  /* set (element-type OstreeCollectionRef) */
  g_autoptr(GVariant) pull_options = NULL;

  context = g_option_context_new ("COLLECTION-ID REF [COLLECTION-ID REF...]");

  /* Parse options. */
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    return FALSE;

  if (!ostree_ensure_repo_writable (repo, error))
    return FALSE;

  if (argc < 3)
    {
      ot_util_usage_error (context, "At least one COLLECTION-ID REF pair must be specified", error);
      return FALSE;
    }

  if (argc % 2 == 0)
    {
      ot_util_usage_error (context, "Only complete COLLECTION-ID REF pairs may be specified", error);
      return FALSE;
    }

  if (opt_mirror && !opt_pull)
    {
      ot_util_usage_error (context, "When --mirror is specified, --pull must also be", error);
      return FALSE;
    }

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (opt_cache_dir &&
      !ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
    return FALSE;

  /* Read in the refs to search for remotes for. */
  refs = g_ptr_array_new_full (argc, (GDestroyNotify) collection_ref_free0);

  for (i = 1; i < argc; i += 2)
    {
      if (!ostree_validate_collection_id (argv[i], error) ||
          !ostree_validate_rev (argv[i + 1], error))
        return FALSE;

      g_ptr_array_add (refs, ostree_collection_ref_new (argv[i], argv[i + 1]));
    }

  g_ptr_array_add (refs, NULL);

  /* Build the array of OstreeRepoFinder instances */
  if (opt_finders != NULL)
    {
      g_auto(GStrv) finders_strings = NULL;

      finders_strings = g_strsplit (opt_finders, ",", 0);
      if (!validate_finders_list ((const char **)finders_strings, context, error))
        return FALSE;

      finders = g_ptr_array_new_with_free_func (NULL);
      for (const char **iter = (const char **)finders_strings; iter && *iter; iter++)
        {
          if (g_strcmp0 (*iter, "config") == 0)
            {
              finder_config = OSTREE_REPO_FINDER (ostree_repo_finder_config_new ());
              g_ptr_array_add (finders, finder_config);
            }
          else if (g_strcmp0 (*iter, "mount") == 0)
            {
              finder_mount = OSTREE_REPO_FINDER (ostree_repo_finder_mount_new (NULL));
              g_ptr_array_add (finders, finder_mount);
            }
          else if (g_strcmp0 (*iter, "lan") == 0)
            {
#ifdef HAVE_AVAHI
              GMainContext *main_context = g_main_context_get_thread_default ();
              g_autoptr(GError) local_error = NULL;

              finder_avahi = OSTREE_REPO_FINDER (ostree_repo_finder_avahi_new (main_context));
              ostree_repo_finder_avahi_start (OSTREE_REPO_FINDER_AVAHI (finder_avahi), &local_error);

              if (local_error != NULL)
                {
                  g_warning ("Avahi finder failed; removing it: %s", local_error->message);
                  g_clear_object (&finder_avahi);
                }
              else
                g_ptr_array_add (finders, finder_avahi);
#else
              ot_util_usage_error (context, "LAN repo finder requested but ostree was compiled without Avahi support", error);
              return FALSE;
#endif  /* HAVE_AVAHI */
            }
          else
            g_assert_not_reached ();
        }
      g_ptr_array_add (finders, NULL);
    }

  /* Run the operation. */
  glnx_console_lock (&console);

  if (console.is_tty)
    progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

  ostree_repo_find_remotes_async (repo,
                                  (const OstreeCollectionRef * const *) refs->pdata,
                                  NULL  /* no options */,
                                  finders != NULL ? (OstreeRepoFinder **) finders->pdata : NULL,
                                  progress, cancellable,
                                  get_result_cb, &find_result);

  while (find_result == NULL)
    g_main_context_iteration (NULL, TRUE);

  results = ostree_repo_find_remotes_finish (repo, find_result, error);

  if (results == NULL)
    return FALSE;

  if (progress)
    ostree_async_progress_finish (progress);

  /* Print results and work out which refs were not found. */
  refs_found = g_hash_table_new_full (ostree_collection_ref_hash,
                                      ostree_collection_ref_equal, NULL, NULL);

  for (i = 0; results[i] != NULL; i++)
    {
      g_autofree gchar *uri = NULL;
      g_autofree gchar *refs_string = NULL;
      g_autofree gchar *last_modified_string = NULL;

      uri = remote_get_uri (results[i]->remote);
      refs_string = format_ref_to_checksum (results[i]->ref_to_checksum, "   ");
      add_keys_to_set_if_non_null (refs_found, results[i]->ref_to_checksum);

      if (results[i]->summary_last_modified > 0)
        last_modified_string = uint64_secs_to_iso8601 (results[i]->summary_last_modified);
      else
        last_modified_string = g_strdup ("unknown");

      g_print ("Result %" G_GSIZE_FORMAT ": %s\n"
               " - Finder: %s\n"
               " - Keyring: %s\n"
               " - Priority: %d\n"
               " - Summary last modified: %s\n"
               " - Refs:\n"
               "%s\n",
               i, uri, G_OBJECT_TYPE_NAME (results[i]->finder), results[i]->remote->keyring,
               results[i]->priority, last_modified_string, refs_string);
    }

  if (results[0] == NULL)
    {
      g_print ("No results.\n");
      return TRUE;
    }

  g_print ("%u/%u refs were found.\n", g_hash_table_size (refs_found), refs->len - 1);

  /* Print out the refs which weren’t found. */
  if (g_hash_table_size (refs_found) != refs->len - 1  /* NULL terminator */)
    {
      g_print ("Refs not found in any remote:\n");

      for (i = 0; i < refs->len && refs->pdata[i] != NULL; i++)
        {
          const OstreeCollectionRef *ref = g_ptr_array_index (refs, i);
          if (!g_hash_table_contains (refs_found, ref))
            g_print (" - (%s, %s)\n", ref->collection_id, ref->ref_name);
        }
    }

  /* Does the user want us to pull the updates? */
  if (!opt_pull)
    return TRUE;

  {
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

    if (opt_mirror)
      g_variant_builder_add (&builder, "{s@v}", "flags",
                             g_variant_new_variant (g_variant_new_int32 (OSTREE_REPO_PULL_FLAGS_MIRROR)));

    pull_options = g_variant_ref_sink (g_variant_builder_end (&builder));
  }

  /* Run the pull operation. */
  if (console.is_tty)
    progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

  ostree_repo_pull_from_remotes_async (repo,
                                       (const OstreeRepoFinderResult * const *) results,
                                       pull_options,
                                       progress, cancellable,
                                       get_result_cb, &pull_result);

  while (pull_result == NULL)
    g_main_context_iteration (NULL, TRUE);

  if (!ostree_repo_pull_from_remotes_finish (repo, pull_result, error))
    return FALSE;

  if (progress)
    ostree_async_progress_finish (progress);

  /* The pull operation fails if any of the refs can’t be pulled. */
  g_print ("Pulled %u/%u refs successfully.\n", refs->len - 1, refs->len - 1);

  return TRUE;
}
