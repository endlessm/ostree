/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Sjoerd Simons<sjoerd.simons@collabora.co.uk>
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

#include "ostree-gpg-verifier.h"
#include "libgsystem.h"

#define GPGVGOODPREFIX "[GNUPG:] GOODSIG "

typedef struct {
  GObjectClass parent_class;
} OstreeGpgVerifierClass;

struct OstreeGpgVerifier {
  GObject parent;

  GList *keyrings;
  gchar *homedir;
};

G_DEFINE_QUARK (ostree-gpg-error, ostree_gpg_error)

G_DEFINE_TYPE (OstreeGpgVerifier, ostree_gpg_verifier, G_TYPE_OBJECT)

static void
ostree_gpg_verifier_finalize (GObject *object)
{
  OstreeGpgVerifier *self = OSTREE_GPG_VERIFIER (object);

  g_list_free_full (self->keyrings, g_free);
  g_free (self->homedir);

  G_OBJECT_CLASS (ostree_gpg_verifier_parent_class)->finalize (object);
}

static void
ostree_gpg_verifier_class_init (OstreeGpgVerifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ostree_gpg_verifier_finalize;
}

static void
ostree_gpg_verifier_init (OstreeGpgVerifier *self)
{
}

typedef struct {
  OstreeGpgVerifier *self;
  GCancellable *cancellable;
  gboolean gpgv_done;
  gboolean status_done;

  gint goodsigs;
  gint exitcode;
  GError *error;
  GMainLoop *loop;
} VerifyRun;

static void
_gpgv_parse_line (VerifyRun *v, const gchar *line)
{
  if (g_str_has_prefix (line, GPGVGOODPREFIX))
    v->goodsigs++;
}

static void
_process_done (GObject *s, GAsyncResult *res, gpointer user_data)
{
  VerifyRun *v = user_data;
  gs_subprocess_wait_finish (GS_SUBPROCESS (s), res,
    &v->exitcode, &v->error);

  v->gpgv_done = TRUE;

  g_main_loop_quit (v->loop);
}

static void
_read_line (GObject *s, GAsyncResult *res, gpointer user_data)
{
  VerifyRun *v = user_data;
  gchar *line;

  /* Ignore errors when reading from the data input */
  line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (s),
    res, NULL, NULL);

  if (line == NULL)
    {
      v->status_done = TRUE;
      g_main_loop_quit (v->loop);
    }
  else
    {
      _gpgv_parse_line (v, line);
      g_free (line);
      g_data_input_stream_read_line_async (G_DATA_INPUT_STREAM (s),
        G_PRIORITY_DEFAULT, v->cancellable, _read_line, v);
    }
}


gboolean
ostree_gpg_verifier_check_signature (OstreeGpgVerifier *self,
                                     GFile *file,
                                     GFile *signature,
                                     GCancellable     *cancellable,
                                     GError          **error)
{
  gs_unref_object GSSubprocessContext *context = NULL;
  gs_unref_object GSSubprocess *proc = NULL;
  gs_unref_object GDataInputStream *data = NULL;
  gs_free gchar *status_fd_str = NULL;
  const gchar *file_path = NULL;
  const gchar *signature_path = NULL;
  GInputStream *output;
  gint fd;
  VerifyRun v = { 0, };
  GList *item;
  gboolean ret = FALSE;
  GMainContext *maincontext = g_main_context_new();
  GMainLoop *loop = g_main_loop_new (maincontext, FALSE);

  g_main_context_push_thread_default (maincontext);

  file_path = g_file_get_path (file);
  if (file_path == NULL || !g_file_query_exists (file, NULL))
    {
      g_set_error (error,
        G_FILE_ERROR, G_FILE_ERROR_NOENT,
        "File to verify doesn't exist");
      goto bail;
    }

  signature_path = g_file_get_path (signature);
  g_print ("=> %s\n", file_path);
  g_print ("=> %s\n", signature_path);
  if (signature_path == NULL || !g_file_query_exists (signature, NULL))
    {
      g_set_error (error,
        G_FILE_ERROR, G_FILE_ERROR_NOENT,
        "Signature file doesn't exist");
      goto bail;
    }


  context = gs_subprocess_context_newv ("gpgv", NULL);
  gs_subprocess_context_set_stdin_disposition (context,
    GS_SUBPROCESS_STREAM_DISPOSITION_NULL);
  gs_subprocess_context_set_stdout_disposition (context,
    GS_SUBPROCESS_STREAM_DISPOSITION_NULL);
  gs_subprocess_context_set_stderr_disposition (context,
    GS_SUBPROCESS_STREAM_DISPOSITION_NULL);

  if (!gs_subprocess_context_open_pipe_read (context, &output, &fd, error))
    goto bail;

  status_fd_str = g_strdup_printf ("%d", fd);
  gs_subprocess_context_argv_append (context, "--status-fd");
  gs_subprocess_context_argv_append (context, status_fd_str);

  for (item = self->keyrings ; item != NULL; item = g_list_next (item))
    {
      gs_subprocess_context_argv_append (context, "--keyring");
      gs_subprocess_context_argv_append (context, item->data);
    }

  gs_subprocess_context_argv_append (context, signature_path);
  gs_subprocess_context_argv_append (context, file_path);

  proc = gs_subprocess_new (context, cancellable, error);
  if (proc == NULL)
    goto bail;

  data = g_data_input_stream_new (output);

  v.self = self;
  v.cancellable = cancellable;
  v.loop = loop;

  gs_subprocess_wait (proc, cancellable, _process_done, &v);
  g_data_input_stream_read_line_async (data, G_PRIORITY_DEFAULT, cancellable,
    _read_line, &v);

  while (!v.gpgv_done || !v.status_done)
    g_main_loop_run (loop);

  if (v.goodsigs > 0)
    {
      ret = TRUE;
    }
  else
    {
      /* No good signatures found.. */
      g_set_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_TRUSTED_SIG,
        "File not signed with a trusted signature");
    }

bail:
  g_main_context_pop_thread_default (maincontext);
  g_main_loop_unref (loop);
  g_main_context_unref (maincontext);

  return ret;;
}

void
ostree_gpg_verifier_set_homedir (OstreeGpgVerifier *self, const gchar *path)
{
  g_free (self->homedir);
  self->homedir = g_strdup (path);
}

gboolean
ostree_gpg_verifier_add_keyring (OstreeGpgVerifier *self,
                                 const gchar *path,
                                 GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);

  self->keyrings = g_list_append (self->keyrings, g_strdup (path));
  return TRUE;
}

gboolean
ostree_gpg_verifier_add_keyring_dir (OstreeGpgVerifier *self,
                                     const gchar *path,
                                     GError **error)
{
  gs_unref_object GFileEnumerator *files = NULL;
  gs_unref_object GFileInfo *f = NULL;
  gs_unref_object GFile *dir = NULL;
  GError *e = NULL;

  dir = g_file_new_for_path (path);
  files = g_file_enumerate_children (dir, "*",
    G_FILE_QUERY_INFO_NONE, NULL, error);


  if (files == NULL)
    return FALSE;

  while ((f = g_file_enumerator_next_file (files, NULL, &e)) != NULL)
    {
      if (g_file_info_get_file_type (f) == G_FILE_TYPE_REGULAR &&
        g_str_has_suffix (g_file_info_get_name (f), ".gpg"))
        {
          self->keyrings = g_list_append (self->keyrings,
            g_build_filename (path, g_file_info_get_name (f), NULL));
        }
      g_object_unref (f);
    }

  if (e != NULL)
    {
      g_propagate_error (error, e);
      return FALSE;
    }

  return TRUE;
}

OstreeGpgVerifier*
ostree_gpg_verifier_new (void)
{
  return g_object_new (OSTREE_TYPE_GPG_VERIFIER, NULL);
}
