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

#include <gio/gio.h>

#ifndef _OSTREE_PUBLIC
#define _OSTREE_PUBLIC extern
#endif

G_BEGIN_DECLS

typedef struct OstreeRepo OstreeRepo;
typedef struct OstreeRepoDevInoCache OstreeRepoDevInoCache;
typedef struct OstreeSePolicy OstreeSePolicy;
typedef struct OstreeSysroot OstreeSysroot;
typedef struct OstreeSysrootUpgrader OstreeSysrootUpgrader;
typedef struct OstreeMutableTree OstreeMutableTree;
typedef struct OstreeRepoFile OstreeRepoFile;
typedef struct _OstreeContentWriter OstreeContentWriter;
typedef struct OstreeRemote OstreeRemote;

G_END_DECLS
