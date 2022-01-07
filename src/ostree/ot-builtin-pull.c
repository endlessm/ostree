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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_disable_fsync;
static gboolean opt_per_object_fsync;
static gboolean opt_mirror;
static gboolean opt_commit_only;
static gboolean opt_dry_run;
static gboolean opt_disable_static_deltas;
static gboolean opt_require_static_deltas;
static gboolean opt_untrusted;
static gboolean opt_http_trusted;
static gboolean opt_timestamp_check;
static gboolean opt_disable_verify_bindings;
static char* opt_timestamp_check_from_rev;
static gboolean opt_bareuseronly_files;
static char** opt_subpaths;
static char** opt_http_headers;
static char* opt_cache_dir;
static char* opt_append_user_agent;
static int opt_depth = 0;
static int opt_frequency = 0;
static int opt_network_retries = -1;
static char* opt_url;
static char** opt_localcache_repos;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-pull.xml) when changing the option list.
 */

static GOptionEntry options[] = {
   { "commit-metadata-only", 0, 0, G_OPTION_ARG_NONE, &opt_commit_only, "Fetch only the commit metadata", NULL },
   { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_cache_dir, "Use custom cache dir", NULL },
   { "disable-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
   { "per-object-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_per_object_fsync, "Perform writes in such a way that avoids stalling concurrent processes", NULL },
   { "disable-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_disable_static_deltas, "Do not use static deltas", NULL },
   { "require-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_require_static_deltas, "Require static deltas", NULL },
   { "mirror", 0, 0, G_OPTION_ARG_NONE, &opt_mirror, "Write refs suitable for a mirror and fetches all refs if none provided", NULL },
   { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, "Only pull the provided subpath(s)", NULL },
   { "untrusted", 0, 0, G_OPTION_ARG_NONE, &opt_untrusted, "Verify checksums of local sources (always enabled for HTTP pulls)", NULL },
   { "http-trusted", 0, 0, G_OPTION_ARG_NONE, &opt_http_trusted, "Do not verify checksums of HTTP sources (mostly useful when mirroring)", NULL },
   { "bareuseronly-files", 0, 0, G_OPTION_ARG_NONE, &opt_bareuseronly_files, "Reject regular files with mode outside of 0775 (world writable, suid, etc.)", NULL },
   { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, "Only print information on what will be downloaded (requires static deltas)", NULL },
   { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Traverse DEPTH parents (-1=infinite) (default: 0)", "DEPTH" },
   { "url", 0, 0, G_OPTION_ARG_STRING, &opt_url, "Pull objects from this URL instead of the one from the remote config", "URL" },
   { "http-header", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_http_headers, "Add NAME=VALUE as HTTP header to all requests", "NAME=VALUE" },
   { "update-frequency", 0, 0, G_OPTION_ARG_INT, &opt_frequency, "Sets the update frequency, in milliseconds (0=1000ms) (default: 0)", "FREQUENCY" },
   { "network-retries", 0, 0, G_OPTION_ARG_INT, &opt_network_retries, "Specifies how many times each download should be retried upon error (default: 5)", "N"},
   { "localcache-repo", 'L', 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_localcache_repos, "Add REPO as local cache source for objects during this pull", "REPO" },
   { "timestamp-check", 'T', 0, G_OPTION_ARG_NONE, &opt_timestamp_check, "Require fetched commits to have newer timestamps", NULL },
   { "timestamp-check-from-rev", 0, 0, G_OPTION_ARG_STRING, &opt_timestamp_check_from_rev, "Require fetched commits to have newer timestamps than given rev", NULL },
   { "disable-verify-bindings", 0, 0, G_OPTION_ARG_NONE, &opt_disable_verify_bindings, "Do not verify commit bindings", NULL },
   /* let's leave this hidden for now; we just need it for tests */
   { "append-user-agent", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &opt_append_user_agent, "Append string to user agent", NULL },
   { NULL }
 };

#ifndef OSTREE_DISABLE_GPGME
static void
gpg_verify_result_cb (OstreeRepo *repo,
                      const char *checksum,
                      OstreeGpgVerifyResult *result,
                      GLnxConsoleRef *console)
{
  /* Temporarily place the tty back in normal mode before printing GPG
   * verification results.
   */
  glnx_console_unlock (console);

  g_print ("\n");
  ostree_print_gpg_verify_result (result);

  glnx_console_lock (console);
}
#endif /* OSTREE_DISABLE_GPGME */

static gboolean printed_console_progress;

static void
dry_run_console_progress_changed (OstreeAsyncProgress *progress,
                                  gpointer             user_data)
{
  guint fetched_delta_parts, total_delta_parts;
  guint fetched_delta_part_fallbacks, total_delta_part_fallbacks;
  guint64 fetched_delta_part_size, total_delta_part_size, total_delta_part_usize;

  g_assert (!printed_console_progress);
  printed_console_progress = TRUE;

  ostree_async_progress_get (progress,
                             /* Number of parts */
                             "fetched-delta-parts", "u", &fetched_delta_parts,
                             "total-delta-parts", "u", &total_delta_parts,
                             "fetched-delta-fallbacks", "u", &fetched_delta_part_fallbacks,
                             "total-delta-fallbacks", "u", &total_delta_part_fallbacks,
                             /* Size variables */
                             "fetched-delta-part-size", "t", &fetched_delta_part_size,
                             "total-delta-part-size", "t", &total_delta_part_size,
                             "total-delta-part-usize", "t", &total_delta_part_usize,
                             NULL);

  /* Fold the count of deltaparts + fallbacks for simplicity; if changing this,
   * please change ostree_repo_pull_default_console_progress_changed() first.
   */
  fetched_delta_parts += fetched_delta_part_fallbacks;
  total_delta_parts += total_delta_part_fallbacks;

  g_autoptr(GString) buf = g_string_new ("");

  { g_autofree char *formatted_fetched =
      g_format_size (fetched_delta_part_size);
    g_autofree char *formatted_size =
      g_format_size (total_delta_part_size);
    g_autofree char *formatted_usize =
      g_format_size (total_delta_part_usize);

    g_string_append_printf (buf, "Delta update: %u/%u parts, %s/%s, %s total uncompressed",
                            fetched_delta_parts, total_delta_parts,
                            formatted_fetched, formatted_size,
                            formatted_usize);
  }
  g_print ("%s\n", buf->str);
}

static void
noninteractive_console_progress_changed (OstreeAsyncProgress *progress,
                                         gpointer             user_data)
{
  /* We do nothing here - we just want the final status */
}

gboolean
ostree_builtin_pull (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  g_autofree char *remote = NULL;
  OstreeRepoPullFlags pullflags = 0;
  g_autoptr(GPtrArray) refs_to_fetch = NULL;
  g_autoptr(GPtrArray) override_commit_ids = NULL;
  g_autoptr(OstreeAsyncProgress) progress = NULL;
  gulong signal_handler_id = 0;

  context = g_option_context_new ("REMOTE [BRANCH...]");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REMOTE must be specified", error);
      goto out;
    }

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (opt_cache_dir)
    {
      if (!ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
        goto out;
    }

  if (opt_mirror)
    pullflags |= OSTREE_REPO_PULL_FLAGS_MIRROR;

  if (opt_commit_only)
    pullflags |= OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY;

  if (opt_http_trusted)
    pullflags |= OSTREE_REPO_PULL_FLAGS_TRUSTED_HTTP;
  if (opt_untrusted)
    {
      pullflags |= OSTREE_REPO_PULL_FLAGS_UNTRUSTED;
      /* If the user specifies both, assume they really mean untrusted */
      pullflags &= ~OSTREE_REPO_PULL_FLAGS_TRUSTED_HTTP;
    }
  if (opt_bareuseronly_files)
    pullflags |= OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES;

  if (opt_dry_run && !opt_require_static_deltas)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "--dry-run requires --require-static-deltas");
      goto out;
    }

  if (strchr (argv[1], ':') == NULL)
    {
      remote = g_strdup (argv[1]);
      if (argc > 2)
        {
          int i;
          refs_to_fetch = g_ptr_array_new_with_free_func (g_free);

          for (i = 2; i < argc; i++)
            {
              const char *at = strrchr (argv[i], '@');

              if (at)
                {
                  guint j;
                  const char *override_commit_id = at + 1;

                  if (!ostree_validate_checksum_string (override_commit_id, error))
                    goto out;

                  if (!override_commit_ids)
                    {
                      override_commit_ids = g_ptr_array_new_with_free_func (g_free);

                      /* Backfill */
                      for (j = 2; j < i; j++)
                        g_ptr_array_add (override_commit_ids, g_strdup (""));
                    }

                  g_ptr_array_add (override_commit_ids, g_strdup (override_commit_id));
                  g_ptr_array_add (refs_to_fetch, g_strndup (argv[i], at - argv[i]));
                }
              else
                {
                  g_ptr_array_add (refs_to_fetch, g_strdup (argv[i]));
                  if (override_commit_ids)
                    g_ptr_array_add (override_commit_ids, g_strdup (""));
                }
            }

          g_ptr_array_add (refs_to_fetch, NULL);
        }
    }
  else
    {
      char *ref_to_fetch;
      refs_to_fetch = g_ptr_array_new_with_free_func (g_free);
      if (!ostree_parse_refspec (argv[1], &remote, &ref_to_fetch, error))
        goto out;
      /* Transfer ownership */
      g_ptr_array_add (refs_to_fetch, ref_to_fetch);
      g_ptr_array_add (refs_to_fetch, NULL);
    }

  {
    GVariantBuilder builder;
    g_autoptr(GVariant) pull_options = NULL;
    g_auto(GLnxConsoleRef) console = { 0, };
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

    glnx_console_lock (&console);

    if (opt_url)
      g_variant_builder_add (&builder, "{s@v}", "override-url",
                             g_variant_new_variant (g_variant_new_string (opt_url)));
    if (opt_subpaths && opt_subpaths[0] != NULL)
      {
        /* Special case the one-element case so that we excercise this
           old single-argument version in the tests */
        if (opt_subpaths[1] == NULL)
          g_variant_builder_add (&builder, "{s@v}", "subdir",
                                 g_variant_new_variant (g_variant_new_string (opt_subpaths[0])));
        else
          g_variant_builder_add (&builder, "{s@v}", "subdirs",
                                 g_variant_new_variant (g_variant_new_strv ((const char *const*) opt_subpaths, -1)));
      }
    g_variant_builder_add (&builder, "{s@v}", "flags",
                           g_variant_new_variant (g_variant_new_int32 (pullflags)));
    if (refs_to_fetch)
      g_variant_builder_add (&builder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv ((const char *const*) refs_to_fetch->pdata, -1)));
    g_variant_builder_add (&builder, "{s@v}", "depth",
                           g_variant_new_variant (g_variant_new_int32 (opt_depth)));
   
    g_variant_builder_add (&builder, "{s@v}", "update-frequency",
                           g_variant_new_variant (g_variant_new_uint32 (opt_frequency)));
    
    if (opt_network_retries >= 0)
      g_variant_builder_add (&builder, "{s@v}", "n-network-retries",
                             g_variant_new_variant (g_variant_new_uint32 (opt_network_retries)));

    g_variant_builder_add (&builder, "{s@v}", "disable-static-deltas",
                           g_variant_new_variant (g_variant_new_boolean (opt_disable_static_deltas)));

    g_variant_builder_add (&builder, "{s@v}", "require-static-deltas",
                           g_variant_new_variant (g_variant_new_boolean (opt_require_static_deltas)));

    g_variant_builder_add (&builder, "{s@v}", "dry-run",
                           g_variant_new_variant (g_variant_new_boolean (opt_dry_run)));
    if (opt_timestamp_check)
      g_variant_builder_add (&builder, "{s@v}", "timestamp-check",
                             g_variant_new_variant (g_variant_new_boolean (opt_timestamp_check)));
    if (opt_timestamp_check_from_rev)
      g_variant_builder_add (&builder, "{s@v}", "timestamp-check-from-rev",
                             g_variant_new_variant (g_variant_new_string (opt_timestamp_check_from_rev)));

    if (override_commit_ids)
      g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                             g_variant_new_variant (g_variant_new_strv ((const char*const*)override_commit_ids->pdata, override_commit_ids->len)));
    if (opt_localcache_repos)
      g_variant_builder_add (&builder, "{s@v}", "localcache-repos",
                             g_variant_new_variant (g_variant_new_strv ((const char*const*)opt_localcache_repos, -1)));
    if (opt_per_object_fsync)
      g_variant_builder_add (&builder, "{s@v}", "per-object-fsync",
                             g_variant_new_variant (g_variant_new_boolean (TRUE)));
    g_variant_builder_add (&builder, "{s@v}", "disable-verify-bindings",
                           g_variant_new_variant (g_variant_new_boolean (opt_disable_verify_bindings)));
    if (opt_http_headers)
      {
        GVariantBuilder hdr_builder;
        g_variant_builder_init (&hdr_builder, G_VARIANT_TYPE ("a(ss)"));

        for (char **iter = opt_http_headers; iter && *iter; iter++)
          {
            const char *kv = *iter;
            const char *eq = strchr (kv, '=');
            g_autofree char *key = NULL;
            if (!eq)
              {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Missing '=' in --http-header");
                goto out;
              }
            key = g_strndup (kv, eq - kv);
            g_variant_builder_add (&hdr_builder, "(ss)", key, eq + 1);
          }
        g_variant_builder_add (&builder, "{s@v}", "http-headers",
                               g_variant_new_variant (g_variant_builder_end (&hdr_builder)));
      }

    if (opt_append_user_agent)
      g_variant_builder_add (&builder, "{s@v}", "append-user-agent",
                             g_variant_new_variant (g_variant_new_string (opt_append_user_agent)));

    if (!opt_dry_run)
      {
        if (console.is_tty)
          progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);
        else
          progress = ostree_async_progress_new_and_connect (noninteractive_console_progress_changed, &console);
      }
    else
      {
        progress = ostree_async_progress_new_and_connect (dry_run_console_progress_changed, NULL);
      }

    if (console.is_tty)
      {
#ifndef OSTREE_DISABLE_GPGME
        signal_handler_id = g_signal_connect (repo, "gpg-verify-result",
                                              G_CALLBACK (gpg_verify_result_cb),
                                              &console);
#endif /* OSTREE_DISABLE_GPGME */
      }

    pull_options = g_variant_ref_sink (g_variant_builder_end (&builder));

    if (!ostree_repo_pull_with_options (repo, remote, pull_options,
                                        progress, cancellable, error))
      goto out;

    if (!console.is_tty && !opt_dry_run)
      {
        g_assert (progress);
        const char *status = ostree_async_progress_get_status (progress);
        if (status)
          g_print ("%s\n", status);
      }

    ostree_async_progress_finish (progress);

    if (opt_dry_run)
      g_assert (printed_console_progress);
  }

  ret = TRUE;
 out:
  if (signal_handler_id > 0)
    g_signal_handler_disconnect (repo, signal_handler_id);
  return ret;
}
