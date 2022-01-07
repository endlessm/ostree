/*
 * Copyright © 2011 Colin Walters <walters@verbum.org>
 * Copyright © 2015 Red Hat, Inc.
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
 *  - Colin Walters <walters@verbum.org>
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <stdint.h>
#include <string.h>

#include "ostree-remote.h"
#include "ostree-remote-private.h"
#include "ot-keyfile-utils.h"

/**
 * SECTION:remote
 *
 * The #OstreeRemote structure represents the configuration for a single remote
 * repository. Currently, all configuration is handled internally, and
 * #OstreeRemote objects are represented by their textual name handle, or by an
 * opaque pointer (which can be reference counted if needed).
 *
 * #OstreeRemote provides configuration for accessing a remote, but does not
 * provide the results of accessing a remote, such as information about what
 * refs are currently on a remote, or the commits they currently point to. Use
 * #OstreeRepo in combination with an #OstreeRemote to query that information.
 *
 * Since: 2018.6
 */

OstreeRemote *
ostree_remote_new (const gchar *name)
{
  return ostree_remote_new_dynamic (name, NULL);
}

OstreeRemote *
ostree_remote_new_dynamic (const gchar *name,
                           const gchar *refspec_name)
{
  OstreeRemote *remote;

  g_assert (name != NULL && *name != '\0');
  g_assert (refspec_name == NULL || *refspec_name != '\0');

  remote = g_slice_new0 (OstreeRemote);
  remote->ref_count = 1;
  remote->name = g_strdup (name);
  remote->refspec_name = g_strdup (refspec_name);
  remote->group = g_strdup_printf ("remote \"%s\"", (refspec_name != NULL) ? refspec_name : name);
  remote->keyring = g_strdup_printf ("%s.trustedkeys.gpg", (refspec_name != NULL) ? refspec_name : name);
  remote->options = g_key_file_new ();

  return remote;
}

OstreeRemote *
ostree_remote_new_from_keyfile (GKeyFile    *keyfile,
                                const gchar *group)
{
  g_autoptr(GMatchInfo) match = NULL;
  OstreeRemote *remote;
  g_autofree gchar *name = NULL;

  static gsize regex_initialized;
  static GRegex *regex;

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^remote \"(.+)\"$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  /* Sanity check */
  g_return_val_if_fail (g_key_file_has_group (keyfile, group), NULL);

  /* If group name doesn't fit the pattern, fail. */
  if (!g_regex_match (regex, group, 0, &match))
    return NULL;

  name = g_match_info_fetch (match, 1);
  remote = ostree_remote_new (name);

  ot_keyfile_copy_group (keyfile, remote->options, group);

  return remote;
}

/**
 * ostree_remote_ref:
 * @remote: an #OstreeRemote
 *
 * Increase the reference count on the given @remote.
 *
 * Returns: (transfer full): a copy of @remote, for convenience
 * Since: 2018.6
 */
OstreeRemote *
ostree_remote_ref (OstreeRemote *remote)
{
  gint refcount;
  g_return_val_if_fail (remote != NULL, NULL);
  refcount = g_atomic_int_add (&remote->ref_count, 1);
  g_assert (refcount > 0);
  return remote;
}

/**
 * ostree_remote_unref:
 * @remote: (transfer full): an #OstreeRemote
 *
 * Decrease the reference count on the given @remote and free it if the
 * reference count reaches 0.
 *
 * Since: 2018.6
 */
void
ostree_remote_unref (OstreeRemote *remote)
{
  g_return_if_fail (remote != NULL);
  g_return_if_fail (remote->ref_count > 0);

  if (g_atomic_int_dec_and_test (&remote->ref_count))
    {
      g_clear_pointer (&remote->name, g_free);
      g_clear_pointer (&remote->refspec_name, g_free);
      g_clear_pointer (&remote->group, g_free);
      g_clear_pointer (&remote->keyring, g_free);
      g_clear_object (&remote->file);
      g_clear_pointer (&remote->options, g_key_file_free);
      g_slice_free (OstreeRemote, remote);
    }
}

G_DEFINE_BOXED_TYPE(OstreeRemote, ostree_remote,
                    ostree_remote_ref,
                    ostree_remote_unref);

/**
 * ostree_remote_get_name:
 * @remote: an #OstreeRemote
 *
 * Get the human-readable name of the remote. This is what the user configured,
 * if the remote was explicitly configured; and will otherwise be a stable,
 * arbitrary, string.
 *
 * Returns: remote’s name
 * Since: 2018.6
 */
const gchar *
ostree_remote_get_name (OstreeRemote *remote)
{
  g_return_val_if_fail (remote != NULL, NULL);
  g_return_val_if_fail (remote->ref_count > 0, NULL);

  return remote->name;
}

/**
 * ostree_remote_get_url:
 * @remote: an #OstreeRemote
 *
 * Get the URL from the remote.
 *
 * Returns: (transfer full): the remote's URL
 * Since: 2018.6
 */
gchar *
ostree_remote_get_url (OstreeRemote *remote)
{
  g_return_val_if_fail (remote != NULL, NULL);
  g_return_val_if_fail (remote->ref_count > 0, NULL);

  return g_key_file_get_string (remote->options, remote->group, "url", NULL);
}
