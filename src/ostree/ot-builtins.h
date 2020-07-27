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

#include "config.h"

#include "ostree.h"

G_BEGIN_DECLS

#define BUILTINPROTO(name) gboolean ostree_builtin_ ## name (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)

BUILTINPROTO(admin);
BUILTINPROTO(cat);
BUILTINPROTO(config);
BUILTINPROTO(checkout);
BUILTINPROTO(checksum);
BUILTINPROTO(commit);
BUILTINPROTO(diff);
BUILTINPROTO(export);
BUILTINPROTO(find_remotes);
BUILTINPROTO(create_usb);
#ifndef OSTREE_DISABLE_GPGME
BUILTINPROTO(gpg_sign);
#endif
BUILTINPROTO(init);
BUILTINPROTO(log);
BUILTINPROTO(pull);
BUILTINPROTO(pull_local);
BUILTINPROTO(ls);
BUILTINPROTO(prune);
BUILTINPROTO(refs);
BUILTINPROTO(reset);
BUILTINPROTO(fsck);
BUILTINPROTO(sign);
BUILTINPROTO(show);
BUILTINPROTO(static_delta);
BUILTINPROTO(summary);
BUILTINPROTO(rev_parse);
BUILTINPROTO(remote);
BUILTINPROTO(write_refs);
BUILTINPROTO(trivial_httpd);

#undef BUILTINPROTO

G_END_DECLS
