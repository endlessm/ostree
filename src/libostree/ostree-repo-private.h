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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <sys/statvfs.h>
#include "otutil.h"
#include "ostree-ref.h"
#include "ostree-repo.h"
#include "ostree-remote-private.h"

G_BEGIN_DECLS

#define OSTREE_DELTAPART_VERSION (0)

#define _OSTREE_OBJECT_SIZES_ENTRY_SIGNATURE "ay"

#define _OSTREE_SUMMARY_CACHE_DIR "summaries"
#define _OSTREE_CACHE_DIR "cache"

#define _OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS 8
#define _OSTREE_MAX_OUTSTANDING_DELTAPART_REQUESTS 2

/* In most cases, writing to disk should be much faster than
 * fetching from the network, so we shouldn't actually hit
 * this. But if using pipelining and e.g. pulling over LAN
 * (or writing to slow media), we can have a runaway
 * situation towards EMFILE.
 * */
#define _OSTREE_MAX_OUTSTANDING_WRITE_REQUESTS 16

/* Well-known keys for the additional metadata field in a summary file. */
#define OSTREE_SUMMARY_LAST_MODIFIED "ostree.summary.last-modified"
#define OSTREE_SUMMARY_EXPIRES "ostree.summary.expires"
#define OSTREE_SUMMARY_COLLECTION_ID "ostree.summary.collection-id"
#define OSTREE_SUMMARY_COLLECTION_MAP "ostree.summary.collection-map"

#define _OSTREE_PAYLOAD_LINK_PREFIX "../"
#define _OSTREE_PAYLOAD_LINK_PREFIX_LEN (sizeof (_OSTREE_PAYLOAD_LINK_PREFIX) - 1)

/* Well-known keys for the additional metadata field in a commit in a ref entry
 * in a summary file. */
#define OSTREE_COMMIT_TIMESTAMP "ostree.commit.timestamp"

typedef enum {
  OSTREE_REPO_TEST_ERROR_PRE_COMMIT = (1 << 0),
  OSTREE_REPO_TEST_ERROR_INVALID_CACHE = (1 << 1),
} OstreeRepoTestErrorFlags;

struct OstreeRepoCommitModifier {
  volatile gint refcount;

  OstreeRepoCommitModifierFlags flags;
  OstreeRepoCommitFilter filter;
  gpointer user_data;
  GDestroyNotify destroy_notify;

  OstreeRepoCommitModifierXattrCallback xattr_callback;
  GDestroyNotify xattr_destroy;
  gpointer xattr_user_data;

  OstreeSePolicy *sepolicy;
  GHashTable *devino_cache;
};

typedef enum {
  OSTREE_REPO_SYSROOT_KIND_UNKNOWN,
  OSTREE_REPO_SYSROOT_KIND_NO,  /* Not a system repo */
  OSTREE_REPO_SYSROOT_KIND_VIA_SYSROOT, /* Constructed via ostree_sysroot_get_repo() */
  OSTREE_REPO_SYSROOT_KIND_IS_SYSROOT_OSTREE, /* We match /ostree/repo */
} OstreeRepoSysrootKind;

typedef struct {
  GHashTable *refs;  /* (element-type utf8 utf8) */
  GHashTable *collection_refs;  /* (element-type OstreeCollectionRef utf8) */
  OstreeRepoTransactionStats stats;
  /* Implementation of min-free-space-percent */
  gulong blocksize;
  fsblkcnt_t max_blocks;
} OstreeRepoTxn;

/**
 * OstreeRepo:
 *
 * Private instance structure.
 */
struct OstreeRepo {
  GObject parent;

  char *stagedir_prefix;
  GLnxTmpDir commit_stagedir;
  GLnxLockFile commit_stagedir_lock;

  /* A cached fd-relative version, distinct from the case where we may have a
   * user-provided absolute path.
   */
  GFile *repodir_fdrel;
  GFile *repodir; /* May be %NULL if we were opened via ostree_repo_open_at() */
  int    repo_dir_fd;
  int    tmp_dir_fd;
  int    cache_dir_fd;
  char  *cache_dir;
  int objects_dir_fd;
  int uncompressed_objects_dir_fd;
  GFile *sysroot_dir;
  GWeakRef sysroot; /* Weak to avoid a circular ref; see also `is_system` */
  char *remotes_config_dir;

  GMutex txn_lock;
  OstreeRepoTxn txn;
  gboolean txn_locked;

  GMutex cache_lock;
  guint dirmeta_cache_refcount;
  /* char * checksum → GVariant * for dirmeta objects, used in the checkout path */
  GHashTable *dirmeta_cache;

  gboolean inited;
  gboolean writable;
  OstreeRepoSysrootKind sysroot_kind;
  GError *writable_error;
  gboolean in_transaction;
  gboolean disable_fsync;
  gboolean disable_min_free_space_check;
  gboolean disable_xattrs;
  guint zlib_compression_level;
  GHashTable *loose_object_devino_hash;
  GHashTable *updated_uncompressed_dirs;
  GHashTable *object_sizes;

  /* Cache the repo's device/inode to use for comparisons elsewhere */
  dev_t device;
  ino_t inode;
  uid_t owner_uid; /* Cache of repo's owner uid */
  uid_t target_owner_uid; /* Ensure files are chowned to this uid/gid */
  gid_t target_owner_gid;
  guint min_free_space_percent; /* See the min-free-space-percent config option */
  guint64 min_free_space_mb; /* See the min-free-space-size config option */
  guint64 reserved_blocks;
  gboolean cleanup_stagedir;

  guint test_error_flags; /* OstreeRepoTestErrorFlags */

  GKeyFile *config;
  GHashTable *remotes;
  GMutex remotes_lock;
  OstreeRepoMode mode;
  gboolean enable_uncompressed_cache;
  gboolean generate_sizes;
  guint64 tmp_expiry_seconds;
  gchar *collection_id;
  gboolean add_remotes_config_dir; /* Add new remotes in remotes.d dir */
  gint lock_timeout_seconds;
  guint64 payload_link_threshold;
  gint fs_support_reflink; /* The underlying filesystem has support for ioctl (FICLONE..) */

  OstreeRepo *parent_repo;
};

/* Taken from flatpak; may be made into public API later */
typedef OstreeRepo _OstreeRepoAutoTransaction;
static inline void
_ostree_repo_auto_transaction_cleanup (void *p)
{
  OstreeRepo *repo = p;
  if (repo)
    (void) ostree_repo_abort_transaction (repo, NULL, NULL);
}

static inline _OstreeRepoAutoTransaction *
_ostree_repo_auto_transaction_start (OstreeRepo     *repo,
                                     GCancellable   *cancellable,
                                     GError        **error)
{
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return NULL;
  return (_OstreeRepoAutoTransaction *)repo;
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (_OstreeRepoAutoTransaction, _ostree_repo_auto_transaction_cleanup)

typedef struct {
  dev_t dev;
  ino_t ino;
  char checksum[OSTREE_SHA256_STRING_LEN+1];
} OstreeDevIno;

/* A MemoryCacheRef is an in-memory cache of objects (currently just DIRMETA).  This can
 * be used when performing an operation that traverses a repository in someway.  Currently,
 * the primary use case is ostree_repo_checkout_at() avoiding lots of duplicate dirmeta
 * lookups.
 */
typedef struct {
  OstreeRepo *repo;
} OstreeRepoMemoryCacheRef;


void
_ostree_repo_memory_cache_ref_init (OstreeRepoMemoryCacheRef *state,
                                    OstreeRepo               *repo);

void
_ostree_repo_memory_cache_ref_destroy (OstreeRepoMemoryCacheRef *state);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(OstreeRepoMemoryCacheRef, _ostree_repo_memory_cache_ref_destroy)

#define OSTREE_REPO_TMPDIR_STAGING "staging-"

gboolean
_ostree_repo_allocate_tmpdir (int           tmpdir_dfd,
                              const char   *tmpdir_prefix,
                              GLnxTmpDir   *tmpdir_out,
                              GLnxLockFile *file_lock_out,
                              gboolean *    reusing_dir_out,
                              GCancellable *cancellable,
                              GError      **error);

gboolean
_ostree_repo_is_locked_tmpdir (const char *filename);

gboolean
_ostree_repo_try_lock_tmpdir (int            tmpdir_dfd,
                              const char    *tmpdir_name,
                              GLnxLockFile  *file_lock_out,
                              gboolean      *out_did_lock,
                              GError       **error);

gboolean
_ostree_repo_ensure_loose_objdir_at (int             dfd,
                                     const char     *loose_path,
                                     GCancellable   *cancellable,
                                     GError        **error);

GFile *
_ostree_repo_get_commit_metadata_loose_path (OstreeRepo        *self,
                                             const char        *checksum);

gboolean
_ostree_repo_has_loose_object (OstreeRepo           *self,
                               const char           *checksum,
                               OstreeObjectType      objtype,
                               gboolean             *out_is_stored,
                               GCancellable         *cancellable,
                               GError             **error);

gboolean
_ostree_write_bareuser_metadata (int fd,
                                 guint32       uid,
                                 guint32       gid,
                                 guint32       mode,
                                 GVariant     *xattrs,
                                 GError       **error);

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
_ostree_repo_update_collection_refs (OstreeRepo    *self,
                                     GHashTable    *refs,
                                     GCancellable  *cancellable,
                                     GError       **error);

gboolean
_ostree_repo_file_replace_contents (OstreeRepo    *self,
                                    int            dfd,
                                    const char    *path,
                                    const guint8  *buf,
                                    gsize          len,
                                    GCancellable  *cancellable,
                                    GError       **error);

gboolean
_ostree_repo_write_ref (OstreeRepo                 *self,
                        const char                 *remote,
                        const OstreeCollectionRef  *ref,
                        const char                 *rev,
                        const char                 *alias,
                        GCancellable               *cancellable,
                        GError                    **error);

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

OstreeGpgVerifyResult *
_ostree_repo_gpg_verify_with_metadata (OstreeRepo          *self,
                                       GBytes              *signed_data,
                                       GVariant            *metadata,
                                       const char          *remote_name,
                                       GFile               *keyringdir,
                                       GFile               *extra_keyring,
                                       GCancellable        *cancellable,
                                       GError             **error);

OstreeGpgVerifyResult *
_ostree_repo_verify_commit_internal (OstreeRepo    *self,
                                     const char    *commit_checksum,
                                     const char    *remote_name,
                                     GFile         *keyringdir,
                                     GFile         *extra_keyring,
                                     GCancellable  *cancellable,
                                     GError       **error);

typedef enum {
  _OSTREE_REPO_IMPORT_FLAGS_NONE,
  _OSTREE_REPO_IMPORT_FLAGS_TRUSTED,
  _OSTREE_REPO_IMPORT_FLAGS_VERIFY_BAREUSERONLY,
} OstreeRepoImportFlags;

gboolean
_ostree_repo_import_object (OstreeRepo           *self,
                            OstreeRepo           *source,
                            OstreeObjectType      objtype,
                            const char           *checksum,
                            OstreeRepoImportFlags flags,
                            GCancellable         *cancellable,
                            GError              **error);

gboolean
_ostree_repo_commit_tmpf_final (OstreeRepo        *self,
                                const char        *checksum,
                                OstreeObjectType   objtype,
                                GLnxTmpfile       *tmpf,
                                GCancellable      *cancellable,
                                GError           **error);

typedef struct {
  gboolean initialized;
  gpointer opaque0[10];
  guint opaque1[10];
} OstreeRepoBareContent;
void _ostree_repo_bare_content_cleanup (OstreeRepoBareContent *regwrite);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(OstreeRepoBareContent, _ostree_repo_bare_content_cleanup)

gboolean
_ostree_repo_bare_content_open (OstreeRepo            *self,
                                const char            *checksum,
                                guint64                content_len,
                                guint                  uid,
                                guint                  gid,
                                guint                  mode,
                                GVariant              *xattrs,
                                OstreeRepoBareContent *out_regwrite,
                                GCancellable        *cancellable,
                                GError             **error);

gboolean
_ostree_repo_bare_content_write (OstreeRepo                 *repo,
                                 OstreeRepoBareContent      *barewrite,
                                 const guint8               *buf,
                                 size_t                      len,
                                 GCancellable               *cancellable,
                                 GError                    **error);

gboolean
_ostree_repo_bare_content_commit (OstreeRepo                 *self,
                                  OstreeRepoBareContent      *barewrite,
                                  char                       *checksum_buf,
                                  size_t                      buflen,
                                  GCancellable               *cancellable,
                                  GError                    **error);

gboolean
_ostree_repo_load_file_bare (OstreeRepo         *self,
                             const char         *checksum,
                             int                *out_fd,
                             struct stat        *out_stbuf,
                             char              **out_symlink,
                             GVariant          **out_xattrs,
                             GCancellable       *cancellable,
                             GError            **error);

gboolean
_ostree_repo_update_mtime (OstreeRepo        *self,
                           GError           **error);

gboolean
_ostree_repo_add_remote (OstreeRepo   *self,
                         OstreeRemote *remote);
gboolean
_ostree_repo_remove_remote (OstreeRepo   *self,
                            OstreeRemote *remote);
OstreeRemote *
_ostree_repo_get_remote (OstreeRepo  *self,
                         const char  *name,
                         GError     **error);
OstreeRemote *
_ostree_repo_get_remote_inherited (OstreeRepo  *self,
                                   const char  *name,
                                   GError     **error);

gboolean
_ostree_repo_maybe_regenerate_summary (OstreeRepo    *self,
                                       GCancellable  *cancellable,
                                       GError       **error);

/* Locking APIs are currently private.
 * See https://github.com/ostreedev/ostree/pull/1555
 */
typedef enum {
  OSTREE_REPO_LOCK_SHARED,
  OSTREE_REPO_LOCK_EXCLUSIVE
} OstreeRepoLockType;

gboolean      _ostree_repo_lock_push (OstreeRepo          *self,
                                     OstreeRepoLockType   lock_type,
                                     GCancellable        *cancellable,
                                     GError             **error);
gboolean      _ostree_repo_lock_pop (OstreeRepo    *self,
                                     GCancellable  *cancellable,
                                     GError       **error);

typedef OstreeRepo OstreeRepoAutoLock;

OstreeRepoAutoLock * _ostree_repo_auto_lock_push (OstreeRepo          *self,
                                                  OstreeRepoLockType   lock_type,
                                                  GCancellable        *cancellable,
                                                  GError             **error);
void          _ostree_repo_auto_lock_cleanup (OstreeRepoAutoLock *lock);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoAutoLock, _ostree_repo_auto_lock_cleanup)

G_END_DECLS
