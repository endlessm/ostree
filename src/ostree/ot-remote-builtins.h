/*
 * Copyright (C) 2015 Red Hat, Inc.
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
 */

#pragma once

#include <ostree.h>

G_BEGIN_DECLS

#define BUILTINPROTO(name) gboolean ot_remote_builtin_ ## name (int argc, char **argv, \
                                                                OstreeCommandInvocation *invocation, \
                                                                GCancellable *cancellable, GError **error)

BUILTINPROTO(add);
BUILTINPROTO(delete);
BUILTINPROTO(gpg_import);
BUILTINPROTO(list_gpg_keys);
BUILTINPROTO(list);
#ifdef HAVE_LIBCURL_OR_LIBSOUP
BUILTINPROTO(add_cookie);
BUILTINPROTO(list_cookies);
BUILTINPROTO(delete_cookie);
#endif
BUILTINPROTO(show_url);
BUILTINPROTO(refs);
BUILTINPROTO(summary);

#undef BUILTINPROTO

G_END_DECLS
