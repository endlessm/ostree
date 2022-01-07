/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
#include <ostree.h>

G_BEGIN_DECLS

gboolean ot_test_run_libtest (const char *cmd, GError **error);

OstreeRepo *ot_test_setup_repo (GCancellable *cancellable,
                                GError **error);

gboolean ot_check_relabeling (gboolean *can_relabel,
                              GError  **error);

gboolean ot_check_user_xattrs (gboolean *has_user_xattrs,
                               GError  **error);

OstreeSysroot *ot_test_setup_sysroot (GCancellable *cancellable,
                                      GError **error);

G_END_DECLS
