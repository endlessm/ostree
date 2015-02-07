/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ot-deployment.h"
#include "ostree.h"
#include "otutil.h"
#include "libgsystem.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static int opt_index = -1;
static char **opt_set;

static GOptionEntry options[] = {
  { "set", 's', 0, G_OPTION_ARG_STRING_ARRAY, &opt_set, "Set config option KEY=VALUE for remote", "KEY=VALUE" },
  { "index", 0, 0, G_OPTION_ARG_INT, &opt_index, "Operate on the deployment INDEX, starting from zero", "INDEX" },
  { NULL }
};

static gboolean
split_option_string (const char   *opt,
                     char       **out_key,
                     char       **out_value,
                     GError     **error)
{
  const char *equal = strchr (opt, '=');
  
  if (!equal)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Setting must be of the form \"key=value\"");
      return FALSE;
    }

  *out_key = g_strndup (opt, equal - opt);
  *out_value = g_strdup (equal + 1);

  return TRUE;
}

static gboolean
write_origin_file (GFile             *sysroot,
                   OtDeployment      *deployment,
                   GCancellable      *cancellable,
                   GError           **error)
{
  gboolean ret = FALSE;
  GKeyFile *origin = ot_deployment_get_origin (deployment);

  if (origin)
    {
      gs_unref_object GFile *deployment_path = ot_admin_get_deployment_directory (sysroot, deployment);
      gs_unref_object GFile *origin_path = ot_admin_get_deployment_origin_path (deployment_path);
      gs_free char *contents = NULL;
      gsize len;

      contents = g_key_file_to_data (origin, &len, error);
      if (!contents)
        goto out;

      if (!g_file_replace_contents (origin_path, contents, len, NULL, FALSE,
                                    G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                    cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_admin_builtin_set_origin (int argc, char **argv, GFile *sysroot, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  const char *remotename = NULL;
  const char *url = NULL;
  const char *branch = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *current_deployments = NULL;
  gs_unref_object OtDeployment *target_deployment = NULL;
  int current_bootversion;
  GKeyFile *config = NULL;

  context = g_option_context_new ("REMOTENAME URL [BRANCH]");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 3)
    {
      ot_util_usage_error (context, "REMOTENAME and URL must be specified", error);
      goto out;
    }

  remotename = argv[1];
  url = argv[2];
  if (argc > 3)
    branch = argv[3];

  if (!ot_admin_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (!ot_admin_list_deployments (sysroot, &current_bootversion, &current_deployments,
                                  cancellable, error))
    {
      g_prefix_error (error, "While listing deployments: ");
      goto out;
    }

  if (opt_index == -1)
    {
      if (!ot_admin_find_booted_deployment (sysroot, current_deployments,
                                            &target_deployment,
                                            cancellable, error))
        {
          g_prefix_error (error, "While getting booted deployment: ");
          goto out;
        }
      if (target_deployment == NULL)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Not currently booted into an OSTree system");
          goto out;
        }
    }
  else
    {
      if (opt_index < 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Invalid index %d", opt_index);
          goto out;
        }
      if (opt_index >= current_deployments->len)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Out of range index %d, expected < %d", opt_index, current_deployments->len);
          goto out;
        }
      target_deployment = g_object_ref (current_deployments->pdata[opt_index]);
    }

  { char **iter;
    gs_free char *remote_section = g_strdup_printf ("remote \"%s\"", remotename);

    config = ostree_repo_copy_config (repo);
    for (iter = opt_set; iter && *iter; iter++)
      {
        const char *keyvalue = *iter;
        gs_free char *subkey = NULL;
        gs_free char *subvalue = NULL;

        if (!split_option_string (keyvalue, &subkey, &subvalue, error))
          goto out;

        g_key_file_set_string (config, remote_section, subkey, subvalue);
      }

    g_key_file_set_string (config, remote_section, "url", url);
    if (!ostree_repo_write_config (repo, config, error))
        goto out;
  }
  
  { GKeyFile *old_origin = ot_deployment_get_origin (target_deployment);
    gs_free char *origin_refspec = g_key_file_get_string (old_origin, "origin", "refspec", NULL);
    gs_free char *origin_remote = NULL;
    gs_free char *origin_ref = NULL;
  
    if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, error))
      goto out;

    { gs_free char *new_refspec = g_strconcat (remotename, ":", branch ? branch : origin_ref, NULL);

      g_key_file_set_string (old_origin, "origin", "refspec", new_refspec);
      if (!write_origin_file (sysroot, target_deployment, cancellable, error))
        goto out;
    }
  }

  ret = TRUE;
 out:
  if (config)
    g_key_file_free (config);
  if (context)
    g_option_context_free (context);
  return ret;
}
