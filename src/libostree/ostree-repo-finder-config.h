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

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "ostree-repo-finder.h"
#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO_FINDER_CONFIG (ostree_repo_finder_config_get_type ())

/* Manually expanded version of the following, omitting autoptr support (for GLib < 2.44):
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeRepoFinderConfig, ostree_repo_finder_config, OSTREE, REPO_FINDER_CONFIG, GObject) */

_OSTREE_PUBLIC
GType ostree_repo_finder_config_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeRepoFinderConfig OstreeRepoFinderConfig;
typedef struct { GObjectClass parent_class; } OstreeRepoFinderConfigClass;

static inline OstreeRepoFinderConfig *OSTREE_REPO_FINDER_CONFIG (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_repo_finder_config_get_type (), OstreeRepoFinderConfig); }
static inline gboolean OSTREE_IS_REPO_FINDER_CONFIG (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_repo_finder_config_get_type ()); }
G_GNUC_END_IGNORE_DEPRECATIONS

_OSTREE_PUBLIC
OstreeRepoFinderConfig *ostree_repo_finder_config_new (void);

G_END_DECLS
