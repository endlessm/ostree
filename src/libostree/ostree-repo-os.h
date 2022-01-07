/*
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

#pragma once

#include <sys/stat.h>
#include <gio/gio.h>
#include <ostree-types.h>

G_BEGIN_DECLS

/** 
 * OSTREE_METADATA_KEY_BOOTABLE:
 *
 * GVariant type `b`: Set if this commit is intended to be bootable
 * Since: 2021.1
 */
#define OSTREE_METADATA_KEY_BOOTABLE "ostree.bootable"
/** 
 * OSTREE_METADATA_KEY_LINUX:
 *
 * GVariant type `s`: Contains the Linux kernel release (i.e. `uname -r`)
 * Since: 2021.1
 */
#define OSTREE_METADATA_KEY_LINUX "ostree.linux"

_OSTREE_PUBLIC
gboolean
ostree_commit_metadata_for_bootable (GFile *root, GVariantDict *dict, GCancellable *cancellable, GError **error);

G_END_DECLS
