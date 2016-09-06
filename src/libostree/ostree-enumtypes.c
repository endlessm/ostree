
/* Generated data (by glib-mkenums) */

/*
 * Copyright (C) 2015 Red Hat, Inc.
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

#include "ostree-enumtypes.h"
/* enumerations from "./src/libostree/ostree-fetcher.h" */
#include "./src/libostree/ostree-fetcher.h"

GType
_ostree_fetcher_config_flags_get_type (void)
{
  static volatile gsize the_type__volatile = 0;

  if (g_once_init_enter (&the_type__volatile))
    {
      static const GFlagsValue values[] = {
        { OSTREE_FETCHER_FLAGS_NONE,
          "OSTREE_FETCHER_FLAGS_NONE",
          "none" },
        { OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE,
          "OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE",
          "tls-permissive" },
        { 0, NULL, NULL }
      };

      GType the_type = g_flags_register_static (
        g_intern_static_string ("OstreeFetcherConfigFlags"),
        values);

      g_once_init_leave (&the_type__volatile, the_type);
    }

  return the_type__volatile;
}


/* Generated data ends here */

