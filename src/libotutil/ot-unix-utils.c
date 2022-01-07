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

#include "config.h"

#include "otutil.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gio/gunixoutputstream.h>

#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

/* Ensure that a pathname component @name does not contain the special Unix
 * entries `.` or `..`, and does not contain `/`.
 */
gboolean
ot_util_filename_validate (const char *name,
                           GError    **error)
{
  if (name == NULL)
    return glnx_throw (error, "Invalid NULL filename");
  if (strcmp (name, ".") == 0)
    return glnx_throw (error, "Invalid self-referential filename '.'");
  if (strcmp (name, "..") == 0)
    return glnx_throw (error, "Invalid path uplink filename '..'");
  if (strchr (name, '/') != NULL)
    return glnx_throw (error, "Invalid / in filename %s", name);
  if (!g_utf8_validate (name, -1, NULL))
    return glnx_throw (error, "Invalid UTF-8 in filename %s", name);
  return TRUE;
}

static GPtrArray *
ot_split_string_ptrarray (const char *str,
                          char        c)
{
  GPtrArray *ret = g_ptr_array_new_with_free_func (g_free);

  const char *p;
  do {
    p = strchr (str, '/');
    if (!p)
      {
        g_ptr_array_add (ret, g_strdup (str));
        str = NULL;
      }
    else
      {
        g_ptr_array_add (ret, g_strndup (str, p - str));
        str = p + 1;
      }
  } while (str && *str);

  return ret;
}

/* Given a pathname @path, split it into individual entries in @out_components,
 * validating that it does not have backreferences (`..`) etc.
 */
gboolean
ot_util_path_split_validate (const char *path,
                             GPtrArray **out_components,
                             GError    **error)
{
  if (strlen (path) > PATH_MAX)
    return glnx_throw (error, "Path '%s' is too long", path);

  g_autoptr(GPtrArray) ret_components = ot_split_string_ptrarray (path, '/');

  /* Canonicalize by removing '.' and '', throw an error on .. */
  for (int i = ret_components->len-1; i >= 0; i--)
    {
      const char *name = ret_components->pdata[i];
      if (strcmp (name, "..") == 0)
        return glnx_throw (error, "Invalid uplink '..' in path %s", path);
      if (strcmp (name, ".") == 0 || name[0] == '\0')
        g_ptr_array_remove_index (ret_components, i);
    }

  ot_transfer_out_value(out_components, &ret_components);
  return TRUE;
}
