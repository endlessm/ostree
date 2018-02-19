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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include "libglnx.h"
#include "ostree.h"

typedef enum {
  OSTREE_BUILTIN_FLAG_NONE = 0,
  OSTREE_BUILTIN_FLAG_NO_REPO = 1 << 0,
  OSTREE_BUILTIN_FLAG_NO_CHECK = 1 << 1
} OstreeBuiltinFlags;

typedef enum {
  OSTREE_ADMIN_BUILTIN_FLAG_NONE = 0,
  OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER = (1 << 0),
  OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED = (1 << 1),
  OSTREE_ADMIN_BUILTIN_FLAG_NO_SYSROOT = (1 << 2),
} OstreeAdminBuiltinFlags;


typedef struct OstreeCommandInvocation OstreeCommandInvocation;

typedef struct {
  const char *name;
  OstreeBuiltinFlags flags;
  gboolean (*fn) (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error);
  const char *description;
} OstreeCommand;

/* This is a similar implementation as
 * https://github.com/projectatomic/rpm-ostree/commit/12c34bb2491a07079c911ef26401fee939e5573c.
 *
 * In the future if we want to add something new we won't need to
 * touch every prototype
 */
struct OstreeCommandInvocation {
  OstreeCommand *command;
};

int ostree_run (int argc, char **argv, OstreeCommand *commands, GError **error);

int ostree_usage (OstreeCommand *commands, gboolean is_error);

gboolean ostree_parse_sysroot_or_repo_option (GOptionContext *context,
                                              const char *sysroot_path,
                                              const char *repo_path,
                                              OstreeSysroot **out_sysroot,
                                              OstreeRepo **out_repo,
                                              GCancellable *cancellable,
                                              GError **error);

gboolean ostree_option_context_parse (GOptionContext *context,
                                      const GOptionEntry *main_entries,
                                      int *argc, char ***argv,
                                      OstreeCommandInvocation *invocation,
                                      OstreeRepo **out_repo,
                                      GCancellable *cancellable, GError **error);

gboolean ostree_admin_option_context_parse (GOptionContext *context,
                                            const GOptionEntry *main_entries,
                                            int *argc, char ***argv,
                                            OstreeAdminBuiltinFlags flags,
                                            OstreeCommandInvocation *invocation,
                                            OstreeSysroot **out_sysroot,
                                            GCancellable *cancellable, GError **error);

gboolean ostree_ensure_repo_writable (OstreeRepo *repo, GError **error);

void ostree_print_gpg_verify_result (OstreeGpgVerifyResult *result);

gboolean ot_enable_tombstone_commits (OstreeRepo *repo, GError **error);
