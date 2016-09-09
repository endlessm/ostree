/* -*- c-file-style: "gnu" -*-
 * Switch to new root directory and start init.
 * 
 * Copyright 2011,2012,2013 Colin Walters <walters@verbum.org>
 *
 * Based on code from util-linux/sys-utils/switch_root.c, 
 * Copyright 2002-2009 Red Hat, Inc.  All rights reserved.
 * Authors:
 *	Peter Jones <pjones@redhat.com>
 *	Jeremy Katz <katzj@redhat.com>
 *
 * Relicensed with permission to LGPLv2+.
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

#include "config.h"

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <ctype.h>

#include "ostree-mount-util.h"

static char *
read_proc_cmdline (void)
{
  FILE *f = fopen("/proc/cmdline", "r");
  char *cmdline = NULL;
  size_t len;

  if (!f)
    goto out;

  /* Note that /proc/cmdline will not end in a newline, so getline
   * will fail unelss we provide a length.
   */
  if (getline (&cmdline, &len, f) < 0)
    goto out;
  /* ... but the length will be the size of the malloc buffer, not
   * strlen().  Fix that.
   */
  len = strlen (cmdline);

  if (cmdline[len-1] == '\n')
    cmdline[len-1] = '\0';
out:
  if (f)
    fclose (f);
  return cmdline;
}

static char *
parse_ostree_cmdline (void)
{
  char *cmdline = NULL;
  const char *iter;
  char *ret = NULL;

  cmdline = read_proc_cmdline ();
  if (!cmdline)
    err (EXIT_FAILURE, "failed to read /proc/cmdline");

  iter = cmdline;
  while (iter != NULL)
    {
      const char *next = strchr (iter, ' ');
      const char *next_nonspc = next;
      while (next_nonspc && *next_nonspc == ' ')
	next_nonspc += 1;
      if (strncmp (iter, "ostree=", strlen ("ostree=")) == 0)
        {
	  const char *start = iter + strlen ("ostree=");
	  if (next)
	    ret = strndup (start, next - start);
	  else
	    ret = strdup (start);
	  break;
        }
      iter = next_nonspc;
    }

  free (cmdline);
  return ret;
}

/* This is an API for other projects to determine whether or not the
 * currently running system is ostree-controlled.
 */
static void
touch_run_ostree (void)
{
  int fd;
  
  fd = open ("/run/ostree-booted", O_CREAT | O_WRONLY | O_NOCTTY, 0640);
  /* We ignore failures here in case /run isn't mounted...not much we
   * can do about that, but we don't want to fail.
   */
  if (fd == -1)
    return;
  (void) close (fd);
}

static char*
resolve_deploy_path (const char * root_mountpoint)
{
  char destpath[PATH_MAX];
  struct stat stbuf;
  char *ostree_target, *deploy_path;

  ostree_target = parse_ostree_cmdline ();
  if (!ostree_target)
    errx (EXIT_FAILURE, "No OSTree target; expected ostree=/ostree/boot.N/...");

  snprintf (destpath, sizeof(destpath), "%s/%s", root_mountpoint, ostree_target);
  printf ("Examining %s\n", destpath);
  if (lstat (destpath, &stbuf) < 0)
    err (EXIT_FAILURE, "Couldn't find specified OSTree root '%s'", destpath);
  if (!S_ISLNK (stbuf.st_mode))
    errx (EXIT_FAILURE, "OSTree target is not a symbolic link: %s", destpath);
  deploy_path = realpath (destpath, NULL);
  if (deploy_path == NULL)
    err (EXIT_FAILURE, "realpath(%s) failed", destpath);
  printf ("Resolved OSTree target to: %s\n", deploy_path);
  return deploy_path;
}

static int
pivot_root(const char * new_root, const char * put_old)
{
  return syscall(__NR_pivot_root, new_root, put_old);
}

int
main(int argc, char *argv[])
{
  const char *root_mountpoint = NULL, *root_arg = NULL;
  char *deploy_path = NULL;
  char srcpath[PATH_MAX];
  struct stat stbuf;
  int we_mounted_proc = 0;

  if (argc < 2)
    root_arg = "/";
  else
    root_arg = argv[1];

  if (stat ("/proc/cmdline", &stbuf) < 0)
    {
      if (errno != ENOENT)
        err (EXIT_FAILURE, "stat(\"/proc/cmdline\") failed");
      /* We need /proc mounted for /proc/cmdline and realpath (on musl) to
       * work: */
      if (mount ("proc", "/proc", "proc", 0, NULL) < 0)
        err (EXIT_FAILURE, "failed to mount proc on /proc");
      we_mounted_proc = 1;
    }

  root_mountpoint = realpath (root_arg, NULL);
  if (root_mountpoint == NULL)
    err (EXIT_FAILURE, "realpath(\"%s\")", root_arg);
  deploy_path = resolve_deploy_path (root_mountpoint);

  if (we_mounted_proc)
    {
      /* Leave the filesystem in the state that we found it: */
      if (umount ("/proc"))
        err (EXIT_FAILURE, "failed to umount proc from /proc");
    }

  /* Work-around for a kernel bug: for some reason the kernel
   * refuses switching root if any file systems are mounted
   * MS_SHARED. Hence remount them MS_PRIVATE here as a
   * work-around.
   *
   * https://bugzilla.redhat.com/show_bug.cgi?id=847418 */
  if (mount (NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) < 0)
    err (EXIT_FAILURE, "failed to make \"/\" private mount");

  /* Make deploy_path a bind mount, so we can move it later */
  if (mount (deploy_path, deploy_path, NULL, MS_BIND, NULL) < 0)
    err (EXIT_FAILURE, "failed to make initial bind mount %s", deploy_path);

  /* chdir to our new root.  We need to do this after bind-mounting it over
   * itself otherwise our cwd is still on the non-bind-mounted filesystem
   * below. */
  if (chdir (deploy_path) < 0)
    err (EXIT_FAILURE, "failed to chdir to deploy_path");

  /* Link to the deployment's /var */
  if (mount ("../../var", "var", NULL, MS_MGC_VAL|MS_BIND, NULL) < 0)
    err (EXIT_FAILURE, "failed to bind mount ../../var to var");

  /* If /boot is on the same partition, use a bind mount to make it visible
   * at /boot inside the deployment. */
  snprintf (srcpath, sizeof(srcpath), "%s/boot/loader", root_mountpoint);
  if (lstat (srcpath, &stbuf) == 0 && S_ISLNK (stbuf.st_mode))
    {
      if (lstat ("boot", &stbuf) == 0 && S_ISDIR (stbuf.st_mode))
        {
          snprintf (srcpath, sizeof(srcpath), "%s/boot", root_mountpoint);
          if (mount (srcpath, "boot", NULL, MS_BIND, NULL) < 0)
            err (EXIT_FAILURE, "failed to bind mount %s to boot", srcpath);
        }
    }

  /* Do we have a persistent overlayfs for /usr?  If so, mount it now. */
  if (lstat (".usr-ovl-work", &stbuf) == 0)
    {
      const char usr_ovl_options[] = "lowerdir=usr,upperdir=.usr-ovl-upper,workdir=.usr-ovl-work";

      /* Except overlayfs barfs if we try to mount it on a read-only
       * filesystem.  For this use case I think admins are going to be
       * okay if we remount the rootfs here, rather than waiting until
       * later boot and `systemd-remount-fs.service`.
       */
      if (path_is_on_readonly_fs ("."))
	{
	  if (mount (".", ".", NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
	    err (EXIT_FAILURE, "failed to remount rootfs writable (for overlayfs)");
	}
      
      if (mount ("overlay", "usr", "overlay", 0, usr_ovl_options) < 0)
        err (EXIT_FAILURE, "failed to mount /usr overlayfs");
    }
  else
    {
      /* Otherwise, a read-only bind mount for /usr */
      if (mount ("usr", "usr", NULL, MS_BIND, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
      if (mount ("usr", "usr", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
    }

  touch_run_ostree ();

  if (strcmp(root_mountpoint, "/") == 0)
    {
      /* pivot_root rotates two mount points around.  In this instance . (the
       * deploy location) becomes / and the existing / becomes /sysroot.  We
       * have to use pivot_root rather than mount --move in this instance
       * because our deploy location is mounted as a subdirectory of the real
       * sysroot, so moving sysroot would also move the deploy location.   In
       * reality attempting mount --move would fail with EBUSY. */
      if (pivot_root (".", "sysroot") < 0)
        err (EXIT_FAILURE, "failed to pivot_root to deployment");
    }
  else
    {
      /* In this instance typically we have our ready made-up up root at
       * /sysroot/ostree/deploy/.../ (deploy_path) and the real rootfs at
       * /sysroot (root_mountpoint).  We want to end up with our made-up root at
       * /sysroot/ and the real rootfs under /sysroot/sysroot as systemd will be
       * responsible for moving /sysroot to /.
       *
       * We need to do this in 3 moves to avoid trying to move /sysroot under
       * itself:
       *
       * 1. /sysroot/ostree/deploy/... -> /sysroot.tmp
       * 2. /sysroot -> /sysroot.tmp/sysroot
       * 3. /sysroot.tmp -> /sysroot
       */
      if (mkdir ("/sysroot.tmp", 0755) < 0)
        err (EXIT_FAILURE, "couldn't create temporary sysroot /sysroot.tmp");

      if (mount (deploy_path, "/sysroot.tmp", NULL, MS_MOVE, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE '%s' to '/sysroot.tmp'", deploy_path);

      if (mount (root_mountpoint, "sysroot", NULL, MS_MOVE, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE '%s' to 'sysroot'", root_mountpoint);

      if (mount (".", root_mountpoint, NULL, MS_MOVE, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE %s to %s", deploy_path, root_mountpoint);
    }

  if (getpid() == 1)
    {
      execl ("/sbin/init", "/sbin/init", NULL);
      err (EXIT_FAILURE, "failed to exec init inside ostree");
    }
  else
    {
      exit (EXIT_SUCCESS);
    }
}
