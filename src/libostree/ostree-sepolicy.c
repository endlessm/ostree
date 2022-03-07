/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#include <selinux/label.h>
#endif

#include "otutil.h"

#include "ostree-sepolicy.h"
#include "ostree-repo.h"
#include "ostree-sepolicy-private.h"
#include "ostree-bootloader-uboot.h"
#include "ostree-bootloader-syslinux.h"

/**
 * SECTION:ostree-sepolicy
 * @title: SELinux policy management
 * @short_description: Read SELinux policy and manage filesystem labels
 *
 * A #OstreeSePolicy object can load the SELinux policy from a given
 * root and perform labeling.
 */
struct OstreeSePolicy {
  GObject parent;

  int rootfs_dfd;
  int rootfs_dfd_owned;
  GFile *path;
  GLnxTmpDir tmpdir;

#ifdef HAVE_SELINUX
  GFile *selinux_policy_root;
  struct selabel_handle *selinux_hnd;
  char *selinux_policy_name;
  char *selinux_policy_csum;
#endif
};

typedef struct {
  GObjectClass parent_class;
} OstreeSePolicyClass;

static void initable_iface_init       (GInitableIface      *initable_iface);

enum {
  PROP_0,

  PROP_PATH,
  PROP_ROOTFS_DFD
};

G_DEFINE_TYPE_WITH_CODE (OstreeSePolicy, ostree_sepolicy, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init))

static void
ostree_sepolicy_finalize (GObject *object)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  (void) glnx_tmpdir_delete (&self->tmpdir, NULL, NULL);

  g_clear_object (&self->path);
  if (self->rootfs_dfd_owned != -1)
    (void) close (self->rootfs_dfd_owned);
#ifdef HAVE_SELINUX
  g_clear_object (&self->selinux_policy_root);
  g_clear_pointer (&self->selinux_policy_name, g_free);
  g_clear_pointer (&self->selinux_policy_csum, g_free);
  if (self->selinux_hnd)
    {
      selabel_close (self->selinux_hnd);
      self->selinux_hnd = NULL;
    }
#endif

  G_OBJECT_CLASS (ostree_sepolicy_parent_class)->finalize (object);
}

static void
ostree_sepolicy_set_property(GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  switch (prop_id)
    {
    case PROP_PATH:
      {
        GFile *path = g_value_get_object (value);
        if (path)
          {
            /* Canonicalize */
            self->path = g_file_new_for_path (gs_file_get_path_cached (path));
            g_assert_cmpint (self->rootfs_dfd, ==, -1);
          }
      }
      break;
    case PROP_ROOTFS_DFD:
      {
        int fd = g_value_get_int (value);
        if (fd != -1)
          {
            g_assert (self->path == NULL);
            self->rootfs_dfd = fd;
          }
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sepolicy_get_property(GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->path);
      break;
    case PROP_ROOTFS_DFD:
      g_value_set_int (value, self->rootfs_dfd);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sepolicy_constructed (GObject *object)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  g_assert (self->path != NULL || self->rootfs_dfd != -1);

  G_OBJECT_CLASS (ostree_sepolicy_parent_class)->constructed (object);
}

static void
ostree_sepolicy_class_init (OstreeSePolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ostree_sepolicy_constructed;
  object_class->get_property = ostree_sepolicy_get_property;
  object_class->set_property = ostree_sepolicy_set_property;
  object_class->finalize = ostree_sepolicy_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_ROOTFS_DFD,
                                   g_param_spec_int ("rootfs-dfd",
                                                     "", "",
                                                     -1, G_MAXINT, -1,
                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

}

#ifdef HAVE_SELINUX

/* Find the latest policy file in our root and return its checksum. */
static gboolean
get_policy_checksum (char        **out_csum,
                     GCancellable *cancellable,
                     GError      **error)
{
  const char *binary_policy_path = selinux_binary_policy_path ();
  const char *binfile_prefix = glnx_basename (binary_policy_path);
  g_autofree char *bindir_path = g_path_get_dirname (binary_policy_path);


  g_autofree char *best_policy = NULL;
  int best_version = 0;

  glnx_autofd int bindir_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, bindir_path, TRUE, &bindir_dfd, error))
    return FALSE;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0,};
  if (!glnx_dirfd_iterator_init_at (bindir_dfd, ".", FALSE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent,
                                                       cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type == DT_REG)
        {
          /* We could probably save a few hundred nanoseconds if we accept that
           * the prefix will always be "policy" and hardcode that in a static
           * compile-once GRegex... But picture how exciting it'd be if it *did*
           * somehow change; there would be cheers & slow-mo high-fives at the
           * sight of our code not breaking. Is that hope not worth a fraction
           * of a millisecond? I believe it is... or maybe I'm just lazy. */
          g_autofree char *regex = g_strdup_printf ("^\\Q%s\\E\\.[0-9]+$",
                                                    binfile_prefix);

          /* we could use match groups to extract the version, but mehhh, we
           * already have the prefix on hand */
          if (g_regex_match_simple (regex, dent->d_name, 0, 0))
            {
              int version = /* do +1 for the period */
                (int)g_ascii_strtoll (dent->d_name + strlen (binfile_prefix)+1,
                                      NULL, 10);
              g_assert (version > 0);

              if (version > best_version)
                {
                  best_version = version;
                  g_free (best_policy);
                  best_policy = g_strdup (dent->d_name);
                }
            }
        }
    }

  if (!best_policy)
    return glnx_throw (error, "Could not find binary policy file");

  *out_csum = ot_checksum_file_at (bindir_dfd, best_policy, G_CHECKSUM_SHA256,
                                   cancellable, error);
  if (*out_csum == NULL)
    return FALSE;

  return TRUE;
}

#endif

/**
 * ostree_sepolicy_new_from_commit:
 * @repo: The repo
 * @rev: ostree ref or checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Extract the SELinux policy from a commit object via a partial checkout.  This is useful
 * for labeling derived content as separate commits.
 *
 * This function is the backend of `ostree_repo_commit_modifier_set_sepolicy_from_commit()`.
 *
 * Returns: (transfer full): A new policy
 */
OstreeSePolicy*
ostree_sepolicy_new_from_commit (OstreeRepo  *repo,
                                 const char  *rev,
                                 GCancellable *cancellable,
                                 GError     **error)
{
  GLNX_AUTO_PREFIX_ERROR ("setting sepolicy from commit", error);
  g_autoptr(GFile) root = NULL;
  g_autofree char *commit = NULL;
  if (!ostree_repo_read_commit (repo, rev, &root, &commit, cancellable, error))
    return NULL;
  const char policypath[] = "usr/etc/selinux";
  g_autoptr(GFile) policyroot = g_file_get_child (root, policypath);

  GLnxTmpDir tmpdir = {0,};
  if (!glnx_mkdtemp ("ostree-commit-sepolicy-XXXXXX", 0700, &tmpdir, error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (tmpdir.fd, "usr/etc", 0755, cancellable, error))
    return FALSE;

  if (g_file_query_exists (policyroot, NULL))
    {
       OstreeRepoCheckoutAtOptions coopts = {0,};
       coopts.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
       coopts.subpath = glnx_strjoina ("/", policypath);
     
       if (!ostree_repo_checkout_at (repo, &coopts, tmpdir.fd, policypath, commit, cancellable, error))
         return glnx_prefix_error_null (error, "policy checkout");
    }

  OstreeSePolicy *ret = ostree_sepolicy_new_at (tmpdir.fd, cancellable, error);
  if (!ret)
    return NULL;
  /* Transfer ownership of tmpdir */
  ret->tmpdir = tmpdir;
  tmpdir.initialized = FALSE;
  return ret;
}

/* Workaround for http://marc.info/?l=selinux&m=149323809332417&w=2 */
#ifdef HAVE_SELINUX
static gboolean
cached_is_selinux_enabled (void)
{
  static gsize initialized;
  static gboolean cached_enabled;
  if (g_once_init_enter (&initialized))
    {
      cached_enabled = is_selinux_enabled () == 1;
      g_once_init_leave (&initialized, 1);
    }
  return cached_enabled;
}
#endif

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
#ifdef HAVE_SELINUX
  OstreeSePolicy *self = OSTREE_SEPOLICY (initable);
  gboolean enabled = FALSE;
  g_autofree char *policytype = NULL;
  const char *selinux_prefix = "SELINUX=";
  const char *selinuxtype_prefix = "SELINUXTYPE=";

  /* First thing here, call is_selinux_enabled() to prime the cache. See the
   * link above for more information why.
   */
  (void) cached_is_selinux_enabled ();

  /* TODO - use this below */
  g_autoptr(GFile) path = NULL;
  if (self->rootfs_dfd != -1)
    path = ot_fdrel_to_gfile (self->rootfs_dfd, ".");
  else if (self->path)
    {
      path = g_object_ref (self->path);
#if 0
      /* TODO - use this below */
      if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (self->path), TRUE,
                           &self->rootfs_dfd_owned, error))
        return FALSE;
      self->rootfs_dfd = self->rootfs_dfd_owned;
#endif
    }
  else
    g_assert_not_reached ();

  g_autoptr(GFile) etc_selinux_dir = g_file_resolve_relative_path (path, "etc/selinux");
  if (!g_file_query_exists (etc_selinux_dir, NULL))
    {
      g_object_unref (etc_selinux_dir);
      etc_selinux_dir = g_file_resolve_relative_path (path, "usr/etc/selinux");
    }

  g_autoptr(GFile) policy_config_path = g_file_get_child (etc_selinux_dir, "config");
  g_autoptr(GFile) policy_root = NULL;
  if (g_file_query_exists (policy_config_path, NULL))
    {
      g_autoptr(GFileInputStream) filein = g_file_read (policy_config_path, cancellable, error);

      if (!filein)
        return FALSE;

      g_autoptr(GDataInputStream) datain = g_data_input_stream_new ((GInputStream*)filein);

      while (TRUE)
        {
          gsize len;
          g_autoptr(GError) temp_error = NULL;
          g_autofree char *line = g_data_input_stream_read_line_utf8 (datain, &len,
                                                                   cancellable, &temp_error);

          if (temp_error)
            return g_propagate_error (error, g_steal_pointer (&temp_error)), FALSE;

          if (!line)
            break;

          if (g_str_has_prefix (line, selinuxtype_prefix))
            {
              policytype = g_strstrip (g_strdup (line + strlen (selinuxtype_prefix)));
              policy_root = g_file_get_child (etc_selinux_dir, policytype);
            }
          else if (g_str_has_prefix (line, selinux_prefix))
            {
              const char *enabled_str = line + strlen (selinux_prefix);
              if (g_ascii_strncasecmp (enabled_str, "enforcing", strlen ("enforcing")) == 0 ||
                  g_ascii_strncasecmp (enabled_str, "permissive", strlen ("permissive")) == 0)
                enabled = TRUE;
            }
        }
    }

  if (enabled)
    {
      const char *policy_rootpath = gs_file_get_path_cached (policy_root);

      if (selinux_set_policy_root (policy_rootpath) != 0)
        return glnx_throw_errno_prefix (error, "selinux_set_policy_root(%s)", policy_rootpath);

      self->selinux_hnd = selabel_open (SELABEL_CTX_FILE, NULL, 0);
      if (!self->selinux_hnd)
        return glnx_throw_errno_prefix (error, "With policy root '%s': selabel_open(SELABEL_CTX_FILE)",
                                        policy_rootpath);

      char *con = NULL;
      if (selabel_lookup_raw (self->selinux_hnd, &con, "/", 0755) != 0)
        return glnx_throw_errno_prefix (error, "With policy root '%s': Failed to look up context of /",
                                        policy_rootpath);
      freecon (con);

      if (!get_policy_checksum (&self->selinux_policy_csum, cancellable, error))
        return glnx_prefix_error (error, "While calculating SELinux checksum");

      self->selinux_policy_name = g_steal_pointer (&policytype);
      self->selinux_policy_root = g_object_ref (etc_selinux_dir);
    }

#endif
  return TRUE;
}

static void
ostree_sepolicy_init (OstreeSePolicy *self)
{
  self->rootfs_dfd = -1;
  self->rootfs_dfd_owned = -1;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/**
 * ostree_sepolicy_new:
 * @path: Path to a root directory
 * @cancellable: Cancellable
 * @error: Error
 *
 * Returns: (transfer full): An accessor object for SELinux policy in root located at @path
 */
OstreeSePolicy*
ostree_sepolicy_new (GFile         *path,
                     GCancellable  *cancellable,
                     GError       **error)
{
  return g_initable_new (OSTREE_TYPE_SEPOLICY, cancellable, error, "path", path, NULL);
}

/**
 * ostree_sepolicy_new_at:
 * @rootfs_dfd: Directory fd for rootfs (will not be cloned)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Returns: (transfer full): An accessor object for SELinux policy in root located at @rootfs_dfd
 *
 * Since: 2017.4
 */
OstreeSePolicy*
ostree_sepolicy_new_at (int         rootfs_dfd,
                        GCancellable  *cancellable,
                        GError       **error)
{
  return g_initable_new (OSTREE_TYPE_SEPOLICY, cancellable, error, "rootfs-dfd", rootfs_dfd, NULL);
}

/**
 * ostree_sepolicy_get_path:
 * @self: A SePolicy object
 *
 * This API should be considered deprecated, because it's supported for
 * policy objects to be created from file-descriptor relative paths, which
 * may not be globally accessible.
 *
 * Returns: (transfer none): Path to rootfs
 */
GFile *
ostree_sepolicy_get_path (OstreeSePolicy  *self)
{
  return self->path;
}

/**
 * ostree_sepolicy_get_name:
 * @self:
 *
 * Returns: (transfer none): Type of current policy
 */
const char *
ostree_sepolicy_get_name (OstreeSePolicy *self)
{
#ifdef HAVE_SELINUX
  return self->selinux_policy_name;
#else
  return NULL;
#endif
}

/**
 * ostree_sepolicy_get_csum:
 * @self:
 *
 * Returns: (transfer none): Checksum of current policy
 *
 * Since: 2016.5
 */
const char *
ostree_sepolicy_get_csum (OstreeSePolicy *self)
{
#ifdef HAVE_SELINUX
  return self->selinux_policy_csum;
#else
  return NULL;
#endif
}

/**
 * ostree_sepolicy_get_label:
 * @self: Self
 * @relpath: Path
 * @unix_mode: Unix mode
 * @out_label: (allow-none) (out) (transfer full): Return location for security context
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store in @out_label the security context for the given @relpath and
 * mode @unix_mode.  If the policy does not specify a label, %NULL
 * will be returned.
 */
gboolean
ostree_sepolicy_get_label (OstreeSePolicy    *self,
                           const char       *relpath,
                           guint32           unix_mode,
                           char            **out_label,
                           GCancellable     *cancellable,
                           GError          **error)
{
#ifdef HAVE_SELINUX
  /* Early return if no policy */
  if (!self->selinux_hnd)
    return TRUE;

  /* http://marc.info/?l=selinux&m=149082134430052&w=2
   * https://github.com/ostreedev/ostree/pull/768
   */
  if (strcmp (relpath, "/proc") == 0)
    relpath = "/mnt";

  char *con = NULL;
  int res = selabel_lookup_raw (self->selinux_hnd, &con, relpath, unix_mode);
  if (res != 0)
    {
      if (errno == ENOENT)
        *out_label = NULL;
      else
        return glnx_throw_errno_prefix (error, "selabel_lookup");
    }
  else
    {
      /* Ensure we consistently allocate with g_malloc */
      *out_label = g_strdup (con);
      freecon (con);
    }

#endif
  return TRUE;
}

/**
 * ostree_sepolicy_restorecon:
 * @self: Self
 * @path: Path string to use for policy lookup
 * @info: (allow-none): File attributes
 * @target: Physical path to target file
 * @flags: Flags controlling behavior
 * @out_new_label: (allow-none) (out): New label, or %NULL if unchanged
 * @cancellable: Cancellable
 * @error: Error
 *
 * Reset the security context of @target based on the SELinux policy.
 */
gboolean
ostree_sepolicy_restorecon (OstreeSePolicy    *self,
                            const char       *path,
                            GFileInfo        *info,
                            GFile            *target,
                            OstreeSePolicyRestoreconFlags flags,
                            char            **out_new_label,
                            GCancellable     *cancellable,
                            GError          **error)
{
#ifdef HAVE_SELINUX
  g_autoptr(GFileInfo) src_info = NULL;
  if (info != NULL)
    src_info = g_object_ref (info);
  else
    {
      src_info = g_file_query_info (target, "unix::mode",
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    cancellable, error);
      if (!src_info)
        return FALSE;
    }

  gboolean do_relabel = TRUE;
  if (flags & OSTREE_SEPOLICY_RESTORECON_FLAGS_KEEP_EXISTING)
    {
      char *existing_con = NULL;
      if (lgetfilecon_raw (gs_file_get_path_cached (target), &existing_con) > 0
          && existing_con)
        {
          do_relabel = FALSE;
          freecon (existing_con);
        }
    }

  g_autofree char *label = NULL;
  if (do_relabel)
    {
      if (!ostree_sepolicy_get_label (self, path,
                                      g_file_info_get_attribute_uint32 (src_info, "unix::mode"),
                                      &label,
                                      cancellable, error))
        return FALSE;

      if (!label)
        {
          if (!(flags & OSTREE_SEPOLICY_RESTORECON_FLAGS_ALLOW_NOLABEL))
            return glnx_throw (error, "No label found for '%s'", path);
        }
      else
        {
          if (lsetfilecon (gs_file_get_path_cached (target), label) < 0)
            return glnx_throw_errno_prefix (error, "lsetfilecon");
        }
    }

  if (out_new_label)
    *out_new_label = g_steal_pointer (&label);
#endif
  return TRUE;
}

/**
 * ostree_sepolicy_setfscreatecon:
 * @self: Policy
 * @path: Use this path to determine a label
 * @mode: Used along with @path
 * @error: Error
 *
 */
gboolean
ostree_sepolicy_setfscreatecon (OstreeSePolicy   *self,
                                const char       *path,
                                guint32           mode,
                                GError          **error)
{
#ifdef HAVE_SELINUX
  g_autofree char *label = NULL;

  /* setfscreatecon() will bomb out if the host has SELinux disabled,
   * but we're enabled for the target system.  This is kind of a
   * broken scenario...for now, we'll silently ignore the label
   * request.  To correctly handle the case of disabled host but
   * enabled target will require nontrivial work.
   */
  if (!cached_is_selinux_enabled ())
    return TRUE;

  if (!ostree_sepolicy_get_label (self, path, mode, &label, NULL, error))
    return FALSE;

  if (setfscreatecon_raw (label) != 0)
    return glnx_throw_errno_prefix (error, "setfscreatecon");

#endif
  return TRUE;
}

/**
 * ostree_sepolicy_fscreatecon_cleanup:
 * @unused: Not used, just in case you didn't infer that from the parameter name
 *
 * Cleanup function for ostree_sepolicy_setfscreatecon().
 */
void
ostree_sepolicy_fscreatecon_cleanup (void **unused)
{
#ifdef HAVE_SELINUX
  setfscreatecon (NULL);
#endif
}

/* Currently private copy of the older sepolicy/fscreatecon API with a nicer
 * g_auto() cleanup. May be made public later.
 */
gboolean
_ostree_sepolicy_preparefscreatecon (OstreeSepolicyFsCreatecon *con,
                                     OstreeSePolicy   *self,
                                     const char       *path,
                                     guint32           mode,
                                     GError          **error)
{
  if (!self || ostree_sepolicy_get_name (self) == NULL)
    return TRUE;

  if (!ostree_sepolicy_setfscreatecon (self, path, mode, error))
    return FALSE;

  con->initialized = TRUE;
  return TRUE;
}

void
_ostree_sepolicy_fscreatecon_clear (OstreeSepolicyFsCreatecon *con)
{
  if (!con->initialized)
    return;
  ostree_sepolicy_fscreatecon_cleanup (NULL);
}

/*
 * Given @xattrs, filter out `security.selinux`, and return
 * a new GVariant without it.  Supports @xattrs as %NULL to
 * mean "no xattrs", and also returns %NULL if no xattrs
 * would result (rather than a zero-length array).
 */
GVariant *
_ostree_filter_selinux_xattr (GVariant *xattrs)
{
  if (!xattrs)
    return NULL;

  gboolean have_xattrs = FALSE;
  GVariantBuilder builder;
  guint n = g_variant_n_children (xattrs);
  for (guint i = 0; i < n; i++)
    {
      const char *name = NULL;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (xattrs, i, "(^&ay@ay)",
                           &name, &value);

      if (strcmp (name, "security.selinux") == 0)
        continue;
      /* Initialize builder lazily */
      if (!have_xattrs)
        {
          have_xattrs = TRUE;
          g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));
        }
      g_variant_builder_add (&builder, "(@ay@ay)",
                             g_variant_new_bytestring (name),
                             value);
    }
  /* Canonicalize zero length to NULL for efficiency */
  if (!have_xattrs)
    return NULL;
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}
