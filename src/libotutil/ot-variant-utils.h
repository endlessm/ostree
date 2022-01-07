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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

GVariant *ot_gvariant_new_bytearray (const guchar   *data,
                                     gsize           len);

GVariant *ot_gvariant_new_ay_bytes (GBytes *bytes);

GVariant *ot_gvariant_new_empty_string_dict (void);

gboolean ot_variant_read_fd (int                  fd,
                             goffset              offset,
                             const GVariantType  *type,
                             gboolean             trusted,
                             GVariant           **out_variant,
                             GError             **error);

GVariantBuilder *ot_util_variant_builder_from_variant (GVariant            *variant,
                                                       const GVariantType  *type);

gboolean
ot_variant_bsearch_str (GVariant   *array,
                        const char *str,
                        int        *out_pos);

G_END_DECLS
