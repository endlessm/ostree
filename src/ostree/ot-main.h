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

#include "libglnx.h"
#include "ostree.h"

typedef enum {
  OSTREE_BUILTIN_FLAG_NONE = 0,
  OSTREE_BUILTIN_FLAG_NO_REPO = 1 << 0,
  OSTREE_BUILTIN_FLAG_NO_CHECK = 1 << 1,
  OSTREE_BUILTIN_FLAG_HIDDEN = 1 << 2,
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

int ostree_main (int argc, char **argv, OstreeCommand *commands);

int ostree_run (int argc, char **argv, OstreeCommand *commands, GError **error);

int ostree_usage (OstreeCommand *commands, gboolean is_error);

char* ostree_command_lookup_external (int argc, char **argv, OstreeCommand *commands);

int ostree_command_exec_external (char **argv);

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

/* Copied from rpm-ostree's rpmostree-libbuiltin.h */
#define TERM_ESCAPE_SEQUENCE(type,seq)          \
  static inline const char* ot_get_##type (void) { \
    if (glnx_stdout_is_tty ())                  \
      return seq;                               \
    return "";                                  \
  }

TERM_ESCAPE_SEQUENCE(red_start,  "\x1b[31m")
TERM_ESCAPE_SEQUENCE(red_end,    "\x1b[22m")
TERM_ESCAPE_SEQUENCE(bold_start, "\x1b[1m")
TERM_ESCAPE_SEQUENCE(bold_end,   "\x1b[0m")

#undef TERM_ESCAPE_SEQUENCE
