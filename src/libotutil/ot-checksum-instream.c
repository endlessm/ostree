/* 
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
 * Copyright (C) 2022 Igalia S.L.
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ot-checksum-instream.h"
#include "ot-checksum-utils.h"

struct _OtChecksumInstreamPrivate {
  OtChecksum checksum;
};

G_DEFINE_TYPE_WITH_PRIVATE (OtChecksumInstream, ot_checksum_instream, G_TYPE_FILTER_INPUT_STREAM)

static gssize   ot_checksum_instream_read         (GInputStream         *stream,
                                                           void                 *buffer,
                                                           gsize                 count,
                                                           GCancellable         *cancellable,
                                                           GError              **error);

static void
ot_checksum_instream_finalize (GObject *object)
{
  OtChecksumInstream *self = (OtChecksumInstream*)object;

  ot_checksum_clear (&self->priv->checksum);

  G_OBJECT_CLASS (ot_checksum_instream_parent_class)->finalize (object);
}

static void
ot_checksum_instream_class_init (OtChecksumInstreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);

  object_class->finalize = ot_checksum_instream_finalize;
  stream_class->read_fn = ot_checksum_instream_read;
}

static void
ot_checksum_instream_init (OtChecksumInstream *self)
{
  self->priv = ot_checksum_instream_get_instance_private (self);
}

OtChecksumInstream *
ot_checksum_instream_new (GInputStream    *base,
                          GChecksumType    checksum_type)
{
  return ot_checksum_instream_new_with_start (base, checksum_type, NULL, 0);
}

/* Initialize a checksum stream with starting state from data */
OtChecksumInstream *
ot_checksum_instream_new_with_start (GInputStream   *base,
                                     GChecksumType   checksum_type,
                                     const guint8   *buf,
                                     size_t          len)
{
  OtChecksumInstream *stream;

  g_return_val_if_fail (G_IS_INPUT_STREAM (base), NULL);

  stream = g_object_new (OT_TYPE_CHECKSUM_INSTREAM,
                         "base-stream", base,
                         NULL);

  /* For now */
  g_assert (checksum_type == G_CHECKSUM_SHA256);
  ot_checksum_init (&stream->priv->checksum);
  if (buf)
    ot_checksum_update (&stream->priv->checksum, buf, len);

  return (OtChecksumInstream*) (stream);
}

static gssize
ot_checksum_instream_read (GInputStream  *stream,
                           void          *buffer,
                           gsize          count,
                           GCancellable  *cancellable,
                           GError       **error)
{
  OtChecksumInstream *self = (OtChecksumInstream*) stream;
  GFilterInputStream *fself = (GFilterInputStream*) self;
  gssize res = -1;

  res = g_input_stream_read (fself->base_stream,
                             buffer,
                             count,
                             cancellable,
                             error);
  if (res > 0)
    ot_checksum_update (&self->priv->checksum, buffer, res);

  return res;
}

char *
ot_checksum_instream_get_string (OtChecksumInstream *stream)
{
  char buf[_OSTREE_SHA256_STRING_LEN+1];
  ot_checksum_get_hexdigest (&stream->priv->checksum, buf, sizeof(buf));
  return g_strndup (buf, sizeof(buf));
}
