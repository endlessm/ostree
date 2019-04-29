/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean
ot_keyfile_get_boolean_with_default (GKeyFile      *keyfile,
                                     const char    *section,
                                     const char    *value,
                                     gboolean       default_value,
                                     gboolean      *out_bool,
                                     GError       **error);


gboolean
ot_keyfile_get_value_with_default (GKeyFile      *keyfile,
                                   const char    *section,
                                   const char    *value,
                                   const char    *default_value,
                                   char         **out_value,
                                   GError       **error);

gboolean
ot_keyfile_get_value_with_default_group_optional (GKeyFile      *keyfile,
                                                  const char    *section,
                                                  const char    *value,
                                                  const char    *default_value,
                                                  char         **out_value,
                                                  GError       **error);

gboolean
ot_keyfile_get_string_list_with_separator_choice (GKeyFile      *keyfile,
                                                  const char    *section,
                                                  const char    *key,
                                                  const char    *separators,
                                                  char        ***out_value_list,
                                                  GError       **error);

gboolean
ot_keyfile_get_string_list_with_default (GKeyFile      *keyfile,
                                         const char    *section,
                                         const char    *key,
                                         char           separator,
                                         char         **default_value,
                                         char        ***out_value,
                                         GError       **error);

gboolean
ot_keyfile_copy_group (GKeyFile   *source_keyfile,
                       GKeyFile   *target_keyfile,
                       const char *group_name);

G_END_DECLS
