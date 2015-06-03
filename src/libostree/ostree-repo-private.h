/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
 */

#pragma once

#include "ostree-repo.h"

G_BEGIN_DECLS

#define OSTREE_DELTAPART_VERSION (0)

#define _OSTREE_OBJECT_SIZES_ENTRY_SIGNATURE "ay"

/**
 * OstreeRepo:
 *
 * Private instance structure.
 */
struct OstreeRepo {
  GObject parent;

  char *boot_id;
  int commit_stagedir_fd;
  char *commit_stagedir_name;

  GFile *repodir;
  int    repo_dir_fd;
  GFile *tmp_dir;
  int    tmp_dir_fd;
  GFile *local_heads_dir;
  GFile *remote_heads_dir;
  GFile *objects_dir;
  GFile *state_dir;
  int objects_dir_fd;
  GFile *deltas_dir;
  GFile *uncompressed_objects_dir;
  int uncompressed_objects_dir_fd;
  GFile *config_file;

  GFile *transaction_lock_path;
  GHashTable *txn_refs;
  GMutex txn_stats_lock;
  OstreeRepoTransactionStats txn_stats;

  GMutex cache_lock;
  GPtrArray *cached_meta_indexes;
  GPtrArray *cached_content_indexes;

  gboolean inited;
  gboolean writable;
  GError *writable_error;
  gboolean in_transaction;
  gboolean disable_fsync;
  GHashTable *loose_object_devino_hash;
  GHashTable *updated_uncompressed_dirs;
  GHashTable *object_sizes;

  uid_t target_owner_uid;
  gid_t target_owner_gid;

  GKeyFile *config;
  GHashTable *remotes;
  GMutex remotes_lock;
  OstreeRepoMode mode;
  gboolean enable_uncompressed_cache;
  gboolean generate_sizes;

  OstreeRepo *parent_repo;
};

gboolean
_ostree_repo_ensure_loose_objdir_at (int             dfd,
                                     const char     *loose_path,
                                     GCancellable   *cancellable,
                                     GError        **error);

gboolean
_ostree_repo_find_object (OstreeRepo           *self,
                          OstreeObjectType      objtype,
                          const char           *checksum,
                          GFile               **out_stored_path,
                          GCancellable         *cancellable,
                          GError             **error);

GFile *
_ostree_repo_get_commit_metadata_loose_path (OstreeRepo        *self,
                                             const char        *checksum);

GFile *
_ostree_repo_get_commit_compat_signature_loose_path (OstreeRepo  *self,
                                                     const char  *checksum);

gboolean
_ostree_repo_read_commit_compat_signature (OstreeRepo      *self,
                                           const char      *checksum,
                                           GBytes         **out_signature,
                                           GCancellable    *cancellable,
                                           GError         **error);

gboolean
_ostree_repo_write_commit_compat_signature (OstreeRepo      *self,
                                            const char      *checksum,
                                            GBytes          *signature,
                                            GCancellable    *cancellable,
                                            GError         **error);

GFile *
_ostree_repo_get_commit_compat_sizes_loose_path (OstreeRepo  *self,
                                                 const char  *checksum);

gboolean
_ostree_repo_read_commit_compat_sizes (OstreeRepo      *self,
                                       const char      *checksum,
                                       GVariant       **out_sizes,
                                       GCancellable    *cancellable,
                                       GError         **error);

gboolean
_ostree_repo_write_commit_compat_sizes (OstreeRepo      *self,
                                        const char      *checksum,
                                        GVariant        *sizes,
                                        GCancellable    *cancellable,
                                        GError         **error);

gboolean
_ostree_repo_has_loose_object (OstreeRepo           *self,
                               const char           *checksum,
                               OstreeObjectType      objtype,
                               gboolean             *out_is_stored,
                               char                 *loose_path_buf,
                               GFile               **out_stored_path,
                               GCancellable         *cancellable,
                               GError             **error);

gboolean
_ostree_repo_write_directory_meta (OstreeRepo   *self,
                                   GFileInfo    *file_info,
                                   GVariant     *xattrs,
                                   guchar      **out_csum,
                                   GCancellable *cancellable,
                                   GError      **error);
gboolean
_ostree_repo_update_refs (OstreeRepo        *self,
                          GHashTable        *refs,
                          GCancellable      *cancellable,
                          GError           **error);

gboolean      
_ostree_repo_file_replace_contents (OstreeRepo    *self,
                                    int            dfd,
                                    const char    *path,
                                    const guint8  *buf,
                                    gsize          len,
                                    GCancellable  *cancellable,
                                    GError       **error);

gboolean      
_ostree_repo_write_ref (OstreeRepo    *self,
                        const char    *remote,
                        const char    *ref,
                        const char    *rev,
                        GCancellable  *cancellable,
                        GError       **error);

OstreeRepoFile *
_ostree_repo_file_new_for_commit (OstreeRepo  *repo,
                                  const char  *commit,
                                  GError     **error);

OstreeRepoFile *
_ostree_repo_file_new_root (OstreeRepo  *repo,
                            const char  *contents_checksum,
                            const char  *metadata_checksum);

gboolean
_ostree_repo_traverse_dirtree_internal (OstreeRepo      *repo,
                                        const char      *dirtree_checksum,
                                        int              recursion_depth,
                                        GHashTable      *inout_reachable,
                                        GHashTable      *inout_content_names,
                                        GCancellable    *cancellable,
                                        GError         **error);

OstreeRepoCommitFilterResult
_ostree_repo_commit_modifier_apply (OstreeRepo               *self,
                                    OstreeRepoCommitModifier *modifier,
                                    const char               *path,
                                    GFileInfo                *file_info,
                                    GFileInfo               **out_modified_info);

gboolean
_ostree_repo_remote_name_is_file (const char *remote_name);

gboolean
_ostree_repo_get_remote_option (OstreeRepo  *self,
                                const char  *remote_name,
                                const char  *option_name,
                                const char  *default_value,
                                char       **out_value,
                                GError     **error);

gboolean
_ostree_repo_get_remote_list_option (OstreeRepo   *self,
                                     const char   *remote_name,
                                     const char   *option_name,
                                     char       ***out_value,
                                     GError      **error);

gboolean
_ostree_repo_get_remote_boolean_option (OstreeRepo  *self,
                                        const char  *remote_name,
                                        const char  *option_name,
                                        gboolean     default_value,
                                        gboolean    *out_value,
                                        GError     **error);

OstreeGpgVerifyResult *
_ostree_repo_gpg_verify_with_metadata (OstreeRepo          *self,
                                       GBytes              *signed_data,
                                       GVariant            *metadata,
                                       GFile               *keyringdir,
                                       GFile               *extra_keyring,
                                       GCancellable        *cancellable,
                                       GError             **error);

gboolean
_ostree_repo_commit_loose_final (OstreeRepo        *self,
                                 const char        *checksum,
                                 OstreeObjectType   objtype,
                                 int                temp_dfd,
                                 const char        *temp_filename,
                                 GCancellable      *cancellable,
                                 GError           **error);

typedef struct {
  int fd;
  char *temp_filename;
} OstreeRepoTrustedContentBareCommit;

gboolean
_ostree_repo_open_trusted_content_bare (OstreeRepo          *self,
                                        const char          *checksum,
                                        guint64              content_len,
                                        OstreeRepoTrustedContentBareCommit *out_state,
                                        GOutputStream      **out_stream,
                                        gboolean            *out_have_object,
                                        GCancellable        *cancellable,
                                        GError             **error);

gboolean
_ostree_repo_commit_trusted_content_bare (OstreeRepo          *self,
                                          const char          *checksum,
                                          OstreeRepoTrustedContentBareCommit *state,
                                          guint32              uid,
                                          guint32              gid,
                                          guint32              mode,
                                          GVariant            *xattrs,
                                          GCancellable        *cancellable,
                                          GError             **error);

gboolean
_ostree_repo_read_bare_fd (OstreeRepo           *self,
                           const char           *checksum,
                           int                  *out_fd,
                           GCancellable        *cancellable,
                           GError             **error);
                           
G_END_DECLS
