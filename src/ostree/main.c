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

#include "config.h"

#include <gio/gio.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>

#include "ot-main.h"
#include "ot-builtins.h"

static OstreeCommand commands[] = {
  /* Note: all admin related commands have
   * no_repo as their command flag, but each
   * admin command may have their own
   * admin flag
   */
  { "admin", OSTREE_BUILTIN_FLAG_NO_REPO,
    ostree_builtin_admin,
    "Commands for managing a host system booted with ostree" },
  { "cat", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_cat,
    "Concatenate contents of files"},
  { "checkout", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_checkout,
    "Check out a commit into a filesystem tree" },
  { "checksum", OSTREE_BUILTIN_FLAG_NO_REPO,
    ostree_builtin_checksum,
    "Checksum a file or directory" },
  { "commit", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_commit,
    "Commit a new revision" },
  { "config", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_config,
    "Change repo configuration settings" },
  { "diff", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_diff,
    "Compare directory TARGETDIR against revision REV"},
  { "export", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_export,
    "Stream COMMIT to stdout in tar format" },
  { "find-remotes", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_find_remotes,
    "Find remotes to serve the given refs" },
  { "create-usb", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_create_usb,
    "Copy the refs to a USB stick" },
  { "fsck", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_fsck,
    "Check the repository for consistency" },
#ifndef OSTREE_DISABLE_GPGME
  { "gpg-sign", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_gpg_sign,
    "Sign a commit" },
#endif /* OSTREE_DISABLE_GPGME */
  { "init", OSTREE_BUILTIN_FLAG_NO_CHECK,
    ostree_builtin_init,
    "Initialize a new empty repository" },
  { "log", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_log,
    "Show log starting at commit or ref" },
  { "ls", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_ls,
    "List file paths" },
  { "prune", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_prune,
    "Search for unreachable objects" },
  { "pull-local", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_pull_local,
    "Copy data from SRC_REPO" },
#ifdef HAVE_LIBCURL_OR_LIBSOUP
  { "pull", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_pull,
    "Download data from remote repository" },
#endif
  { "refs", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_refs,
    "List refs" },
  { "remote", OSTREE_BUILTIN_FLAG_NO_REPO,
    ostree_builtin_remote,
    "Remote commands that may involve internet access" },
  { "reset", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_reset,
    "Reset a REF to a previous COMMIT" },
  { "rev-parse", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_rev_parse,
    "Output the target of a rev" },
  { "sign", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_sign,
    "Sign a commit" },
  { "show", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_show,
    "Output a metadata object" },
  { "static-delta", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_static_delta,
    "Static delta related commands" },
  { "summary", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_summary,
    "Manage summary metadata" },
#if defined(HAVE_LIBSOUP) && defined(BUILDOPT_ENABLE_TRIVIAL_HTTPD_CMDLINE)
  { "trivial-httpd", OSTREE_BUILTIN_FLAG_NONE,
    ostree_builtin_trivial_httpd,
    NULL },
#endif
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  g_autoptr(GError) error = NULL;
  int ret;

  setlocale (LC_ALL, "");

  g_set_prgname (argv[0]);

  ret = ostree_run (argc, argv, commands, &error);

  if (error != NULL)
    {
      g_printerr ("%s%serror:%s%s %s\n",
                  ot_get_red_start (), ot_get_bold_start (),
                  ot_get_bold_end (), ot_get_red_end (),
                  error->message);
    }

  return ret;
}
