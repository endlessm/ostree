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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ot-editor.h"
#include "ostree.h"
#include "otutil.h"
#include "parse-datetime.h"
#include "ostree-repo-private.h"
#include "ostree-libarchive-private.h"
#include "ostree-sign.h"

static char *opt_subject;
static char *opt_body;
static char *opt_bootable;
static char *opt_body_file;
static gboolean opt_editor;
static char *opt_parent;
static gboolean opt_orphan;
static gboolean opt_no_bindings;
static char **opt_bind_refs;
static char *opt_branch;
static char *opt_statoverride_file;
static char *opt_skiplist_file;
static char **opt_metadata_strings;
static char **opt_metadata_variants;
static char **opt_detached_metadata_strings;
static char **opt_metadata_keep;
static gboolean opt_link_checkout_speedup;
static gboolean opt_skip_if_unchanged;
static gboolean opt_tar_autocreate_parents;
static char *opt_tar_pathname_filter;
static gboolean opt_no_xattrs;
static char *opt_selinux_policy;
static gboolean opt_selinux_policy_from_base;
static gboolean opt_canonical_permissions;
static gboolean opt_ro_executables;
static gboolean opt_consume;
static gboolean opt_devino_canonical;
static char *opt_base;
static char **opt_trees;
static gint opt_owner_uid = -1;
static gint opt_owner_gid = -1;
static gboolean opt_table_output;
#ifndef OSTREE_DISABLE_GPGME
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;
#endif
static char **opt_key_ids;
static char *opt_sign_name;
static gboolean opt_generate_sizes;
static gboolean opt_disable_fsync;
static char *opt_timestamp;

static gboolean
parse_fsync_cb (const char  *option_name,
                const char  *value,
                gpointer     data,
                GError     **error)
{
  gboolean val;
  if (!ot_parse_boolean (value, &val, error))
    return FALSE;

  opt_disable_fsync = !val;
  return TRUE;
}

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-commit.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "parent", 0, 0, G_OPTION_ARG_STRING, &opt_parent, "Parent commit checksum, or \"none\"", "COMMIT" },
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject", "SUBJECT" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &opt_body, "Full description", "BODY" },
  { "body-file", 'F', 0, G_OPTION_ARG_FILENAME, &opt_body_file, "Commit message from FILE path", "FILE" },
  { "editor", 'e', 0, G_OPTION_ARG_NONE, &opt_editor, "Use an editor to write the commit message", NULL },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &opt_branch, "Branch", "BRANCH" },
  { "orphan", 0, 0, G_OPTION_ARG_NONE, &opt_orphan, "Create a commit without writing a ref", NULL },
  { "no-bindings", 0, 0, G_OPTION_ARG_NONE, &opt_no_bindings, "Do not write any ref bindings", NULL },
  { "bind-ref", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_bind_refs, "Add a ref to ref binding commit metadata", "BRANCH" },
  { "base", 0, 0, G_OPTION_ARG_STRING, &opt_base, "Start from the given commit as a base (no modifiers apply)", "REV" },
  { "tree", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_trees, "Overlay the given argument as a tree", "dir=PATH or tar=TARFILE or ref=COMMIT" },
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Add a key/value pair to metadata", "KEY=VALUE" },
  { "add-metadata", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_variants, "Add a key/value pair to metadata, where the KEY is a string, an VALUE is g_variant_parse() formatted", "KEY=VALUE" },
  { "keep-metadata", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_keep, "Keep metadata KEY and its associated VALUE from parent", "KEY" },
  { "add-detached-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_detached_metadata_strings, "Add a key/value pair to detached metadata", "KEY=VALUE" },
  { "owner-uid", 0, 0, G_OPTION_ARG_INT, &opt_owner_uid, "Set file ownership user id", "UID" },
  { "owner-gid", 0, 0, G_OPTION_ARG_INT, &opt_owner_gid, "Set file ownership group id", "GID" },
  { "canonical-permissions", 0, 0, G_OPTION_ARG_NONE, &opt_canonical_permissions, "Canonicalize permissions in the same way bare-user does for hardlinked files", NULL },
  { "bootable", 0, 0, G_OPTION_ARG_NONE, &opt_bootable, "Flag this commit as a bootable OSTree (e.g. contains a Linux kernel)", NULL },
  { "mode-ro-executables", 0, 0, G_OPTION_ARG_NONE, &opt_ro_executables, "Ensure executable files are not writable", NULL },
  { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs, "Do not import extended attributes", NULL },
  { "selinux-policy", 0, 0, G_OPTION_ARG_FILENAME, &opt_selinux_policy, "Set SELinux labels based on policy in root filesystem PATH (may be /)", "PATH" },
  { "selinux-policy-from-base", 'P', 0, G_OPTION_ARG_NONE, &opt_selinux_policy_from_base, "Set SELinux labels based on first --tree argument", NULL },
  { "link-checkout-speedup", 0, 0, G_OPTION_ARG_NONE, &opt_link_checkout_speedup, "Optimize for commits of trees composed of hardlinks into the repository", NULL },
  { "devino-canonical", 'I', 0, G_OPTION_ARG_NONE, &opt_devino_canonical, "Assume hardlinked objects are unmodified.  Implies --link-checkout-speedup", NULL },
  { "tar-autocreate-parents", 0, 0, G_OPTION_ARG_NONE, &opt_tar_autocreate_parents, "When loading tar archives, automatically create parent directories as needed", NULL },
  { "tar-pathname-filter", 0, 0, G_OPTION_ARG_STRING, &opt_tar_pathname_filter, "When loading tar archives, use REGEX,REPLACEMENT against path names", "REGEX,REPLACEMENT" },
  { "skip-if-unchanged", 0, 0, G_OPTION_ARG_NONE, &opt_skip_if_unchanged, "If the contents are unchanged from previous commit, do nothing", NULL },
  { "statoverride", 0, 0, G_OPTION_ARG_FILENAME, &opt_statoverride_file, "File containing list of modifications to make to permissions", "PATH" },
  { "skip-list", 0, 0, G_OPTION_ARG_FILENAME, &opt_skiplist_file, "File containing list of files to skip", "PATH" },
  { "consume", 0, 0, G_OPTION_ARG_NONE, &opt_consume, "Consume (delete) content after commit (for local directories)", NULL },
  { "table-output", 0, 0, G_OPTION_ARG_NONE, &opt_table_output, "Output more information in a KEY: VALUE format", NULL },
#ifndef OSTREE_DISABLE_GPGME
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, "GPG Key ID to sign the commit with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
#endif
  { "sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "Sign the commit with", "KEY_ID"},
  { "sign-type", 0, 0, G_OPTION_ARG_STRING, &opt_sign_name, "Signature type to use (defaults to 'ed25519')", "NAME"},
  { "generate-sizes", 0, 0, G_OPTION_ARG_NONE, &opt_generate_sizes, "Generate size information along with commit metadata", NULL },
  { "disable-fsync", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
  { "fsync", 0, 0, G_OPTION_ARG_CALLBACK, parse_fsync_cb, "Specify how to invoke fsync()", "POLICY" },
  { "timestamp", 0, 0, G_OPTION_ARG_STRING, &opt_timestamp, "Override the timestamp of the commit", "TIMESTAMP" },
  { NULL }
};

struct CommitFilterData {
  GHashTable *mode_adds;
  GHashTable *mode_overrides;
  GHashTable *skip_list;
};

static gboolean
handle_statoverride_line (const char  *line,
                          void        *data,
                          GError     **error)
{
  struct CommitFilterData *cf = data;
  const char *spc = strchr (line, ' ');
  if (spc == NULL)
    return glnx_throw (error, "Malformed statoverride file (no space found)");
  const char *fn = spc + 1;

  if (g_str_has_prefix (line, "="))
    {
      guint mode_override = (guint32)(gint32)g_ascii_strtod (line+1, NULL);
      g_hash_table_insert (cf->mode_overrides, g_strdup (fn),
                           GUINT_TO_POINTER((gint32)mode_override));
    }
  else
    {
      guint mode_add = (guint32)(gint32)g_ascii_strtod (line, NULL);
      g_hash_table_insert (cf->mode_adds, g_strdup (fn),
                           GUINT_TO_POINTER((gint32)mode_add));
    }
  return TRUE;
}

static gboolean
handle_skiplist_line (const char  *line,
                      void        *data,
                      GError     **error)
{
  GHashTable *files = data;
  g_hash_table_add (files, g_strdup (line));
  return TRUE;
}

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo         *self,
               const char         *path,
               GFileInfo          *file_info,
               gpointer            user_data)
{
  struct CommitFilterData *data = user_data;
  GHashTable *mode_adds = data->mode_adds;
  GHashTable *mode_overrides = data->mode_overrides;
  GHashTable *skip_list = data->skip_list;
  gpointer value;

  if (opt_owner_uid >= 0)
    g_file_info_set_attribute_uint32 (file_info, "unix::uid", opt_owner_uid);
  if (opt_owner_gid >= 0)
    g_file_info_set_attribute_uint32 (file_info, "unix::gid", opt_owner_gid);
  guint mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");

  if (S_ISREG (mode) && opt_ro_executables && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
    {
      mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
      g_file_info_set_attribute_uint32 (file_info, "unix::mode", mode);
    }

  if (mode_adds && g_hash_table_lookup_extended (mode_adds, path, NULL, &value))
    {
      guint mode_add = GPOINTER_TO_UINT (value);
      g_file_info_set_attribute_uint32 (file_info, "unix::mode",
                                        mode | mode_add);
      g_hash_table_remove (mode_adds, path);
    }
  else if (mode_overrides && g_hash_table_lookup_extended (mode_overrides, path, NULL, &value))
    {
      guint current_fmt = g_file_info_get_attribute_uint32 (file_info, "unix::mode") & S_IFMT;
      guint mode_override = GPOINTER_TO_UINT (value);
      g_file_info_set_attribute_uint32 (file_info, "unix::mode",
                                        current_fmt | mode_override);
      g_hash_table_remove (mode_adds, path);
    }

  if (skip_list && g_hash_table_contains (skip_list, path))
    {
      g_hash_table_remove (skip_list, path);
      return OSTREE_REPO_COMMIT_FILTER_SKIP;
    }

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

#ifdef HAVE_LIBARCHIVE
typedef struct {
  GRegex *regex;
  const char *replacement;
} TranslatePathnameData;

/* Implement --tar-pathname-filter */
static char *
handle_translate_pathname (OstreeRepo *repo,
                           const struct stat *stbuf,
                           const char *path,
                           gpointer user_data)
{
  TranslatePathnameData *tpdata = user_data;
  g_autoptr(GError) tmp_error = NULL;
  char *ret =
    g_regex_replace (tpdata->regex, path, -1, 0,
                     tpdata->replacement, 0, &tmp_error);
  g_assert_no_error (tmp_error);
  g_assert (ret);
  return ret;
}
#endif

static gboolean
commit_editor (OstreeRepo     *repo,
               const char     *branch,
               char          **subject,
               char          **body,
               GCancellable   *cancellable,
               GError        **error)
{
  g_autofree char *input = g_strdup_printf ("\n"
      "# Please enter the commit message for your changes. The first line will\n"
      "# become the subject, and the remainder the body. Lines starting\n"
      "# with '#' will be ignored, and an empty message aborts the commit."
      "%s%s%s%s%s%s\n"
              , branch ? "\n#\n# Branch: " : "", branch ? branch : ""
              , *subject ? "\n" : "", *subject ? *subject : ""
              , *body ? "\n" : "", *body ? *body : ""
              );

  *subject = NULL;
  *body = NULL;

  g_autofree char *output = ot_editor_prompt (repo, input, cancellable, error);
  if (output == NULL)
    return FALSE;

  g_auto(GStrv) lines = g_strsplit (output, "\n", -1);
  g_autoptr(GString) bodybuf = NULL;
  for (guint i = 0; lines[i] != NULL; i++)
    {
      g_strchomp (lines[i]);

      /* Lines starting with # are skipped */
      if (lines[i][0] == '#')
        continue;

      /* Blank lines before body starts are skipped */
      if (lines[i][0] == '\0')
        {
          if (!bodybuf)
            continue;
        }

      if (!*subject)
        {
          *subject = g_strdup (lines[i]);
        }
      else if (!bodybuf)
        {
          bodybuf = g_string_new (lines[i]);
        }
      else
        {
          g_string_append_c (bodybuf, '\n');
          g_string_append (bodybuf, lines[i]);
        }
    }

  if (!*subject)
    return glnx_throw (error, "Aborting commit due to empty commit subject.");

  if (bodybuf)
    {
      *body = g_string_free (g_steal_pointer (&bodybuf), FALSE);
      g_strchomp (*body);
    }

  return TRUE;
}

static gboolean
parse_keyvalue_strings (GVariantBuilder   *builder,
                        char             **strings,
                        gboolean           is_gvariant_print,
                        GError           **error)
{
  for (char ** iter = strings; *iter; iter++)
    {
      const char *s = *iter;
      const char *eq = strchr (s, '=');
      if (!eq)
        return glnx_throw (error, "Missing '=' in KEY=VALUE metadata '%s'", s);
      g_autofree char *key = g_strndup (s, eq - s);
      const char *value = eq + 1;
      if (is_gvariant_print)
        {
          g_autoptr(GVariant) variant = g_variant_parse (NULL, value, NULL, NULL, error);
          if (!variant)
            return glnx_prefix_error (error, "Parsing %s", s);

          g_variant_builder_add (builder, "{sv}", key, variant);
        }
      else
        g_variant_builder_add (builder, "{sv}", key,
                               g_variant_new_string (value));
    }

  return TRUE;
}

static void
add_collection_binding (OstreeRepo       *repo,
                        GVariantBuilder  *metadata_builder)
{
  const char *collection_id = ostree_repo_get_collection_id (repo);

  if (collection_id == NULL)
    return;

  g_variant_builder_add (metadata_builder, "{s@v}", OSTREE_COMMIT_META_KEY_COLLECTION_BINDING,
                         g_variant_new_variant (g_variant_new_string (collection_id)));
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  const char **sa = (const char **)a;
  const char **sb = (const char **)b;

  return strcmp (*sa, *sb);
}

static void
add_ref_binding (GVariantBuilder *metadata_builder)
{
  g_assert (opt_branch != NULL || opt_orphan);

  g_autoptr(GPtrArray) refs = g_ptr_array_new ();
  if (opt_branch != NULL)
    g_ptr_array_add (refs, opt_branch);
  for (char **iter = opt_bind_refs; iter != NULL && *iter != NULL; ++iter)
    g_ptr_array_add (refs, *iter);
  g_ptr_array_sort (refs, compare_strings);
  g_autoptr(GVariant) refs_v = g_variant_new_strv ((const char *const *)refs->pdata,
                                                   refs->len);
  g_variant_builder_add (metadata_builder, "{s@v}", OSTREE_COMMIT_META_KEY_REF_BINDING,
                         g_variant_new_variant (g_steal_pointer (&refs_v)));
}

/* Note if you're using the API, you currently need to do this yourself */
static void
fill_bindings (OstreeRepo    *repo,
               GVariant      *metadata,
               GVariant     **out_metadata)
{
  g_autoptr(GVariantBuilder) metadata_builder =
    ot_util_variant_builder_from_variant (metadata, G_VARIANT_TYPE_VARDICT);

  add_ref_binding (metadata_builder);

  /* Allow the collection ID to be overridden using
   * --add-metadata-string=ostree.collection-binding=blah */
  if (metadata == NULL ||
      !g_variant_lookup (metadata, OSTREE_COMMIT_META_KEY_COLLECTION_BINDING, "*", NULL))
    add_collection_binding (repo, metadata_builder);

  *out_metadata = g_variant_ref_sink (g_variant_builder_end (metadata_builder));
}

gboolean
ostree_builtin_commit (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  gboolean skip_commit = FALSE;
  g_autoptr(GFile) object_to_commit = NULL;
  g_autofree char *parent = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariant) detached_metadata = NULL;
  g_autoptr(OstreeMutableTree) mtree = NULL;
  g_autofree char *tree_type = NULL;
  g_autoptr(GHashTable) mode_adds = NULL;
  g_autoptr(GHashTable) mode_overrides = NULL;
  g_autoptr(GHashTable) skip_list = NULL;
  OstreeRepoCommitModifierFlags flags = 0;
  g_autoptr(OstreeSePolicy) policy = NULL;
  g_autoptr(OstreeRepoCommitModifier) modifier = NULL;
  OstreeRepoTransactionStats stats;
  struct CommitFilterData filter_data = { 0, };
  g_autofree char *commit_body = NULL;
  g_autoptr (OstreeSign) sign = NULL;

  context = g_option_context_new ("[PATH]");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (opt_statoverride_file)
    {
      filter_data.mode_adds = mode_adds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      filter_data.mode_overrides = mode_overrides = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      if (!ot_parse_file_by_line (opt_statoverride_file, handle_statoverride_line,
                                  &filter_data, cancellable, error))
        goto out;
    }

  if (opt_skiplist_file)
    {
      skip_list = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      if (!ot_parse_file_by_line (opt_skiplist_file, handle_skiplist_line,
                                  skip_list, cancellable, error))
        goto out;
    }

  if (!(opt_branch || opt_orphan))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A branch must be specified with --branch, or use --orphan");
      goto out;
    }

  if (opt_parent)
    {
      if (g_str_equal (opt_parent, "none"))
        parent = NULL;
      else
        {
          if (!ostree_validate_checksum_string (opt_parent, error))
            goto out;
          parent = g_strdup (opt_parent);
        }
    }
  else if (!opt_orphan)
    {
      if (!ostree_repo_resolve_rev (repo, opt_branch, TRUE, &parent, error))
        {
          if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
            {
              /* A folder exists with the specified ref name,
                 * which is handled by _ostree_repo_write_ref */
              g_clear_error (error);
            }
          else goto out;
        }
    }

  if (!parent && opt_metadata_keep)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Either --branch or --parent must be specified when using "
                           "--keep-metadata");
      goto out;
    }

  if (opt_metadata_strings || opt_metadata_variants || opt_metadata_keep || opt_bootable)
    {
      g_autoptr(GVariantBuilder) builder =
        g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

      if (opt_metadata_strings &&
          !parse_keyvalue_strings (builder, opt_metadata_strings, FALSE, error))
          goto out;

      if (opt_metadata_variants &&
          !parse_keyvalue_strings (builder, opt_metadata_variants, TRUE, error))
        goto out;

      if (opt_metadata_keep)
        {
          g_assert (parent);

          g_autoptr(GVariant) parent_commit = NULL;
          if (!ostree_repo_load_commit (repo, parent, &parent_commit, NULL, error))
            goto out;

          g_auto(GVariantDict) dict;
          g_variant_dict_init (&dict, g_variant_get_child_value (parent_commit, 0));
          for (char **keyp = opt_metadata_keep; keyp && *keyp; keyp++)
            {
              const char *key = *keyp;
              g_autoptr(GVariant) val = g_variant_dict_lookup_value (&dict, key, NULL);
              if (!val)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Missing metadata key '%s' from commit '%s'", key, parent);
                  goto out;
                }

              g_variant_builder_add (builder, "{sv}", key, val);
            }
        }

      metadata = g_variant_ref_sink (g_variant_builder_end (builder));
    }

  if (opt_detached_metadata_strings)
    {
      g_autoptr(GVariantBuilder) builder =
        g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

      if (!parse_keyvalue_strings (builder, opt_detached_metadata_strings, FALSE, error))
        goto out;

      detached_metadata = g_variant_ref_sink (g_variant_builder_end (builder));
    }

  /* Check for conflicting options */
  if (opt_canonical_permissions && opt_owner_uid > 0)
    {
      glnx_throw (error, "Cannot specify both --canonical-permissions and non-zero --owner-uid");
      goto out;
    }
  if (opt_canonical_permissions && opt_owner_gid > 0)
    {
      glnx_throw (error, "Cannot specify both --canonical-permissions and non-zero --owner-gid");
      goto out;
    }
  if (opt_selinux_policy && opt_selinux_policy_from_base)
    {
      glnx_throw (error, "Cannot specify both --selinux-policy and --selinux-policy-from-base");
      goto out;
    }

  if (opt_canonical_permissions || repo->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS;
  if (opt_no_xattrs || repo->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS;
  if (opt_consume)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME;
  if (opt_devino_canonical)
    {
      opt_link_checkout_speedup = TRUE; /* Imply this */
      flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_DEVINO_CANONICAL;
    }
  if (opt_generate_sizes)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES;
  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (flags != 0
      || opt_owner_uid >= 0
      || opt_owner_gid >= 0
      || opt_statoverride_file != NULL
      || opt_skiplist_file != NULL
      || opt_no_xattrs
      || opt_ro_executables
      || opt_selinux_policy
      || opt_selinux_policy_from_base)
    {
      filter_data.mode_adds = mode_adds;
      filter_data.skip_list = skip_list;
      modifier = ostree_repo_commit_modifier_new (flags, commit_filter,
                                                  &filter_data, NULL);

      if (opt_selinux_policy)
        {
          glnx_autofd int rootfs_dfd = -1;
          if (!glnx_opendirat (AT_FDCWD, opt_selinux_policy, TRUE, &rootfs_dfd, error))
            goto out;
          policy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
          if (!policy)
            goto out;
          ostree_repo_commit_modifier_set_sepolicy (modifier, policy);
        }
    }

  if (opt_editor)
    {
      if (!commit_editor (repo, opt_branch, &opt_subject, &commit_body, cancellable, error))
        goto out;
    }
  else if (opt_body_file)
    {
      commit_body = glnx_file_get_contents_utf8_at (AT_FDCWD, opt_body_file, NULL,
                                                    cancellable, error);
      if (!commit_body)
        goto out;
    }
  else if (opt_body)
    commit_body = g_strdup (opt_body);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (opt_link_checkout_speedup && !ostree_repo_scan_hardlinks (repo, cancellable, error))
    goto out;

  if (opt_base)
    {
      mtree = ostree_mutable_tree_new_from_commit (repo, opt_base, error);
      if (!mtree)
        goto out;

      if (opt_selinux_policy_from_base)
        {
          g_assert (modifier);
          if (!ostree_repo_commit_modifier_set_sepolicy_from_commit (modifier, repo, opt_base, cancellable, error))
            goto out;
          /* Don't try to handle it twice */
          opt_selinux_policy_from_base = FALSE;
        }
    }
  else
    {
      mtree = ostree_mutable_tree_new ();
    }


  /* Convert implicit . or explicit path via argv into
   * --tree=dir= so that we only have one primary code path below.
   */
  if (opt_trees == NULL || opt_trees[0] == NULL)
    {
      char *path;
      if (argc <= 1)
        path = ".";
      else
        path = argv[1];
      opt_trees = g_new0 (char *, 2);
      opt_trees[0] = g_strconcat ("dir=", path, NULL);
    }

  const char *const*tree_iter;
  const char *tree;
  const char *eq;
  g_assert (opt_trees && *opt_trees);
  for (tree_iter = (const char *const*)opt_trees; *tree_iter; tree_iter++)
    {
      const gboolean first = (tree_iter == (const char *const*)opt_trees);
      tree = *tree_iter;

      eq = strchr (tree, '=');
      if (!eq)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Missing type in tree specification '%s'", tree);
          goto out;
        }
      g_free (tree_type);
      tree_type = g_strndup (tree, eq - tree);
      tree = eq + 1;

      g_clear_object (&object_to_commit);
      if (strcmp (tree_type, "dir") == 0)
        {
          if (first && opt_selinux_policy_from_base)
            {
              glnx_autofd int rootfs_dfd = -1;
              if (!glnx_opendirat (AT_FDCWD, tree, TRUE, &rootfs_dfd, error))
                goto out;
              policy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
              if (!policy)
                goto out;
              ostree_repo_commit_modifier_set_sepolicy (modifier, policy);
            }
          if (!ostree_repo_write_dfd_to_mtree (repo, AT_FDCWD, tree, mtree, modifier,
                                                cancellable, error))
            goto out;
        }
      else if (strcmp (tree_type, "tar") == 0)
        {
          if (first && opt_selinux_policy_from_base)
            {
              glnx_throw (error, "Cannot use --selinux-policy-from-base with tar");
              goto out;
            }
          if (!opt_tar_pathname_filter)
            {
              if (strcmp (tree, "-") == 0)
                {
                  if (!ostree_repo_write_archive_to_mtree_from_fd (repo, STDIN_FILENO, mtree, modifier,
                                                                    opt_tar_autocreate_parents,
                                                                    cancellable, error))
                    goto out;
                }
              else
                {
                  object_to_commit = g_file_new_for_path (tree);

                  if (!ostree_repo_write_archive_to_mtree (repo, object_to_commit, mtree, modifier,
                                                            opt_tar_autocreate_parents,
                                                            cancellable, error))
                    goto out;
                }
            }
          else
            {
#ifdef HAVE_LIBARCHIVE
              const char *comma = strchr (opt_tar_pathname_filter, ',');
              if (!comma)
                {
                  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "Missing ',' in --tar-pathname-filter");
                  goto out;
                }
              const char *replacement = comma + 1;
              g_autofree char *regexp_text = g_strndup (opt_tar_pathname_filter, comma - opt_tar_pathname_filter);
              /* Use new API if we have a pathname filter */
              OstreeRepoImportArchiveOptions opts = { 0, };
              opts.autocreate_parents = opt_tar_autocreate_parents;
              opts.translate_pathname = handle_translate_pathname;
              g_autoptr(GRegex) regexp = g_regex_new (regexp_text, 0, 0, error);
              TranslatePathnameData tpdata = { regexp, replacement };
              if (!regexp)
                {
                  g_prefix_error (error, "--tar-pathname-filter: ");
                  goto out;
                }
              opts.translate_pathname_user_data = &tpdata;

              g_autoptr(OtAutoArchiveRead) archive;
              if (strcmp (tree, "-") == 0)
                archive = ot_open_archive_read_fd (STDIN_FILENO, error);
              else
                archive = ot_open_archive_read (tree, error);

              if (!archive)
                goto out;
              if (!ostree_repo_import_archive_to_mtree (repo, &opts, archive, mtree,
                                                        modifier, cancellable, error))
                goto out;
#else
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "This version of ostree is not compiled with libarchive support");
              goto out;
#endif
            }
        }
      else if (strcmp (tree_type, "ref") == 0)
        {
          if (first && opt_selinux_policy_from_base)
            {
              g_assert (modifier);
              if (!ostree_repo_commit_modifier_set_sepolicy_from_commit (modifier, repo, tree, cancellable, error))
                goto out;
            }
          if (!ostree_repo_read_commit (repo, tree, &object_to_commit, NULL, cancellable, error))
            goto out;

          if (!ostree_repo_write_directory_to_mtree (repo, object_to_commit, mtree, modifier,
                                                      cancellable, error))
            goto out;
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Invalid tree type specification '%s'", tree_type);
          goto out;
        }
    }

  if (mode_adds && g_hash_table_size (mode_adds) > 0)
    {
      GHashTableIter hash_iter;
      gpointer key, value;

      g_hash_table_iter_init (&hash_iter, mode_adds);

      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          g_printerr ("Unmatched statoverride path: %s\n", (char*)key);
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unmatched statoverride paths");
      goto out;
    }

  if (skip_list && g_hash_table_size (skip_list) > 0)
    {
      GHashTableIter hash_iter;
      gpointer key;

      g_hash_table_iter_init (&hash_iter, skip_list);

      while (g_hash_table_iter_next (&hash_iter, &key, NULL))
        {
          g_printerr ("Unmatched skip-list path: %s\n", (char*)key);
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unmatched skip-list paths");
      goto out;
    }

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (opt_skip_if_unchanged && parent)
    {
      g_autoptr(GFile) parent_root;

      if (!ostree_repo_read_commit (repo, parent, &parent_root, NULL, cancellable, error))
        goto out;

      if (g_file_equal (root, parent_root))
        skip_commit = TRUE;
    }

  if (!skip_commit)
    {
      if (!opt_no_bindings)
        {
          g_autoptr(GVariant) old_metadata = g_steal_pointer (&metadata);
          fill_bindings (repo, old_metadata, &metadata);
        }

      if (opt_bootable)
        {
          g_autoptr(GVariant) old_metadata = g_steal_pointer (&metadata);
          g_auto(GVariantDict) bootmeta;
          g_variant_dict_init (&bootmeta, old_metadata);
          if (!ostree_commit_metadata_for_bootable (root, &bootmeta, cancellable, error))
            goto out;

          metadata = g_variant_ref_sink (g_variant_dict_end (&bootmeta));
        }

      if (!opt_timestamp)
        {
          if (!ostree_repo_write_commit (repo, parent, opt_subject, commit_body, metadata,
                                         OSTREE_REPO_FILE (root),
                                         &commit_checksum, cancellable, error))
            goto out;
        }
      else
        {
          struct timespec ts;
          if (!parse_datetime (&ts, opt_timestamp, NULL))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not parse '%s'", opt_timestamp);
              goto out;
            }

          guint64 timestamp = ts.tv_sec;
          if (!ostree_repo_write_commit_with_time (repo, parent, opt_subject, commit_body, metadata,
                                                   OSTREE_REPO_FILE (root),
                                                   timestamp,
                                                   &commit_checksum, cancellable, error))
            goto out;
        }

      if (detached_metadata)
        {
          if (!ostree_repo_write_commit_detached_metadata (repo, commit_checksum,
                                                           detached_metadata,
                                                           cancellable, error))
            goto out;
        }

      if (opt_key_ids)
        {
          /* Initialize crypto system */
          opt_sign_name = opt_sign_name ?: OSTREE_SIGN_NAME_ED25519;

          sign = ostree_sign_get_by_name (opt_sign_name, error);
          if (sign == NULL)
            goto out;

          char **iter;

          for (iter = opt_key_ids; iter && *iter; iter++)
            {
              const char *keyid = *iter;
              g_autoptr (GVariant) secret_key = NULL;

              secret_key = g_variant_new_string (keyid);
              if (!ostree_sign_set_sk (sign, secret_key, error))
                  goto out;

              if (!ostree_sign_commit (sign,
                                       repo,
                                       commit_checksum,
                                       cancellable,
                                       error))
                goto out;
            }
        }

#ifndef OSTREE_DISABLE_GPGME
      if (opt_gpg_key_ids)
        {
          char **iter;

          for (iter = opt_gpg_key_ids; iter && *iter; iter++)
            {
              const char *keyid = *iter;

              if (!ostree_repo_sign_commit (repo,
                                            commit_checksum,
                                            keyid,
                                            opt_gpg_homedir,
                                            cancellable,
                                            error))
                goto out;
            }
        }
#endif

      if (opt_branch)
        ostree_repo_transaction_set_ref (repo, NULL, opt_branch, commit_checksum);
      else
        g_assert (opt_orphan);

      if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
        goto out;
    }
  else
    {
      commit_checksum = g_strdup (parent);
    }

  if (opt_table_output)
    {
      g_print ("Commit: %s\n", commit_checksum);
      g_print ("Metadata Total: %u\n", stats.metadata_objects_total);
      g_print ("Metadata Written: %u\n", stats.metadata_objects_written);
      g_print ("Content Total: %u\n", stats.content_objects_total);
      g_print ("Content Written: %u\n", stats.content_objects_written);
      g_print ("Content Cache Hits: %u\n", stats.devino_cache_hits);
      g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats.content_bytes_written);
    }
  else
    {
      g_print ("%s\n", commit_checksum);
    }

  ret = TRUE;
 out:
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}
