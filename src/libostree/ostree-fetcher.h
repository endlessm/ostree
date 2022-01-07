/*
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#ifndef __GI_SCANNER__

#include "libglnx.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_FETCHER         (_ostree_fetcher_get_type ())
#define OSTREE_FETCHER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_FETCHER, OstreeFetcher))
#define OSTREE_FETCHER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_FETCHER, OstreeFetcherClass))
#define OSTREE_IS_FETCHER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_FETCHER))
#define OSTREE_IS_FETCHER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_FETCHER))
#define OSTREE_FETCHER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_FETCHER, OstreeFetcherClass))

/* Lower values have higher priority */
#define OSTREE_FETCHER_DEFAULT_PRIORITY 0

typedef struct OstreeFetcherURI OstreeFetcherURI;

typedef struct OstreeFetcherClass   OstreeFetcherClass;
typedef struct OstreeFetcher   OstreeFetcher;

struct OstreeFetcherClass
{
  GObjectClass parent_class;
};

G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeFetcher, g_object_unref)

typedef enum {
  OSTREE_FETCHER_FLAGS_NONE = 0,
  OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE = (1 << 0),
  OSTREE_FETCHER_FLAGS_TRANSFER_GZIP = (1 << 1),
  OSTREE_FETCHER_FLAGS_DISABLE_HTTP2 = (1 << 2),
} OstreeFetcherConfigFlags;

typedef enum {
  OSTREE_FETCHER_REQUEST_NUL_TERMINATION = (1 << 0),
  OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT = (1 << 1),
  OSTREE_FETCHER_REQUEST_LINKABLE = (1 << 2),
} OstreeFetcherRequestFlags;

void
_ostree_fetcher_uri_free (OstreeFetcherURI *uri);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeFetcherURI, _ostree_fetcher_uri_free)

OstreeFetcherURI *
_ostree_fetcher_uri_parse (const char       *str,
                           GError          **error);

OstreeFetcherURI *
_ostree_fetcher_uri_clone (OstreeFetcherURI *uri);

OstreeFetcherURI *
_ostree_fetcher_uri_new_path (OstreeFetcherURI *uri,
                              const char       *subpath);

OstreeFetcherURI *
_ostree_fetcher_uri_new_subpath (OstreeFetcherURI *uri,
                                 const char       *subpath);

char *
_ostree_fetcher_uri_get_scheme (OstreeFetcherURI *uri);

char *
_ostree_fetcher_uri_get_path (OstreeFetcherURI *uri);

char *
_ostree_fetcher_uri_to_string (OstreeFetcherURI *uri);

gboolean
_ostree_fetcher_uri_validate (OstreeFetcherURI *uri, GError **error);

GType   _ostree_fetcher_get_type (void) G_GNUC_CONST;

OstreeFetcher *_ostree_fetcher_new (int                      tmpdir_dfd,
                                    const char              *remote_name,
                                    OstreeFetcherConfigFlags flags);

int  _ostree_fetcher_get_dfd (OstreeFetcher *fetcher);

void _ostree_fetcher_set_cookie_jar (OstreeFetcher *self,
                                     const char    *jar_path);

void _ostree_fetcher_set_proxy (OstreeFetcher *fetcher,
                                const char    *proxy);

void _ostree_fetcher_set_client_cert (OstreeFetcher *fetcher,
                                      const char     *cert_path,
                                      const char     *key_path);

void _ostree_fetcher_set_tls_database (OstreeFetcher *self,
                                       const char    *tlsdb_path);

void _ostree_fetcher_set_extra_headers (OstreeFetcher *self,
                                        GVariant      *extra_headers);

void _ostree_fetcher_set_extra_user_agent (OstreeFetcher *self,
                                           const char    *extra_user_agent);

guint64 _ostree_fetcher_bytes_transferred (OstreeFetcher       *self);

void _ostree_fetcher_request_to_tmpfile (OstreeFetcher         *self,
                                         GPtrArray             *mirrorlist,
                                         const char            *filename,
                                         OstreeFetcherRequestFlags flags,
                                         const char            *if_none_match,
                                         guint64                if_modified_since,
                                         guint64                max_size,
                                         int                    priority,
                                         GCancellable          *cancellable,
                                         GAsyncReadyCallback    callback,
                                         gpointer               user_data);

gboolean _ostree_fetcher_request_to_tmpfile_finish (OstreeFetcher *self,
                                                    GAsyncResult  *result,
                                                    GLnxTmpfile   *out_tmpf,
                                                    gboolean      *out_not_modified,
                                                    char         **out_etag,
                                                    guint64       *out_last_modified,
                                                    GError       **error);

void _ostree_fetcher_request_to_membuf (OstreeFetcher         *self,
                                        GPtrArray             *mirrorlist,
                                        const char            *filename,
                                        OstreeFetcherRequestFlags flags,
                                        const char            *if_none_match,
                                        guint64                if_modified_since,
                                        guint64                max_size,
                                        int                    priority,
                                        GCancellable          *cancellable,
                                        GAsyncReadyCallback    callback,
                                        gpointer               user_data);

gboolean _ostree_fetcher_request_to_membuf_finish (OstreeFetcher *self,
                                                   GAsyncResult  *result,
                                                   GBytes       **out_buf,
                                                   gboolean      *out_not_modified,
                                                   char         **out_etag,
                                                   guint64       *out_last_modified,
                                                   GError       **error);


G_END_DECLS

#endif
