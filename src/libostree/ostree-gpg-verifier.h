/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Sjoerd Simons <sjoerd.simons@collabora.co.uk>
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
 * Author: Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 */

//#pragma once

#include "config.h"
#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_GPG_VERIFIER ostree_gpg_verifier_get_type()
#define OSTREE_GPG_VERIFIER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_GPG_VERIFIER, OstreeGpgVerifier))
#define OSTREE_IS_GPG_VERIFIER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_GPG_VERIFIER))

/**
 * OSTREE_GPG_ERROR:
 *
 * Error domain for ostree gpg errors
 **/
#define OSTREE_GPG_ERROR ostree_gpg_error_quark()
GQuark ostree_gpg_error_quark (void);

/**
 * OstreeGPGErrorEnum:
 * @OSTREE_GPG_ERROR_NO_TRUSTED_SIG: No trusted signatures
 *
 * Error codes returned on GPG verification issues 
 **/

typedef enum {
  OSTREE_GPG_ERROR_NO_TRUSTED_SIG
} OstreeGpgErrorEnum;

typedef struct OstreeGpgVerifier OstreeGpgVerifier;

GType ostree_gpg_verifier_get_type (void);

OstreeGpgVerifier *ostree_gpg_verifier_new (void);

gboolean      ostree_gpg_verifier_check_signature (OstreeGpgVerifier *self,
                                                   GFile *file,
                                                   GFile *signature,
                                                   GCancellable     *cancellable,
                                                   GError          **error);

void ostree_gpg_verifier_set_homedir (OstreeGpgVerifier *self,
                                      const gchar *path);

gboolean      ostree_gpg_verifier_add_keyring_dir (OstreeGpgVerifier *self,
                                               const gchar *path,
                                               GError **error);

gboolean      ostree_gpg_verifier_add_keyring (OstreeGpgVerifier *self,
                                               const gchar *path,
                                               GError **error);
G_END_DECLS
