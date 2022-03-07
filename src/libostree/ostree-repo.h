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

#pragma once

#include <sys/stat.h>

#include "ostree-core.h"
#include "ostree-types.h"
#include "ostree-async-progress.h"
#include "ostree-ref.h"
#include "ostree-repo-finder.h"
#include "ostree-sepolicy.h"
#include "ostree-gpg-verify-result.h"
#include "ostree-sign.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO ostree_repo_get_type()
#define OSTREE_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_REPO, OstreeRepo))
#define OSTREE_IS_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_REPO))

_OSTREE_PUBLIC
gboolean ostree_repo_mode_from_string (const char      *mode,
                                       OstreeRepoMode  *out_mode,
                                       GError         **error);

_OSTREE_PUBLIC
GType ostree_repo_get_type (void);

_OSTREE_PUBLIC
OstreeRepo* ostree_repo_new (GFile *path);

_OSTREE_PUBLIC
OstreeRepo* ostree_repo_new_for_sysroot_path (GFile *repo_path,
                                              GFile *sysroot_path);

_OSTREE_PUBLIC
OstreeRepo* ostree_repo_new_default (void);

_OSTREE_PUBLIC
gboolean      ostree_repo_open   (OstreeRepo     *self,
                                  GCancellable   *cancellable,
                                  GError        **error);

_OSTREE_PUBLIC
OstreeRepo*
ostree_repo_open_at (int           dfd,
                     const char   *path,
                     GCancellable *cancellable,
                     GError      **error);

_OSTREE_PUBLIC
void          ostree_repo_set_disable_fsync (OstreeRepo    *self,
                                             gboolean       disable_fsync);

_OSTREE_PUBLIC
gboolean      ostree_repo_set_cache_dir (OstreeRepo    *self,
                                         int            dfd,
                                         const char    *path,
                                         GCancellable   *cancellable,
                                         GError        **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_get_disable_fsync (OstreeRepo    *self);

_OSTREE_PUBLIC
gboolean      ostree_repo_is_system (OstreeRepo   *repo);

_OSTREE_PUBLIC
gboolean      ostree_repo_is_writable (OstreeRepo  *self,
                                       GError     **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_create (OstreeRepo     *self,
                                  OstreeRepoMode  mode,
                                  GCancellable   *cancellable,
                                  GError        **error);
_OSTREE_PUBLIC
OstreeRepo *  ostree_repo_create_at (int             dfd,
                                     const char     *path,
                                     OstreeRepoMode  mode,
                                     GVariant       *options,
                                     GCancellable   *cancellable,
                                     GError        **error);

_OSTREE_PUBLIC
const gchar * ostree_repo_get_collection_id (OstreeRepo   *self);
_OSTREE_PUBLIC
gboolean      ostree_repo_set_collection_id (OstreeRepo   *self,
                                             const gchar  *collection_id,
                                             GError      **error);

_OSTREE_PUBLIC
const gchar * const * ostree_repo_get_default_repo_finders (OstreeRepo   *self);

_OSTREE_PUBLIC
const gchar * ostree_repo_get_bootloader (OstreeRepo   *self);

_OSTREE_PUBLIC
GFile *       ostree_repo_get_path (OstreeRepo  *self);

_OSTREE_PUBLIC
int           ostree_repo_get_dfd (OstreeRepo  *self);

_OSTREE_PUBLIC
guint         ostree_repo_hash (OstreeRepo *self);
_OSTREE_PUBLIC
gboolean      ostree_repo_equal (OstreeRepo *a,
                                 OstreeRepo *b);

_OSTREE_PUBLIC
OstreeRepoMode ostree_repo_get_mode (OstreeRepo  *self);

_OSTREE_PUBLIC
gboolean       ostree_repo_get_min_free_space_bytes (OstreeRepo *self,
                                                     guint64 *out_reserved_bytes,
                                                     GError **error);
_OSTREE_PUBLIC
GKeyFile *    ostree_repo_get_config (OstreeRepo *self);

_OSTREE_PUBLIC
GKeyFile *    ostree_repo_copy_config (OstreeRepo *self);

_OSTREE_PUBLIC
gboolean      ostree_repo_reload_config (OstreeRepo *self,
                                         GCancellable *cancellable,
                                         GError    **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_remote_add (OstreeRepo     *self,
                                      const char     *name,
                                      const char     *url,
                                      GVariant       *options,
                                      GCancellable   *cancellable,
                                      GError        **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_remote_delete (OstreeRepo     *self,
                                         const char     *name,
                                         GCancellable   *cancellable,
                                         GError        **error);

/**
 * OstreeRepoRemoteChange:
 * The remote change operation.
 * @OSTREE_REPO_REMOTE_CHANGE_ADD: Add a remote
 * @OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS: Like above, but do nothing if the remote exists
 * @OSTREE_REPO_REMOTE_CHANGE_DELETE: Delete a remote
 * @OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS: Delete a remote, do nothing if the remote does not exist
 * @OSTREE_REPO_REMOTE_CHANGE_REPLACE: Add or replace a remote (Since: 2019.2)
 */
typedef enum {
  OSTREE_REPO_REMOTE_CHANGE_ADD,
  OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS,
  OSTREE_REPO_REMOTE_CHANGE_DELETE,
  OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS,
  OSTREE_REPO_REMOTE_CHANGE_REPLACE,
} OstreeRepoRemoteChange;

_OSTREE_PUBLIC
gboolean      ostree_repo_remote_change (OstreeRepo     *self,
                                         GFile          *sysroot,
                                         OstreeRepoRemoteChange changeop,
                                         const char     *name,
                                         const char     *url,
                                         GVariant       *options,
                                         GCancellable   *cancellable,
                                         GError        **error);

_OSTREE_PUBLIC
char **       ostree_repo_remote_list    (OstreeRepo *self,
                                          guint      *out_n_remotes);

_OSTREE_PUBLIC
gboolean      ostree_repo_remote_get_url (OstreeRepo   *self,
                                          const char   *name,
                                          char        **out_url,
                                          GError      **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_get_remote_option (OstreeRepo  *self,
                                             const char  *remote_name,
                                             const char  *option_name,
                                             const char  *default_value,
                                             char       **out_value,
                                             GError     **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_get_remote_list_option (OstreeRepo   *self,
                                                  const char   *remote_name,
                                                  const char   *option_name,
                                                  char       ***out_value,
                                                  GError      **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_get_remote_boolean_option (OstreeRepo  *self,
                                                     const char  *remote_name,
                                                     const char  *option_name,
                                                     gboolean     default_value,
                                                     gboolean    *out_value,
                                                     GError     **error);


_OSTREE_PUBLIC
gboolean      ostree_repo_remote_fetch_summary (OstreeRepo    *self,
                                                const char    *name,
                                                GBytes       **out_summary,
                                                GBytes       **out_signatures,
                                                GCancellable  *cancellable,
                                                GError       **error);

_OSTREE_PUBLIC
gboolean ostree_repo_remote_fetch_summary_with_options (OstreeRepo    *self,
                                                        const char    *name,
                                                        GVariant      *options,
                                                        GBytes       **out_summary,
                                                        GBytes       **out_signatures,
                                                        GCancellable  *cancellable,
                                                        GError       **error);

_OSTREE_PUBLIC
OstreeRepo * ostree_repo_get_parent (OstreeRepo  *self);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_config (OstreeRepo *self,
                                        GKeyFile   *new_config,
                                        GError    **error);

/**
 * OstreeRepoCommitState:
 * @OSTREE_REPO_COMMIT_STATE_NORMAL: Commit is complete. This is the default.
 *    (Since: 2017.14.)
 * @OSTREE_REPO_COMMIT_STATE_PARTIAL: One or more objects are missing from the
 *    local copy of the commit, but metadata is present. (Since: 2015.7.)
 * @OSTREE_REPO_COMMIT_STATE_FSCK_PARTIAL: One or more objects are missing from the
 *    local copy of the commit, due to an fsck --delete. (Since: 2019.4.)
 *
 * Flags representing the state of a commit in the local repository, as returned
 * by ostree_repo_load_commit().
 *
 * Since: 2015.7
 */
typedef enum {
  OSTREE_REPO_COMMIT_STATE_NORMAL = 0,
  OSTREE_REPO_COMMIT_STATE_PARTIAL = (1 << 0),
  OSTREE_REPO_COMMIT_STATE_FSCK_PARTIAL = (1 << 1),
} OstreeRepoCommitState;

/**
 * OstreeRepoTransactionStats:
 * @metadata_objects_total: The total number of metadata objects
 * in the repository after this transaction has completed.
 * @metadata_objects_written: The number of metadata objects that
 * were written to the repository in this transaction.
 * @content_objects_total: The total number of content objects
 * in the repository after this transaction has completed.
 * @content_objects_written: The number of content objects that
 * were written to the repository in this transaction.
 * @content_bytes_written: The amount of data added to the repository,
 * in bytes, counting only content objects.
 * @padding1: reserved
 * @padding2: reserved
 * @padding3: reserved
 * @padding4: reserved
 *
 * A list of statistics for each transaction that may be
 * interesting for reporting purposes.
 */
typedef struct _OstreeRepoTransactionStats OstreeRepoTransactionStats;

struct _OstreeRepoTransactionStats {
  guint metadata_objects_total;
  guint metadata_objects_written;
  guint content_objects_total;
  guint content_objects_written;
  guint64 content_bytes_written;
  guint devino_cache_hits;

  guint   padding1;
  guint64 padding2;
  guint64 padding3;
  guint64 padding4;
};

_OSTREE_PUBLIC
GType ostree_repo_transaction_stats_get_type (void);

_OSTREE_PUBLIC
gboolean      ostree_repo_scan_hardlinks      (OstreeRepo     *self,
                                               GCancellable   *cancellable,
                                               GError        **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_prepare_transaction (OstreeRepo     *self,
                                               gboolean       *out_transaction_resume,
                                               GCancellable   *cancellable,
                                               GError        **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_commit_transaction (OstreeRepo                  *self,
                                              OstreeRepoTransactionStats  *out_stats,
                                              GCancellable                *cancellable,
                                              GError                     **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_abort_transaction (OstreeRepo     *self,
                                             GCancellable   *cancellable,
                                             GError        **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_mark_commit_partial (OstreeRepo     *self,
                                               const char     *checksum,
                                               gboolean        is_partial,
                                               GError        **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_mark_commit_partial_reason (OstreeRepo     *self,
                                                      const char     *checksum,
                                                      gboolean        is_partial,
                                                      OstreeRepoCommitState in_state,
                                                      GError        **error);

_OSTREE_PUBLIC
void          ostree_repo_transaction_set_refspec (OstreeRepo *self,
                                                   const char *refspec,
                                                   const char *checksum);

_OSTREE_PUBLIC
void          ostree_repo_transaction_set_ref     (OstreeRepo *self,
                                                   const char *remote,
                                                   const char *ref,
                                                   const char *checksum);

_OSTREE_PUBLIC
void          ostree_repo_transaction_set_collection_ref (OstreeRepo                *self,
                                                          const OstreeCollectionRef *ref,
                                                          const char                *checksum);

_OSTREE_PUBLIC
gboolean      ostree_repo_set_ref_immediate (OstreeRepo *self,
                                             const char *remote,
                                             const char *ref,
                                             const char *checksum,
                                             GCancellable  *cancellable,
                                             GError       **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_set_alias_ref_immediate (OstreeRepo *self,
                                                   const char *remote,
                                                   const char *ref,
                                                   const char *target,
                                                   GCancellable  *cancellable,
                                                   GError       **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_set_collection_ref_immediate (OstreeRepo                 *self,
                                                        const OstreeCollectionRef  *ref,
                                                        const char                 *checksum,
                                                        GCancellable               *cancellable,
                                                        GError                    **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_has_object (OstreeRepo           *self,
                                      OstreeObjectType      objtype,
                                      const char           *checksum,
                                      gboolean             *out_have_object,
                                      GCancellable         *cancellable,
                                      GError              **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_metadata (OstreeRepo        *self,
                                          OstreeObjectType   objtype,
                                          const char        *expected_checksum,
                                          GVariant          *object,
                                          guchar           **out_csum,
                                          GCancellable      *cancellable,
                                          GError           **error);

_OSTREE_PUBLIC
void          ostree_repo_write_metadata_async (OstreeRepo              *self,
                                                OstreeObjectType         objtype,
                                                const char              *expected_checksum,
                                                GVariant                *object,
                                                GCancellable            *cancellable,
                                                GAsyncReadyCallback      callback,
                                                gpointer                 user_data);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_metadata_finish (OstreeRepo        *self,
                                                 GAsyncResult      *result,
                                                 guchar           **out_csum,
                                                 GError           **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_content (OstreeRepo       *self,
                                         const char       *expected_checksum,
                                         GInputStream     *object_input,
                                         guint64           length,
                                         guchar          **out_csum,
                                         GCancellable     *cancellable,
                                         GError          **error);

_OSTREE_PUBLIC
char *      ostree_repo_write_regfile_inline (OstreeRepo       *self,
                                                const char       *expected_checksum,
                                                guint32           uid,
                                                guint32           gid,
                                                guint32           mode,
                                                GVariant         *xattrs,
                                                const guint8*     buf,
                                                gsize             len,
                                                GCancellable     *cancellable,
                                                GError          **error);

_OSTREE_PUBLIC
OstreeContentWriter *    ostree_repo_write_regfile (OstreeRepo       *self,
                                                    const char       *expected_checksum,
                                                    guint32           uid,
                                                    guint32           gid,
                                                    guint32           mode,
                                                    guint64           content_len,
                                                    GVariant         *xattrs,
                                                    GError          **error);
             
_OSTREE_PUBLIC
char *      ostree_repo_write_symlink (OstreeRepo       *self,
                                       const char       *expected_checksum,
                                       guint32           uid,
                                       guint32           gid,
                                       GVariant         *xattrs,
                                       const char       *symlink_target,
                                       GCancellable     *cancellable,
                                       GError          **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_metadata_trusted (OstreeRepo        *self,
                                                  OstreeObjectType   objtype,
                                                  const char        *checksum,
                                                  GVariant          *variant,
                                                  GCancellable      *cancellable,
                                                  GError           **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_metadata_stream_trusted (OstreeRepo        *self,
                                                         OstreeObjectType   objtype,
                                                         const char        *checksum,
                                                         GInputStream      *object_input,
                                                         guint64            length,
                                                         GCancellable      *cancellable,
                                                         GError           **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_content_trusted (OstreeRepo       *self,
                                                 const char       *checksum,
                                                 GInputStream     *object_input,
                                                 guint64           length,
                                                 GCancellable     *cancellable,
                                                 GError          **error);

_OSTREE_PUBLIC
void          ostree_repo_write_content_async (OstreeRepo              *self,
                                               const char              *expected_checksum,
                                               GInputStream            *object,
                                               guint64                  length,
                                               GCancellable            *cancellable,
                                               GAsyncReadyCallback      callback,
                                               gpointer                 user_data);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_content_finish (OstreeRepo        *self,
                                                GAsyncResult      *result,
                                                guchar           **out_csum,
                                                GError           **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_resolve_rev (OstreeRepo  *self,
                                       const char  *refspec,
                                       gboolean     allow_noent,
                                       char       **out_rev,
                                       GError     **error);

/**
 * OstreeRepoResolveRevExtFlags:
 * @OSTREE_REPO_RESOLVE_REV_EXT_NONE: No flags.
 * @OSTREE_REPO_RESOLVE_REV_EXT_LOCAL_ONLY: Exclude remote and mirrored refs. Since: 2019.2
 */
typedef enum {
  OSTREE_REPO_RESOLVE_REV_EXT_NONE = 0,
  OSTREE_REPO_RESOLVE_REV_EXT_LOCAL_ONLY = (1 << 0),
} OstreeRepoResolveRevExtFlags;

_OSTREE_PUBLIC
gboolean      ostree_repo_resolve_rev_ext (OstreeRepo                    *self,
                                           const char                    *refspec,
                                           gboolean                       allow_noent,
                                           OstreeRepoResolveRevExtFlags   flags,
                                           char                         **out_rev,
                                           GError                       **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_resolve_collection_ref (OstreeRepo                    *self,
                                                  const OstreeCollectionRef     *ref,
                                                  gboolean                       allow_noent,
                                                  OstreeRepoResolveRevExtFlags   flags,
                                                  char                         **out_rev,
                                                  GCancellable                  *cancellable,
                                                  GError                       **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_list_refs (OstreeRepo       *self,
                                     const char       *refspec_prefix,
                                     GHashTable      **out_all_refs,
                                     GCancellable     *cancellable,
                                     GError          **error);

/**
 * OstreeRepoListRefsExtFlags:
 * @OSTREE_REPO_LIST_REFS_EXT_NONE: No flags.
 * @OSTREE_REPO_LIST_REFS_EXT_ALIASES: Only list aliases.  Since: 2017.10
 * @OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES: Exclude remote refs.  Since: 2017.11
 * @OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_MIRRORS: Exclude mirrored refs.  Since: 2019.2
 */
typedef enum {
  OSTREE_REPO_LIST_REFS_EXT_NONE = 0,
  OSTREE_REPO_LIST_REFS_EXT_ALIASES = (1 << 0),
  OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES = (1 << 1),
  OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_MIRRORS = (1 << 2),
} OstreeRepoListRefsExtFlags;

_OSTREE_PUBLIC
gboolean      ostree_repo_list_refs_ext (OstreeRepo                 *self,
                                         const char                 *refspec_prefix,
                                         GHashTable                 **out_all_refs,
                                         OstreeRepoListRefsExtFlags flags,
                                         GCancellable               *cancellable,
                                         GError                     **error);

_OSTREE_PUBLIC
gboolean ostree_repo_remote_list_refs (OstreeRepo       *self,
                                       const char       *remote_name,
                                       GHashTable      **out_all_refs,
                                       GCancellable     *cancellable,
                                       GError          **error);

_OSTREE_PUBLIC
gboolean ostree_repo_remote_list_collection_refs (OstreeRepo    *self,
                                                  const char    *remote_name,
                                                  GHashTable   **out_all_refs,
                                                  GCancellable  *cancellable,
                                                  GError       **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_load_variant (OstreeRepo  *self,
                                        OstreeObjectType objtype,
                                        const char    *sha256, 
                                        GVariant     **out_variant,
                                        GError       **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_load_variant_if_exists (OstreeRepo  *self,
                                                  OstreeObjectType objtype,
                                                  const char    *sha256, 
                                                  GVariant     **out_variant,
                                                  GError       **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_load_commit (OstreeRepo            *self,
                                       const char            *checksum, 
                                       GVariant             **out_commit,
                                       OstreeRepoCommitState *out_state,
                                       GError               **error);

_OSTREE_PUBLIC
gboolean ostree_repo_load_file (OstreeRepo         *self,
                                const char         *checksum,
                                GInputStream      **out_input,
                                GFileInfo         **out_file_info,
                                GVariant          **out_xattrs,
                                GCancellable       *cancellable,
                                GError            **error);

_OSTREE_PUBLIC
gboolean ostree_repo_load_object_stream (OstreeRepo         *self,
                                         OstreeObjectType    objtype,
                                         const char         *checksum,
                                         GInputStream      **out_input,
                                         guint64            *out_size,
                                         GCancellable       *cancellable,
                                         GError            **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_query_object_storage_size (OstreeRepo           *self,
                                                     OstreeObjectType      objtype,
                                                     const char           *sha256, 
                                                     guint64              *out_size,
                                                     GCancellable         *cancellable,
                                                     GError              **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_import_object_from (OstreeRepo           *self,
                                              OstreeRepo           *source,
                                              OstreeObjectType      objtype,
                                              const char           *checksum,
                                              GCancellable         *cancellable,
                                              GError              **error);
_OSTREE_PUBLIC
gboolean      ostree_repo_import_object_from_with_trust (OstreeRepo           *self,
                                                         OstreeRepo           *source,
                                                         OstreeObjectType      objtype,
                                                         const char           *checksum,
                                                         gboolean              trusted,
                                                         GCancellable         *cancellable,
                                                         GError              **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_delete_object (OstreeRepo           *self,
                                         OstreeObjectType      objtype,
                                         const char           *sha256,
                                         GCancellable         *cancellable,
                                         GError              **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_fsck_object (OstreeRepo           *self,
                                       OstreeObjectType      objtype,
                                       const char           *sha256,
                                       GCancellable         *cancellable,
                                       GError              **error);

/** 
 * OstreeRepoCommitFilterResult:
 * @OSTREE_REPO_COMMIT_FILTER_ALLOW: Do commit this object
 * @OSTREE_REPO_COMMIT_FILTER_SKIP: Ignore this object
 */
typedef enum {
  OSTREE_REPO_COMMIT_FILTER_ALLOW,
  OSTREE_REPO_COMMIT_FILTER_SKIP
} OstreeRepoCommitFilterResult;

/**
 * OstreeRepoCommitFilter:
 * @repo: Repo
 * @path: Path to file
 * @file_info: File information
 * @user_data: User data
 *
 * Returns: #OstreeRepoCommitFilterResult saying whether or not to commit this file
 */
typedef OstreeRepoCommitFilterResult (*OstreeRepoCommitFilter) (OstreeRepo    *repo,
                                                                const char    *path,
                                                                GFileInfo     *file_info,
                                                                gpointer       user_data);

/**
 * OstreeRepoCommitModifierFlags:
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE: No special flags
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS: Do not process extended attributes
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES: Generate size information.
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS: Canonicalize permissions.
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED: Emit an error if configured SELinux policy does not provide a label
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME: Delete added files/directories after commit; Since: 2017.13
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_DEVINO_CANONICAL: If a devino cache hit is found, skip modifier filters (non-directories only); Since: 2017.14
 *
 * Flags modifying commit behavior. In bare-user-only mode, @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS
 * and @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS are automatically enabled.
 *
 */
typedef enum {
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE = 0,
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS = (1 << 0),
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES = (1 << 1),
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS = (1 << 2),
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED = (1 << 3),
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME = (1 << 4),
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_DEVINO_CANONICAL = (1 << 5),
} OstreeRepoCommitModifierFlags;

/**
 * OstreeRepoCommitModifier:
 *
 * A structure allowing control over commits.
 */
typedef struct OstreeRepoCommitModifier OstreeRepoCommitModifier;

_OSTREE_PUBLIC
OstreeRepoCommitModifier *ostree_repo_commit_modifier_new (OstreeRepoCommitModifierFlags  flags,
                                                           OstreeRepoCommitFilter         commit_filter,
                                                           gpointer                       user_data,
                                                           GDestroyNotify                 destroy_notify);

_OSTREE_PUBLIC
GType ostree_repo_commit_modifier_get_type (void);

typedef GVariant *(*OstreeRepoCommitModifierXattrCallback) (OstreeRepo     *repo,
                                                            const char     *path,
                                                            GFileInfo      *file_info,
                                                            gpointer        user_data);

_OSTREE_PUBLIC
void ostree_repo_commit_modifier_set_xattr_callback (OstreeRepoCommitModifier              *modifier,
                                                     OstreeRepoCommitModifierXattrCallback  callback,
                                                     GDestroyNotify                         destroy,
                                                     gpointer                               user_data);

_OSTREE_PUBLIC
void ostree_repo_commit_modifier_set_sepolicy (OstreeRepoCommitModifier              *modifier,
                                               OstreeSePolicy                        *sepolicy);

_OSTREE_PUBLIC
gboolean ostree_repo_commit_modifier_set_sepolicy_from_commit (OstreeRepoCommitModifier              *modifier,
                                                               OstreeRepo                            *repo,
                                                               const char                            *rev,
                                                               GCancellable                          *cancellable,
                                                               GError                               **error);

_OSTREE_PUBLIC
void ostree_repo_commit_modifier_set_devino_cache (OstreeRepoCommitModifier              *modifier,
                                                   OstreeRepoDevInoCache                 *cache);

_OSTREE_PUBLIC
OstreeRepoCommitModifier *ostree_repo_commit_modifier_ref (OstreeRepoCommitModifier *modifier);
_OSTREE_PUBLIC
void ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_directory_to_mtree (OstreeRepo                 *self,
                                                    GFile                      *dir,
                                                    OstreeMutableTree          *mtree,
                                                    OstreeRepoCommitModifier   *modifier,
                                                    GCancellable               *cancellable,
                                                    GError                    **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_dfd_to_mtree (OstreeRepo                 *self,
                                              int                         dfd,
                                              const char                 *path,
                                              OstreeMutableTree          *mtree,
                                              OstreeRepoCommitModifier   *modifier,
                                              GCancellable               *cancellable,
                                              GError                    **error);


_OSTREE_PUBLIC
gboolean      ostree_repo_write_archive_to_mtree (OstreeRepo                   *self,
                                                  GFile                        *archive,
                                                  OstreeMutableTree            *mtree,
                                                  OstreeRepoCommitModifier     *modifier,
                                                  gboolean                      autocreate_parents,
                                                  GCancellable                 *cancellable,
                                                  GError                      **error);


_OSTREE_PUBLIC
gboolean      ostree_repo_write_archive_to_mtree_from_fd (OstreeRepo                *self,
                                                          int                        fd,
                                                          OstreeMutableTree         *mtree,
                                                          OstreeRepoCommitModifier  *modifier,
                                                          gboolean                   autocreate_parents,
                                                          GCancellable              *cancellable,
                                                          GError                   **error);

/**
 * OstreeRepoImportArchiveTranslatePathname:
 * @repo: Repo
 * @stbuf: Stat buffer
 * @src_path: Path in the archive
 * @user_data: User data
 *
 * Possibly change a pathname while importing an archive. If %NULL is returned,
 * then @src_path will be used unchanged.  Otherwise, return a new pathname which
 * will be freed via `g_free()`.
 *
 * This pathname translation will be performed *before* any processing from an
 * active `OstreeRepoCommitModifier`. Will be invoked for all directory and file
 * types, first with outer directories, then their sub-files and directories.
 *
 * Note that enabling pathname translation will always override the setting for
 * `use_ostree_convention`.
 *
 * Since: 2017.11
 */
typedef char *(*OstreeRepoImportArchiveTranslatePathname) (OstreeRepo     *repo,
                                                           const struct stat *stbuf,
                                                           const char     *src_path,
                                                           gpointer        user_data);

/**
 * OstreeRepoImportArchiveOptions: (skip)
 *
 * An extensible options structure controlling archive import.  Ensure that
 * you have entirely zeroed the structure, then set just the desired
 * options.  This is used by ostree_repo_import_archive_to_mtree().
 */
typedef struct {
  guint ignore_unsupported_content : 1;
  guint autocreate_parents : 1;
  guint use_ostree_convention : 1;
  guint callback_with_entry_pathname : 1;
  guint reserved : 28;

  guint unused_uint[8];
  OstreeRepoImportArchiveTranslatePathname translate_pathname;
  gpointer translate_pathname_user_data;
  gpointer unused_ptrs[6];
} OstreeRepoImportArchiveOptions;

_OSTREE_PUBLIC
gboolean      ostree_repo_import_archive_to_mtree (OstreeRepo                      *self,
                                                   OstreeRepoImportArchiveOptions  *opts,
                                                   void                            *archive, /* Really struct archive * */
                                                   OstreeMutableTree               *mtree,
                                                   OstreeRepoCommitModifier        *modifier,
                                                    GCancellable                   *cancellable,
                                                    GError                        **error);

/**
 * OstreeRepoExportArchiveOptions: (skip)
 *
 * An extensible options structure controlling archive creation.  Ensure that
 * you have entirely zeroed the structure, then set just the desired
 * options.  This is used by ostree_repo_export_tree_to_archive().
 */
typedef struct {
  guint disable_xattrs : 1;
  guint reserved : 31;

  /* 4 byte hole on 64 bit arches */
  guint64 timestamp_secs;

  guint unused_uint[8];

  char *path_prefix;

  gpointer unused_ptrs[7];
} OstreeRepoExportArchiveOptions;

_OSTREE_PUBLIC
gboolean ostree_repo_export_tree_to_archive (OstreeRepo                *self,
                                             OstreeRepoExportArchiveOptions  *opts,
                                             OstreeRepoFile            *root,
                                             void                      *archive,  /* Really struct archive * */
                                             GCancellable             *cancellable,
                                             GError                  **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_mtree (OstreeRepo         *self,
                                       OstreeMutableTree  *mtree,
                                       GFile             **out_file,
                                       GCancellable       *cancellable,
                                       GError            **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_commit (OstreeRepo      *self,
                                        const char      *parent,
                                        const char      *subject,
                                        const char      *body,
                                        GVariant        *metadata,
                                        OstreeRepoFile  *root,
                                        char           **out_commit,
                                        GCancellable    *cancellable,
                                        GError         **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_commit_with_time (OstreeRepo      *self,
                                                  const char      *parent,
                                                  const char      *subject,
                                                  const char      *body,
                                                  GVariant        *metadata,
                                                  OstreeRepoFile  *root,
                                                  guint64         time,
                                                  char           **out_commit,
                                                  GCancellable    *cancellable,
                                                  GError         **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_read_commit_detached_metadata (OstreeRepo      *self,
                                                         const char      *checksum,
                                                         GVariant       **out_metadata,
                                                         GCancellable    *cancellable,
                                                         GError         **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_write_commit_detached_metadata (OstreeRepo      *self,
                                                          const char      *checksum,
                                                          GVariant        *metadata,
                                                          GCancellable    *cancellable,
                                                          GError         **error);

/**
 * OstreeRepoCheckoutMode:
 * @OSTREE_REPO_CHECKOUT_MODE_NONE: No special options
 * @OSTREE_REPO_CHECKOUT_MODE_USER: Ignore uid/gid of files
 */
typedef enum {
  OSTREE_REPO_CHECKOUT_MODE_NONE = 0,
  OSTREE_REPO_CHECKOUT_MODE_USER = 1
} OstreeRepoCheckoutMode;

/**
 * OstreeRepoCheckoutOverwriteMode:
 * @OSTREE_REPO_CHECKOUT_OVERWRITE_NONE: No special options
 * @OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES: When layering checkouts, unlink() and replace existing files, but do not modify existing directories (unless whiteouts are enabled, then directories are replaced)
 * @OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES: Only add new files/directories
 * @OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL: Like UNION_FILES, but error if files are not identical (requires hardlink checkouts)
 */
typedef enum {
  OSTREE_REPO_CHECKOUT_OVERWRITE_NONE = 0,
  OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES = 1,
  OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES = 2, /* Since: 2017.3 */
  OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL = 3, /* Since: 2017.11 */
} OstreeRepoCheckoutOverwriteMode;

_OSTREE_PUBLIC
gboolean
ostree_repo_checkout_tree (OstreeRepo               *self,
                           OstreeRepoCheckoutMode    mode,
                           OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                           GFile                    *destination,
                           OstreeRepoFile           *source,
                           GFileInfo                *source_info,
                           GCancellable             *cancellable,
                           GError                  **error);

/**
 * OstreeRepoCheckoutFilterResult:
 * @OSTREE_REPO_CHECKOUT_FILTER_ALLOW: Do checkout this object
 * @OSTREE_REPO_CHECKOUT_FILTER_SKIP: Ignore this object
 *
 * Since: 2018.2
 */
typedef enum {
  OSTREE_REPO_CHECKOUT_FILTER_ALLOW,
  OSTREE_REPO_CHECKOUT_FILTER_SKIP
} OstreeRepoCheckoutFilterResult;

/**
 * OstreeRepoCheckoutFilter:
 * @repo: Repo
 * @path: Path to file
 * @stbuf: File information
 * @user_data: User data
 *
 * Returns: #OstreeRepoCheckoutFilterResult saying whether or not to checkout this file
 *
 * Since: 2018.2
 */
typedef OstreeRepoCheckoutFilterResult (*OstreeRepoCheckoutFilter) (OstreeRepo    *repo,
                                                                    const char    *path,
                                                                    struct stat   *stbuf,
                                                                    gpointer       user_data);

/**
 * OstreeRepoCheckoutAtOptions:
 *
 * An extensible options structure controlling checkout.  Ensure that
 * you have entirely zeroed the structure, then set just the desired
 * options.  This is used by ostree_repo_checkout_at() which
 * supercedes previous separate enumeration usage in
 * ostree_repo_checkout_tree() and ostree_repo_checkout_tree_at().
 */
typedef struct {
  OstreeRepoCheckoutMode mode;
  OstreeRepoCheckoutOverwriteMode overwrite_mode;

  gboolean enable_uncompressed_cache;  /* Deprecated */
  gboolean enable_fsync;  /* Deprecated */
  gboolean process_whiteouts;
  gboolean no_copy_fallback;
  gboolean force_copy; /* Since: 2017.6 */
  gboolean bareuseronly_dirs; /* Since: 2017.7 */
  gboolean force_copy_zerosized; /* Since: 2018.9 */
  gboolean unused_bools[4];
  /* 4 byte hole on 64 bit */

  const char *subpath;

  OstreeRepoDevInoCache *devino_to_csum_cache;

  int unused_ints[6];
  gpointer unused_ptrs[3];
  OstreeRepoCheckoutFilter filter; /* Since: 2018.2 */
  gpointer filter_user_data; /* Since: 2018.2 */
  OstreeSePolicy *sepolicy; /* Since: 2017.6 */
  const char *sepolicy_prefix;
} OstreeRepoCheckoutAtOptions;

_OSTREE_PUBLIC
GType ostree_repo_devino_cache_get_type (void);
_OSTREE_PUBLIC
OstreeRepoDevInoCache *ostree_repo_devino_cache_new (void);
_OSTREE_PUBLIC
OstreeRepoDevInoCache * ostree_repo_devino_cache_ref (OstreeRepoDevInoCache *cache);
_OSTREE_PUBLIC
void ostree_repo_devino_cache_unref (OstreeRepoDevInoCache *cache);

_OSTREE_PUBLIC
void ostree_repo_checkout_at_options_set_devino (OstreeRepoCheckoutAtOptions *opts, OstreeRepoDevInoCache *cache);

_OSTREE_PUBLIC
gboolean ostree_repo_checkout_at (OstreeRepo                         *self,
                                  OstreeRepoCheckoutAtOptions        *options,
                                  int                                 destination_dfd,
                                  const char                         *destination_path,
                                  const char                         *commit,
                                  GCancellable                       *cancellable,
                                  GError                            **error);

_OSTREE_PUBLIC
gboolean       ostree_repo_checkout_gc (OstreeRepo        *self,
                                        GCancellable      *cancellable,
                                        GError           **error);

_OSTREE_PUBLIC
gboolean       ostree_repo_read_commit (OstreeRepo    *self,
                                        const char    *ref,
                                        GFile        **out_root,
                                        char         **out_commit,
                                        GCancellable  *cancellable,
                                        GError        **error);

/**
 * OstreeRepoListObjectsFlags:
 * @OSTREE_REPO_LIST_OBJECTS_LOOSE: List only loose (plain file) objects
 * @OSTREE_REPO_LIST_OBJECTS_PACKED: List only packed (compacted into blobs) objects
 * @OSTREE_REPO_LIST_OBJECTS_ALL: List all objects
 * @OSTREE_REPO_LIST_OBJECTS_NO_PARENTS: Only list objects in this repo, not parents
 */
typedef enum {
  OSTREE_REPO_LIST_OBJECTS_LOOSE = (1 << 0),
  OSTREE_REPO_LIST_OBJECTS_PACKED = (1 << 1),
  OSTREE_REPO_LIST_OBJECTS_ALL = (1 << 2),
  OSTREE_REPO_LIST_OBJECTS_NO_PARENTS = (1 << 3),
} OstreeRepoListObjectsFlags;

/**
 * OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE:
 *
 * b - %TRUE if object is available "loose"
 * as - List of pack file checksums in which this object appears
 */
#define OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE (G_VARIANT_TYPE ("(bas)")

_OSTREE_PUBLIC
gboolean ostree_repo_list_objects (OstreeRepo                  *self,
                                   OstreeRepoListObjectsFlags   flags,
                                   GHashTable                 **out_objects,
                                   GCancellable                *cancellable,
                                   GError                     **error);

_OSTREE_PUBLIC
gboolean ostree_repo_list_commit_objects_starting_with ( OstreeRepo                  *self,
                                                         const char                  *start,
                                                         GHashTable                 **out_commits,
                                                         GCancellable                *cancellable,
                                                         GError                     **error);

_OSTREE_PUBLIC
gboolean ostree_repo_list_static_delta_names (OstreeRepo                  *self,
                                              GPtrArray                  **out_deltas,
                                              GCancellable                *cancellable,
                                              GError                     **error);

_OSTREE_PUBLIC
gboolean ostree_repo_list_static_delta_indexes (OstreeRepo                  *self,
                                                GPtrArray                  **out_indexes,
                                                GCancellable                *cancellable,
                                                GError                     **error);

/**
 * OstreeStaticDeltaGenerateOpt:
 * @OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY: Optimize for speed of delta creation over space
 * @OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR: Optimize for delta size (may be very slow)
 *
 * Parameters controlling optimization of static deltas.
 */
typedef enum {
  OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY,
  OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR
} OstreeStaticDeltaGenerateOpt;

_OSTREE_PUBLIC
gboolean ostree_repo_static_delta_generate (OstreeRepo                   *self,
                                            OstreeStaticDeltaGenerateOpt  opt,
                                            const char                   *from,
                                            const char                   *to,
                                            GVariant                     *metadata,
                                            GVariant                     *params,
                                            GCancellable                 *cancellable,
                                            GError                      **error);

/**
 * OstreeStaticDeltaIndexFlags:
 * @OSTREE_STATIC_DELTA_INDEX_FLAGS_NONE: No special flags
 *
 * Flags controlling static delta index generation.
 */
typedef enum {
  OSTREE_STATIC_DELTA_INDEX_FLAGS_NONE = 0,
} OstreeStaticDeltaIndexFlags;

/**
 * OstreeRepoCommitTraverseFlags:
 * @OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE: No special options for traverse
 * @OSTREE_REPO_COMMIT_TRAVERSE_FLAG_COMMIT_ONLY: Traverse and retrieve only commit objects.  (Since: 2022.2)
 */
typedef enum {
  OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE = (1 << 0),
  OSTREE_REPO_COMMIT_TRAVERSE_FLAG_COMMIT_ONLY = (1 << 1),
} OstreeRepoCommitTraverseFlags;

_OSTREE_PUBLIC
gboolean ostree_repo_static_delta_reindex (OstreeRepo                  *repo,
                                           OstreeStaticDeltaIndexFlags flags,
                                           const char                  *opt_to_commit,
                                           GCancellable                *cancellable,
                                           GError                     **error);

_OSTREE_PUBLIC
gboolean ostree_repo_static_delta_execute_offline_with_signature (OstreeRepo   *self,
                                                                  GFile        *dir_or_file,
                                                                  OstreeSign   *sign,
                                                                  gboolean     skip_validation,
                                                                  GCancellable *cancellable,
                                                                  GError       **error);

_OSTREE_PUBLIC
gboolean ostree_repo_static_delta_execute_offline (OstreeRepo                    *self,
                                                   GFile                         *dir_or_file,
                                                   gboolean                       skip_validation,
                                                   GCancellable                  *cancellable,
                                                   GError                      **error);

_OSTREE_PUBLIC
gboolean ostree_repo_static_delta_verify_signature (OstreeRepo       *self,
                                                    const char       *delta_id,
                                                    OstreeSign       *sign,
                                                    char            **out_success_message,
                                                    GError          **error);

_OSTREE_PUBLIC
GHashTable *ostree_repo_traverse_new_reachable (void);

_OSTREE_PUBLIC
GHashTable *ostree_repo_traverse_new_parents (void);

_OSTREE_PUBLIC
char ** ostree_repo_traverse_parents_get_commits (GHashTable *parents, GVariant *object);

_OSTREE_PUBLIC
gboolean ostree_repo_traverse_commit (OstreeRepo         *repo,
                                      const char         *commit_checksum,
                                      int                 maxdepth,
                                      GHashTable        **out_reachable,
                                      GCancellable       *cancellable,
                                      GError            **error);

_OSTREE_PUBLIC
gboolean ostree_repo_traverse_commit_union (OstreeRepo         *repo,
                                            const char         *commit_checksum,
                                            int                 maxdepth,
                                            GHashTable         *inout_reachable,
                                            GCancellable       *cancellable,
                                            GError            **error);

_OSTREE_PUBLIC
gboolean ostree_repo_traverse_commit_union_with_parents (OstreeRepo         *repo,
                                                         const char         *commit_checksum,
                                                         int                 maxdepth,
                                                         GHashTable         *inout_reachable,
                                                         GHashTable         *inout_parents,
                                                         GCancellable       *cancellable,
                                                         GError            **error);

_OSTREE_PUBLIC
gboolean ostree_repo_traverse_commit_with_flags (OstreeRepo                     *repo,
                                                 OstreeRepoCommitTraverseFlags   flags,
                                                 const char                     *commit_checksum,
                                                 int                             maxdepth,
                                                 GHashTable                     *inout_reachable,
                                                 GHashTable                     *inout_parents,
                                                 GCancellable                   *cancellable,
                                                 GError                        **error);

struct _OstreeRepoCommitTraverseIter {
  gboolean initialized;
  /* 4 byte hole on 64 bit */
  gpointer dummy[10];
  char dummy_checksum_data[(OSTREE_SHA256_STRING_LEN+1)*2];
};

typedef struct _OstreeRepoCommitTraverseIter OstreeRepoCommitTraverseIter;

_OSTREE_PUBLIC
gboolean
ostree_repo_commit_traverse_iter_init_commit (OstreeRepoCommitTraverseIter    *iter,
                                              OstreeRepo                      *repo,
                                              GVariant                        *commit,
                                              OstreeRepoCommitTraverseFlags    flags,
                                              GError                         **error);

_OSTREE_PUBLIC
gboolean
ostree_repo_commit_traverse_iter_init_dirtree (OstreeRepoCommitTraverseIter    *iter,
                                               OstreeRepo                      *repo,
                                               GVariant                        *dirtree,
                                               OstreeRepoCommitTraverseFlags    flags,
                                               GError                         **error);

typedef enum {
  OSTREE_REPO_COMMIT_ITER_RESULT_ERROR,
  OSTREE_REPO_COMMIT_ITER_RESULT_END,
  OSTREE_REPO_COMMIT_ITER_RESULT_FILE,
  OSTREE_REPO_COMMIT_ITER_RESULT_DIR
} OstreeRepoCommitIterResult;

_OSTREE_PUBLIC
OstreeRepoCommitIterResult ostree_repo_commit_traverse_iter_next (OstreeRepoCommitTraverseIter *iter,
                                                                  GCancellable       *cancellable,
                                                                  GError            **error);

_OSTREE_PUBLIC
void ostree_repo_commit_traverse_iter_get_file (OstreeRepoCommitTraverseIter *iter,
                                                char                        **out_name,
                                                char                        **out_checksum);

_OSTREE_PUBLIC
void ostree_repo_commit_traverse_iter_get_dir (OstreeRepoCommitTraverseIter *iter,
                                               char                        **out_name,
                                               char                        **out_content_checksum,
                                               char                        **out_meta_checksum);

_OSTREE_PUBLIC
void ostree_repo_commit_traverse_iter_clear (OstreeRepoCommitTraverseIter *iter);

_OSTREE_PUBLIC
void ostree_repo_commit_traverse_iter_cleanup (void *p);

#define ostree_cleanup_repo_commit_traverse_iter __attribute__ ((cleanup(ostree_repo_commit_traverse_iter_cleanup)))

/**
 * OstreeRepoPruneFlags:
 * @OSTREE_REPO_PRUNE_FLAGS_NONE: No special options for pruning
 * @OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE: Don't actually delete objects
 * @OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY: Do not traverse individual commit objects, only follow refs
 * @OSTREE_REPO_PRUNE_FLAGS_COMMIT_ONLY: Only traverse commit objects.  (Since 2022.2)
 */
typedef enum {
  OSTREE_REPO_PRUNE_FLAGS_NONE = 0,
  OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE = (1 << 0),
  OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY = (1 << 1),
  OSTREE_REPO_PRUNE_FLAGS_COMMIT_ONLY = (1 << 2),
} OstreeRepoPruneFlags;

_OSTREE_PUBLIC
gboolean
ostree_repo_prune_static_deltas (OstreeRepo *self, const char *commit,
                                 GCancellable      *cancellable,
                                 GError           **error);

_OSTREE_PUBLIC
gboolean ostree_repo_prune (OstreeRepo        *self,
                            OstreeRepoPruneFlags   flags,
                            gint               depth,
                            gint              *out_objects_total,
                            gint              *out_objects_pruned,
                            guint64           *out_pruned_object_size_total,
                            GCancellable      *cancellable,
                            GError           **error);

struct _OstreeRepoPruneOptions {
  OstreeRepoPruneFlags flags;

  /* 4 byte hole on 64 bit */

  GHashTable *reachable; /* Set<GVariant> (object names) */

  gboolean unused_bools[6];
  int unused_ints[6];
  gpointer unused_ptrs[7];
};

typedef struct _OstreeRepoPruneOptions OstreeRepoPruneOptions;

_OSTREE_PUBLIC
gboolean
ostree_repo_traverse_reachable_refs (OstreeRepo *self,
                                     guint       depth,
                                     GHashTable *reachable,
                                     GCancellable *cancellable,
                                     GError      **error);

_OSTREE_PUBLIC
gboolean ostree_repo_prune_from_reachable (OstreeRepo             *self,
                                           OstreeRepoPruneOptions *options,
                                           gint              *out_objects_total,
                                           gint              *out_objects_pruned,
                                           guint64           *out_pruned_object_size_total,
                                           GCancellable           *cancellable,
                                           GError              **error);

/**
 * OstreeRepoPullFlags:
 * @OSTREE_REPO_PULL_FLAGS_NONE: No special options for pull
 * @OSTREE_REPO_PULL_FLAGS_MIRROR: Write out refs suitable for mirrors and fetch all refs if none requested
 * @OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY: Fetch only the commit metadata
 * @OSTREE_REPO_PULL_FLAGS_UNTRUSTED: Do verify checksums of local (filesystem-accessible) repositories (defaults on for HTTP)
 * @OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES: Since 2017.7.  Reject writes of content objects with modes outside of 0775.
 * @OSTREE_REPO_PULL_FLAGS_TRUSTED_HTTP: Don't verify checksums of objects HTTP repositories (Since: 2017.12)
 */
typedef enum {
  OSTREE_REPO_PULL_FLAGS_NONE,
  OSTREE_REPO_PULL_FLAGS_MIRROR = (1 << 0),
  OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY = (1 << 1),
  OSTREE_REPO_PULL_FLAGS_UNTRUSTED = (1 << 2),
  OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES = (1 << 3),
  OSTREE_REPO_PULL_FLAGS_TRUSTED_HTTP = (1 << 4),
} OstreeRepoPullFlags;

_OSTREE_PUBLIC
gboolean ostree_repo_pull (OstreeRepo             *self,
                           const char             *remote_name,
                           char                  **refs_to_fetch,
                           OstreeRepoPullFlags     flags,
                           OstreeAsyncProgress    *progress,
                           GCancellable           *cancellable,
                           GError                **error);

_OSTREE_PUBLIC
gboolean
ostree_repo_pull_one_dir (OstreeRepo               *self,
                          const char               *remote_name,
                          const char               *dir_to_pull,
                          char                    **refs_to_fetch,
                          OstreeRepoPullFlags       flags,
                          OstreeAsyncProgress      *progress,
                          GCancellable             *cancellable,
                          GError                  **error);

_OSTREE_PUBLIC
gboolean ostree_repo_pull_with_options (OstreeRepo             *self,
                                        const char             *remote_name_or_baseurl,
                                        GVariant               *options,
                                        OstreeAsyncProgress    *progress,
                                        GCancellable           *cancellable,
                                        GError                **error);

_OSTREE_PUBLIC
void ostree_repo_find_remotes_async (OstreeRepo                         *self,
                                     const OstreeCollectionRef * const  *refs,
                                     GVariant                           *options,
                                     OstreeRepoFinder                  **finders,
                                     OstreeAsyncProgress                *progress,
                                     GCancellable                       *cancellable,
                                     GAsyncReadyCallback                 callback,
                                     gpointer                            user_data);
_OSTREE_PUBLIC
OstreeRepoFinderResult **ostree_repo_find_remotes_finish (OstreeRepo    *self,
                                                          GAsyncResult  *result,
                                                          GError       **error);

_OSTREE_PUBLIC
void ostree_repo_pull_from_remotes_async (OstreeRepo                           *self,
                                          const OstreeRepoFinderResult * const *results,
                                          GVariant                             *options,
                                          OstreeAsyncProgress                  *progress,
                                          GCancellable                         *cancellable,
                                          GAsyncReadyCallback                   callback,
                                          gpointer                              user_data);
_OSTREE_PUBLIC
gboolean ostree_repo_pull_from_remotes_finish (OstreeRepo    *self,
                                               GAsyncResult  *result,
                                               GError       **error);

_OSTREE_PUBLIC
OstreeRemote *ostree_repo_resolve_keyring_for_collection (OstreeRepo    *self,
                                                          const gchar   *collection_id,
                                                          GCancellable  *cancellable,
                                                          GError       **error);

_OSTREE_PUBLIC
gboolean ostree_repo_list_collection_refs (OstreeRepo                 *self,
                                           const char                 *match_collection_id,
                                           GHashTable                 **out_all_refs,
                                           OstreeRepoListRefsExtFlags flags,
                                           GCancellable               *cancellable,
                                           GError                     **error);

_OSTREE_PUBLIC
void ostree_repo_pull_default_console_progress_changed (OstreeAsyncProgress *progress,
                                                        gpointer             user_data);

_OSTREE_PUBLIC
gboolean ostree_repo_sign_commit (OstreeRepo     *self,
                                  const gchar    *commit_checksum,
                                  const gchar    *key_id,
                                  const gchar    *homedir,
                                  GCancellable   *cancellable,
                                  GError        **error);

_OSTREE_PUBLIC
gboolean ostree_repo_sign_delta (OstreeRepo     *self,
                                 const gchar    *from_commit,
                                 const gchar    *to_commit,
                                 const gchar    *key_id,
                                 const gchar    *homedir,
                                 GCancellable   *cancellable,
                                 GError        **error);


_OSTREE_PUBLIC
gboolean ostree_repo_verify_commit (OstreeRepo   *self,
                                    const gchar  *commit_checksum,
                                    GFile        *keyringdir,
                                    GFile        *extra_keyring,
                                    GCancellable *cancellable,
                                    GError      **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_remote_get_gpg_verify (OstreeRepo  *self,
                                                 const char  *name,
                                                 gboolean    *out_gpg_verify,
                                                 GError     **error);

_OSTREE_PUBLIC
gboolean      ostree_repo_remote_get_gpg_verify_summary (OstreeRepo  *self,
                                                         const char  *name,
                                                         gboolean    *out_gpg_verify_summary,
                                                         GError     **error);

/**
 * OSTREE_GPG_KEY_GVARIANT_FORMAT:
 *
 * - aa{sv} - Array of subkeys. Each a{sv} dictionary represents a
 *   subkey. The primary key is the first subkey. The following keys are
 *   currently recognized:
 *   - key: `fingerprint`, value: `s`, key fingerprint hexadecimal string
 *   - key: `created`, value: `x`, key creation timestamp (seconds since
 *     the Unix epoch in UTC, big-endian)
 *   - key: `expires`, value: `x`, key expiration timestamp (seconds since
 *     the Unix epoch in UTC, big-endian). If this value is 0, the key does
 *     not expire.
 *   - key: `revoked`, value: `b`, whether key is revoked
 *   - key: `expired`, value: `b`, whether key is expired
 *   - key: `invalid`, value: `b`, whether key is invalid
 * - aa{sv} - Array of user IDs. Each a{sv} dictionary represents a
 *   user ID. The following keys are currently recognized:
 *   - key: `uid`, value: `s`, full user ID (name, email and comment)
 *   - key: `name`, value: `s`, user ID name component
 *   - key: `comment`, value: `s`, user ID comment component
 *   - key: `email`, value: `s`, user ID email component
 *   - key: `revoked`, value: `b`, whether user ID is revoked
 *   - key: `invalid`, value: `b`, whether user ID is invalid
 *   - key: `advanced_url`, value: `ms`, advanced WKD update URL
 *   - key: `direct_url`, value: `ms`, direct WKD update URL
 * - a{sv} - Additional metadata dictionary. There are currently no
 *   additional metadata keys defined.
 *
 * Since: 2021.4
 */
#define OSTREE_GPG_KEY_GVARIANT_STRING "(aa{sv}aa{sv}a{sv})"
#define OSTREE_GPG_KEY_GVARIANT_FORMAT G_VARIANT_TYPE (OSTREE_GPG_KEY_GVARIANT_STRING)

_OSTREE_PUBLIC
gboolean      ostree_repo_remote_get_gpg_keys (OstreeRepo          *self,
                                               const char          *name,
                                               const char * const  *key_ids,
                                               GPtrArray          **out_keys,
                                               GCancellable        *cancellable,
                                               GError             **error);

_OSTREE_PUBLIC
gboolean ostree_repo_remote_gpg_import (OstreeRepo         *self,
                                        const char         *name,
                                        GInputStream       *source_stream,
                                        const char * const *key_ids,
                                        guint              *out_imported,
                                        GCancellable       *cancellable,
                                        GError            **error);

_OSTREE_PUBLIC
gboolean ostree_repo_add_gpg_signature_summary (OstreeRepo     *self,
                                                const gchar    **key_id,
                                                const gchar    *homedir,
                                                GCancellable   *cancellable,
                                                GError        **error);

_OSTREE_PUBLIC
gboolean ostree_repo_append_gpg_signature (OstreeRepo     *self,
                                           const gchar    *commit_checksum,
                                           GBytes         *signature_bytes,
                                           GCancellable   *cancellable,
                                           GError        **error);

_OSTREE_PUBLIC
gboolean ostree_repo_gpg_sign_data (OstreeRepo     *self,
                                    GBytes         *data,
                                    GBytes         *old_signatures,
                                    const gchar   **key_id,
                                    const gchar    *homedir,
                                    GBytes        **out_signatures,
                                    GCancellable   *cancellable,
                                    GError        **error);

_OSTREE_PUBLIC
OstreeGpgVerifyResult * ostree_repo_verify_commit_ext (OstreeRepo    *self,
                                                       const gchar   *commit_checksum,
                                                       GFile         *keyringdir,
                                                       GFile         *extra_keyring,
                                                       GCancellable  *cancellable,
                                                       GError       **error);

_OSTREE_PUBLIC
OstreeGpgVerifyResult *
ostree_repo_verify_commit_for_remote (OstreeRepo    *self,
                                      const gchar   *commit_checksum,
                                      const gchar   *remote_name,
                                      GCancellable  *cancellable,
                                      GError       **error);

_OSTREE_PUBLIC
OstreeGpgVerifyResult * ostree_repo_gpg_verify_data (OstreeRepo    *self,
                                                     const gchar   *remote_name,
                                                     GBytes        *data,
                                                     GBytes        *signatures,
                                                     GFile         *keyringdir,
                                                     GFile         *extra_keyring,
                                                     GCancellable  *cancellable,
                                                     GError       **error);

_OSTREE_PUBLIC
OstreeGpgVerifyResult * ostree_repo_verify_summary (OstreeRepo    *self,
                                                    const char    *remote_name,
                                                    GBytes        *summary,
                                                    GBytes        *signatures,
                                                    GCancellable  *cancellable,
                                                    GError       **error);

/**
 * OstreeRepoVerifyFlags:
 * @OSTREE_REPO_VERIFY_FLAGS_NONE: No flags
 * @OSTREE_REPO_VERIFY_FLAGS_NO_GPG: Skip GPG verification
 * @OSTREE_REPO_VERIFY_FLAGS_NO_SIGNAPI: Skip all other signature verification methods
 * 
 * Since: 2021.4
 */
typedef enum {
  OSTREE_REPO_VERIFY_FLAGS_NONE = 0,
  OSTREE_REPO_VERIFY_FLAGS_NO_GPG = (1 << 0),
  OSTREE_REPO_VERIFY_FLAGS_NO_SIGNAPI = (1 << 1),
} OstreeRepoVerifyFlags;

_OSTREE_PUBLIC
gboolean ostree_repo_signature_verify_commit_data (OstreeRepo    *self,
                                                   const char    *remote_name,
                                                   GBytes        *commit_data,
                                                   GBytes        *commit_metadata,
                                                   OstreeRepoVerifyFlags flags,
                                                   char         **out_results,
                                                   GError       **error);

_OSTREE_PUBLIC
gboolean ostree_repo_regenerate_summary (OstreeRepo     *self,
                                         GVariant       *additional_metadata,
                                         GCancellable   *cancellable,
                                         GError        **error);


/**
 * OstreeRepoLockType:
 * @OSTREE_REPO_LOCK_SHARED: A "read only" lock; multiple readers are allowed.
 * @OSTREE_REPO_LOCK_EXCLUSIVE: A writable lock at most one writer can be active, and zero readers.
 *
 * Flags controlling repository locking.
 *
 * Since: 2021.3
 */
typedef enum {
  OSTREE_REPO_LOCK_SHARED,
  OSTREE_REPO_LOCK_EXCLUSIVE
} OstreeRepoLockType;

_OSTREE_PUBLIC
gboolean      ostree_repo_lock_push (OstreeRepo          *self,
                                     OstreeRepoLockType   lock_type,
                                     GCancellable        *cancellable,
                                     GError             **error);
_OSTREE_PUBLIC
gboolean      ostree_repo_lock_pop (OstreeRepo          *self,
                                    OstreeRepoLockType   lock_type,
                                    GCancellable        *cancellable,
                                    GError             **error);

/* C convenience API only */
#ifndef __GI_SCANNER__

/**
 * OstreeRepoAutoLock: (skip)
 *
 * An opaque type for use with ostree_repo_auto_lock_push().
 *
 * Since: 2021.3
 */
typedef struct OstreeRepoAutoLock OstreeRepoAutoLock;

_OSTREE_PUBLIC
OstreeRepoAutoLock * ostree_repo_auto_lock_push (OstreeRepo          *self,
                                                 OstreeRepoLockType   lock_type,
                                                 GCancellable        *cancellable,
                                                 GError             **error);

_OSTREE_PUBLIC
void ostree_repo_auto_lock_cleanup (OstreeRepoAutoLock *lock);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoAutoLock, ostree_repo_auto_lock_cleanup)

#endif


/**
 * OSTREE_REPO_METADATA_REF:
 *
 * The name of a ref which is used to store metadata for the entire repository,
 * such as its expected update time (`ostree.summary.expires`), name, or new
 * GPG keys. Metadata is stored on contentless commits in the ref, and hence is
 * signed with the commits.
 *
 * This supersedes the additional metadata dictionary in the `summary` file
 * (see ostree_repo_regenerate_summary()), as the use of a ref means that the
 * metadata for multiple upstream repositories can be included in a single mirror
 * repository, disambiguating the refs using collection IDs. In order to support
 * peer to peer redistribution of repository metadata, repositories must set a
 * collection ID (ostree_repo_set_collection_id()).
 *
 * Users of OSTree may place arbitrary metadata in commits on this ref, but the
 * keys must be namespaced by product or developer. For example,
 * `exampleos.end-of-life`. The `ostree.` prefix is reserved.
 *
 * Since: 2018.6
 */
#define OSTREE_REPO_METADATA_REF "ostree-metadata"

/**
 * OSTREE_META_KEY_DEPLOY_COLLECTION_ID:
 *
 * GVariant type `s`. This key can be used in the repo metadata which is stored
 * in OSTREE_REPO_METADATA_REF as well as in the summary. The semantics of this
 * are that the remote repository wants clients to update their remote config
 * to add this collection ID (clients can't do P2P operations involving a
 * remote without a collection ID configured on it, even if one is configured
 * on the server side). Clients must never change or remove a collection ID
 * already set in their remote config.
 *
 * Currently, OSTree does not implement changing a remote config based on this
 * key, but it may do so in a later release, and until then clients such as
 * Flatpak may implement it.
 *
 * This is a replacement for the similar metadata key implemented by flatpak,
 * `xa.collection-id`, which is now deprecated as clients which supported it had
 * bugs with their P2P implementations.
 *
 * Since: 2018.9
 */
#define OSTREE_META_KEY_DEPLOY_COLLECTION_ID "ostree.deploy-collection-id"

G_END_DECLS


/* Include here as the functions defined before should not depend on anything which
   is defined in -deprecated.h.  */
#include "ostree-repo-deprecated.h"
