/*
 * main.c: Subversion server inspection tool.
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include <locale.h>
#include <apr_general.h>
#include <apr_pools.h>
#include <apr_time.h>
#include <apr_thread_proc.h>
#include <apr_file_io.h>

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_repos.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_time.h"
#include "svn_utf.h"


/*** Some convenience macros and types. ***/

/* Temporary subdirectory created for use by svnlook.  ### todo: we
   need to either find a way to query APR for a suitable (that is,
   writable) temporary directory, or add this #define to the
   svn_private_config.h stuffs (with a default of perhaps "/tmp/.svnlook" */
#define SVNLOOK_TMPDIR       ".svnlook"

typedef enum svnlook_cmd_t
{
  svnlook_cmd_default = 0,

  svnlook_cmd_author,
  svnlook_cmd_changed,
  svnlook_cmd_date,
  svnlook_cmd_diff,
  svnlook_cmd_dirs_changed,
  svnlook_cmd_ids,
  svnlook_cmd_info,
  svnlook_cmd_log,
  svnlook_cmd_tree
  
} svnlook_cmd_t;


typedef struct svnlook_ctxt_t
{
  svn_repos_t *repos;
  svn_fs_t *fs;
  svn_boolean_t is_revision;
  svn_revnum_t rev_id;
  svn_fs_txn_t *txn;
  const char *txn_name /* UTF-8! */;

} svnlook_ctxt_t;



/*** Helper functions. ***/
static svn_error_t *
get_property (svn_string_t **prop_value,
              svnlook_ctxt_t *c, 
              const char *prop_name /* UTF-8! */,
              apr_pool_t *pool)
{
  /* Fetch transaction property... */
  if (! c->is_revision)
    return svn_fs_txn_prop (prop_value, c->txn, prop_name, pool);

  /* ...or revision property -- it's your call. */
  return svn_fs_revision_prop (prop_value, c->fs, c->rev_id, prop_name, pool);
}


static svn_error_t *
get_root (svn_fs_root_t **root,
          svnlook_ctxt_t *c,
          apr_pool_t *pool)
{
  /* Open up the appropriate root (revision or transaction). */
  if (c->is_revision)
    {
      /* If we didn't get a valid revision number, we'll look at the
         youngest revision. */
      if (! SVN_IS_VALID_REVNUM (c->rev_id))
        SVN_ERR (svn_fs_youngest_rev (&(c->rev_id), c->fs, pool));

      SVN_ERR (svn_fs_revision_root (root, c->fs, c->rev_id, pool));
    }
  else
    {
      SVN_ERR (svn_fs_txn_root (root, c->txn, pool));
    }

  return SVN_NO_ERROR;
}



/*** Tree Routines ***/

/* Generate a generic delta tree. */
static svn_error_t *
generate_delta_tree (svn_repos_node_t **tree,
                     svn_repos_t *repos,
                     svn_fs_root_t *root, 
                     svn_revnum_t base_rev, 
                     svn_boolean_t use_copy_history,
                     apr_pool_t *pool)
{
  svn_fs_root_t *base_root;
  const svn_delta_editor_t *editor;
  void *edit_baton;
  apr_pool_t *edit_pool = svn_pool_create (pool);
  svn_fs_t *fs = svn_repos_fs (repos);

  /* Get the base root. */
  SVN_ERR (svn_fs_revision_root (&base_root, fs, base_rev, pool));

  /* Request our editor. */
  SVN_ERR (svn_repos_node_editor (&editor, &edit_baton, repos,
                                  base_root, root, pool, edit_pool));

  /* Drive our editor. */
  SVN_ERR (svn_repos_dir_delta (base_root, "", NULL, root, "",
                                editor, edit_baton, 
                                FALSE, TRUE, FALSE, 
                                use_copy_history, edit_pool));

  /* Return the tree we just built. */
  *tree = svn_repos_node_from_baton (edit_baton);
  svn_pool_destroy (edit_pool);
  return SVN_NO_ERROR;
}



/*** Tree Printing Routines ***/

/* Recursively print only directory nodes that either a) have property
   mods, or b) contains files that have changed. */
static svn_error_t *
print_dirs_changed_tree (svn_repos_node_t *node,
                         const char *path /* UTF-8! */,
                         apr_pool_t *pool)
{
  svn_repos_node_t *tmp_node;
  int print_me = 0;
  const char *full_path;

  if (! node)
    return SVN_NO_ERROR;

  /* Not a directory?  We're not interested. */
  if (node->kind != svn_node_dir)
    return SVN_NO_ERROR;

  /* Got prop mods?  Excellent. */
  if (node->prop_mod)
    print_me = 1;

  if (! print_me)
    {
      /* Fly through the list of children, checking for modified files. */
      tmp_node = node->child;
      if (tmp_node)
        {
          if ((tmp_node->kind == svn_node_file)
              || (tmp_node->text_mod)
              || (tmp_node->action == 'A')
              || (tmp_node->action == 'D'))
            {
              print_me = 1;
            }
          while (tmp_node->sibling && (! print_me ))
            {
              tmp_node = tmp_node->sibling;
              if ((tmp_node->kind == svn_node_file)
                  || (tmp_node->text_mod)
                  || (tmp_node->action == 'A')
                  || (tmp_node->action == 'D'))
                {
                  print_me = 1;
                }
            }
        }
    }
  
  /* Print the node if it qualifies. */
  if (print_me)
    {
      const char *path_native;
      SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, path, pool));
      printf ("%s/\n", path_native);
    }

  /* Recursively handle the node's children. */
  tmp_node = node->child;
  if (! tmp_node)
    return SVN_NO_ERROR;

  full_path = svn_path_join (path, tmp_node->name, pool);
  SVN_ERR (print_dirs_changed_tree (tmp_node, full_path, pool));
  while (tmp_node->sibling)
    {
      tmp_node = tmp_node->sibling;
      full_path = svn_path_join (path, tmp_node->name, pool);
      SVN_ERR (print_dirs_changed_tree (tmp_node, full_path, pool));
    }

  return SVN_NO_ERROR;
}


/* Recursively print all nodes in the tree that have been modified
   (do not include directories affected only by "bubble-up"). */
static svn_error_t *
print_changed_tree (svn_repos_node_t *node,
                    const char *path /* UTF-8! */,
                    apr_pool_t *pool)
{
  const char *full_path;
  char status[3] = "_ ";
  int print_me = 1;

  if (! node)
    return SVN_NO_ERROR;

  /* Print the node. */
  if (node->action == 'A')
    status[0] = 'A';
  else if (node->action == 'D')
    status[0] = 'D';
  else if (node->action == 'R')
    {
      if ((! node->text_mod) && (! node->prop_mod))
        print_me = 0;
      if (node->text_mod)
        status[0] = 'U';
      if (node->prop_mod)
        status[1] = 'U';
    }
  else
    print_me = 0;

  /* Print this node unless told to skip it. */
  if (print_me)
    {
      const char *path_native;
      SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, path, pool));
      printf ("%s  %s%s\n",
              status,
              path_native,
              node->kind == svn_node_dir ? "/" : "");
    }
  
  /* Return here if the node has no children. */
  node = node->child;
  if (! node)
    return SVN_NO_ERROR;

  /* Recursively handle the node's children. */
  full_path = svn_path_join (path, node->name, pool);
  SVN_ERR (print_changed_tree (node, full_path, pool));
  while (node->sibling)
    {
      node = node->sibling;
      full_path = svn_path_join (path, node->name, pool);
      SVN_ERR (print_changed_tree (node, full_path, pool));
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
open_writable_binary_file (apr_file_t **fh, 
                           const char *path /* UTF-8! */, 
                           apr_pool_t *pool)
{
  apr_array_header_t *path_pieces;
  svn_error_t *err;
  int i;
  const char *full_path, *dir;
  
  /* Try the easy way to open the file. */
  err = svn_io_file_open (fh, path, 
                          APR_WRITE | APR_CREATE | APR_TRUNCATE | APR_BINARY,
                          APR_OS_DEFAULT, pool);
  if (! err)
    return SVN_NO_ERROR;

  svn_path_split_nts (path, &dir, NULL, pool);

  /* If the file path has no parent, then we've already tried to open
     it as best as we care to try above. */
  if (svn_path_is_empty_nts (dir))
    return svn_error_createf (err->apr_err, err->src_err, err, pool,
                              "Error opening writable file %s", path);

  path_pieces = svn_path_decompose (dir, pool);
  if (! path_pieces->nelts)
    return SVN_NO_ERROR;

  full_path = "";
  for (i = 0; i < path_pieces->nelts; i++)
    {
      enum svn_node_kind kind;
      const char *piece = ((const char **) (path_pieces->elts))[i];
      full_path = svn_path_join (full_path, piece, pool);
      SVN_ERR (svn_io_check_path (full_path, &kind, pool));

      /* Does this path component exist at all? */
      if (kind == svn_node_none)
        {
          SVN_ERR (svn_io_dir_make (full_path, APR_OS_DEFAULT, pool));
        }
      else if (kind != svn_node_dir)
        {
          if (err)
            return svn_error_createf (err->apr_err, err->src_err, err, pool,
                                      "Error creating dir %s (path exists)", 
                                      full_path);
        }
    }

  /* Now that we are ensured that the parent path for this file
     exists, try once more to open it. */
  err = svn_io_file_open (fh, path, 
                          APR_WRITE | APR_CREATE | APR_TRUNCATE | APR_BINARY,
                          APR_OS_DEFAULT, pool);
  if (err)
    return svn_error_createf (err->apr_err, err->src_err, err, pool,
                              "Error opening writable file %s", path);
    
  return SVN_NO_ERROR;
}


static svn_error_t *
dump_contents (apr_file_t *fh,
               svn_fs_root_t *root,
               const char *path /* UTF-8! */,
               apr_pool_t *pool)
{
  apr_status_t apr_err;
  apr_size_t len, len2;
  svn_stream_t *stream;
  char buffer[1024];

  /* Get a stream to the current file's contents. */
  SVN_ERR (svn_fs_file_contents (&stream, root, path, pool));
  
  /* Now, route that data into our temporary file. */
  while (1)
    {
      len = sizeof (buffer);
      SVN_ERR (svn_stream_read (stream, buffer, &len));
      len2 = len;
      apr_err = apr_file_write (fh, buffer, &len2);
      if ((apr_err) || (len2 != len))
        return svn_error_createf 
          (apr_err ? apr_err : SVN_ERR_INCOMPLETE_DATA, 0, NULL, pool,
               "Error writing contents of %s", path);
      if (len != sizeof (buffer))
        break;
    }

  /* And close the file. */
  apr_file_close (fh);
  return SVN_NO_ERROR;
}


/* Recursively print all nodes in the tree that have been modified
   (do not include directories affected only by "bubble-up"). */
static svn_error_t *
print_diff_tree (svn_fs_root_t *root,
                 svn_fs_root_t *base_root,
                 svn_repos_node_t *node, 
                 const char *path /* UTF-8! */,
                 const char *base_path /* UTF-8! */,
                 apr_pool_t *pool)
{
  const char *orig_path = NULL, *new_path = NULL;
  apr_file_t *fh1, *fh2;
  svn_boolean_t is_copy = FALSE;

  if (! node)
    return SVN_NO_ERROR;

  /* Print copyfrom history for the top node of a copied tree. */
  if ((SVN_IS_VALID_REVNUM (node->copyfrom_rev))
      && (node->copyfrom_path != NULL))
    {
      const char *base_path_native;

      /* This is ... a copy. */
      is_copy = TRUE;

      /* Propagate the new base.  Copyfrom paths usually start with a
         slash; we remove it for consistency with the target path.
         ### Yes, it would be *much* better for something in the path
             library to be taking care of this! */
      if (node->copyfrom_path[0] == '/')
        base_path = apr_pstrdup (pool, node->copyfrom_path + 1);
      else
        base_path = apr_pstrdup (pool, node->copyfrom_path);

      SVN_ERR (svn_utf_cstring_from_utf8 (&base_path_native, base_path, pool));

      printf ("Copied: %s (from rev %" SVN_REVNUM_T_FMT ", %s)\n",
              node->name, node->copyfrom_rev, base_path_native);

      SVN_ERR (svn_fs_revision_root (&base_root,
                                     svn_fs_root_fs (base_root),
                                     node->copyfrom_rev, pool));
    }

  /* First, we'll just print file content diffs. */
  if (node->kind == svn_node_file)
    {
      /* Here's the generalized way we do our diffs:

         - First, dump the contents of the new version of the file
           into the svnlook temporary directory, building out the
           actual directories that need to be created in order to
           fully represent the filesystem path inside the tmp
           directory.

         - Then, dump the contents of the old version of the file into
           the top level of the svnlook temporary directory using a
           unique temporary file name (we do this *after* putting the
           new version of the file there in case something actually
           versioned has a name that looks like one of our unique
           identifiers).
           
         - Next, we run 'diff', passing the repository path as the
           label.  

         - Finally, we delete the temporary files (but leave the
           built-out directories in place until after all diff
           handling has been finished).  */
      if ((node->action == 'R') && (node->text_mod))
        {
          new_path = svn_path_join (SVNLOOK_TMPDIR, path, pool);
          SVN_ERR (open_writable_binary_file (&fh1, new_path, pool));
          SVN_ERR (dump_contents (fh1, root, path, pool));
          apr_file_close (fh1);

          SVN_ERR (svn_io_open_unique_file (&fh2, &orig_path, new_path,
                                            NULL, FALSE, pool));
          SVN_ERR (dump_contents
                   (fh2, base_root, base_path, pool));
          apr_file_close (fh2);
        }
      if ((node->action == 'A') && (node->text_mod))
        {
          new_path = svn_path_join (SVNLOOK_TMPDIR, path, pool);
          SVN_ERR (open_writable_binary_file (&fh1, new_path, pool));
          SVN_ERR (dump_contents (fh1, root, path, pool));
          apr_file_close (fh1);

          /* Create an empty file. */
          SVN_ERR (svn_io_open_unique_file (&fh2, &orig_path, new_path,
                                            NULL, FALSE, pool));
          apr_file_close (fh2);
        }
      if (node->action == 'D')
        {
          new_path = svn_path_join (SVNLOOK_TMPDIR, path, pool);
          SVN_ERR (open_writable_binary_file (&fh1, new_path, pool));
          apr_file_close (fh1);

          SVN_ERR (svn_io_open_unique_file (&fh2, &orig_path, new_path,
                                            NULL, FALSE, pool));
          SVN_ERR (dump_contents
                   (fh2, base_root, base_path, pool));
          apr_file_close (fh2);
        }
    }

  if (orig_path && new_path)
    {
      apr_file_t *outhandle;
      apr_status_t apr_err;
      const char *label;
      const char *abs_path;
      int exitcode;
      const char *path_native;
      
      if (! is_copy)
        {
          SVN_ERR (svn_utf_cstring_from_utf8 (&path_native, path, pool));
          printf ("%s: %s\n", 
                  ((node->action == 'A') ? "Added" : 
                   ((node->action == 'D') ? "Deleted" :
                    ((node->action == 'R') ? "Modified" : "Index"))),
                  path_native);
        }
      printf ("===============================================================\
===============\n");
      fflush (stdout);

      /* Get an apr_file_t representing stdout, which is where we'll have
         the diff program print to. */
      apr_err = apr_file_open_stdout (&outhandle, pool);
      if (apr_err)
        return svn_error_create 
          (apr_err, 0, NULL, pool,
           "print_diff_tree: can't open handle to stdout");

      label = apr_psprintf (pool, "%s\t(original)", base_path);
      SVN_ERR (svn_path_get_absolute (&abs_path, orig_path, pool));
      SVN_ERR (svn_io_run_diff (SVNLOOK_TMPDIR, NULL, 0, label, NULL,
                                abs_path, path, 
                                &exitcode, outhandle, NULL, pool));

      /* TODO: Handle exit code == 2 (i.e. diff error) here. */

      printf ("\n");
      fflush (stdout);
    }
  else if (is_copy)
    {
      printf ("\n");
    }
    
  /* Now, delete any temporary files. */
  if (orig_path)
    svn_io_remove_file (orig_path, pool);
  if (new_path)
    svn_io_remove_file (new_path, pool);

  /* Return here if the node has no children. */
  node = node->child;
  if (! node)
    return SVN_NO_ERROR;

  /* Handle children and siblings. */
  {
    apr_pool_t *subpool = svn_pool_create (pool);

    /* Recurse down into children. */
    SVN_ERR (print_diff_tree
             (root, base_root, node,
              svn_path_join (path, node->name, subpool),
              svn_path_join (base_path, node->name, subpool),
              subpool));

    /* Recurse across siblings. */
    while (node->sibling)
      {
        node = node->sibling;
       
        SVN_ERR (print_diff_tree
                 (root, base_root, node,
                  svn_path_join (path, node->name, subpool),
                  svn_path_join (base_path, node->name, subpool),
                  pool));
      }
    
    apr_pool_destroy (subpool);
  }

  return SVN_NO_ERROR;
}


/* Recursively print all nodes in the tree. 
 * 
 * ### todo: I'd like to write a more descriptive doc string for this
 * function, but I wasn't able to figure out in a finite amount of
 * time why it even takes the arguments it takes, or why it does what
 * it does with them.  See issue #540.  -kff
 */
static svn_error_t *
print_ids_tree (svn_repos_node_t *node,
                svn_fs_root_t *root,
                const char *path /* UTF-8! */,
                int indentation,
                apr_pool_t *pool)
{
  const char *full_path;
  int i;
  const svn_fs_id_t *id;
  svn_string_t *unparsed_id = NULL;
  apr_pool_t *subpool;
  const char *name_native;

  if (! node)
    return SVN_NO_ERROR;

  /* Print the indentation. */
  for (i = 0; i < indentation; i++)
    {
      printf (" ");
    }

  /* Get the node's ID */
  svn_fs_node_id (&id, root, path, pool);
  if (id)
    unparsed_id = svn_fs_unparse_id (id, pool);
  
  /* Print the node. */
  SVN_ERR (svn_utf_cstring_from_utf8 (&name_native, node->name, pool));
  printf ("%s%s <%s>\n", 
          name_native, 
          node->kind == svn_node_dir ? "/" : "",
          unparsed_id ? unparsed_id->data : "unknown");

  /* Return here if the node has no children. */
  node = node->child;
  if (! node)
    return SVN_NO_ERROR;

  /* Recursively handle the node's children. */
  subpool = svn_pool_create (pool);
  full_path = svn_path_join (path, node->name, subpool);
  SVN_ERR (print_ids_tree (node, root, full_path, indentation + 1, subpool));
  while (node->sibling)
    {
      svn_pool_clear (subpool);
      node = node->sibling;
      full_path = svn_path_join (path, node->name, pool);
      SVN_ERR (print_ids_tree (node, root, full_path, indentation + 1,
                               subpool));
    }
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


/* Recursively print all nodes in the tree.  If SHOW_IDS is non-zero,
   print the id of each node next to its name. */
static svn_error_t *
print_tree (svn_repos_node_t *node,
            int indentation,
            apr_pool_t *pool)
{
  int i;
  const char *name_native;

  if (! node)
    return SVN_NO_ERROR;

  /* Print the indentation. */
  for (i = 0; i < indentation; i++)
    {
      printf (" ");
    }

  /* Print the node. */
  SVN_ERR (svn_utf_cstring_from_utf8 (&name_native, node->name, pool));
  printf ("%s%s\n", 
          name_native, 
          node->kind == svn_node_dir ? "/" : "");

  /* Return here if the node has no children. */
  node = node->child;
  if (! node)
    return SVN_NO_ERROR;

  /* Recursively handle the node's children. */
  SVN_ERR (print_tree (node, indentation + 1, pool));
  while (node->sibling)
    {
      node = node->sibling;
      SVN_ERR (print_tree (node, indentation + 1, pool));
    }

  return SVN_NO_ERROR;
}




/*** Subcommand handlers. ***/

/* Print the revision's log message to stdout, followed by a newline. */
static svn_error_t *
do_log (svnlook_ctxt_t *c, svn_boolean_t print_size, apr_pool_t *pool)
{
  svn_string_t *prop_value;

  SVN_ERR (get_property (&prop_value, c, SVN_PROP_REVISION_LOG, pool));

  if (prop_value && prop_value->data)
    {
      const char *log_native;

      if (print_size)
        {
          printf ("%" APR_SIZE_T_FMT "\n", prop_value->len);
        }

      SVN_ERR (svn_utf_cstring_from_utf8 (&log_native, prop_value->data,
                                          pool));
      printf ("%s", log_native);
    }
  else if (print_size)
    {
      printf ("0");
    }
  
  printf ("\n");
  return SVN_NO_ERROR;
}


/* Print the timestamp of the commit (in the revision case) or the
   empty string (in the transaction case) to stdout, followed by a
   newline. */
static svn_error_t *
do_date (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  if (c->is_revision)
    {
      svn_string_t *prop_value;
      
      SVN_ERR (get_property (&prop_value, c, SVN_PROP_REVISION_DATE, pool));

      if (prop_value && prop_value->data)
        {
          /* Convert the date for humans. */
          apr_time_t aprtime;
          
          SVN_ERR (svn_time_from_nts (&aprtime, prop_value->data, pool));
          printf ("%s", svn_time_to_human_nts (aprtime, pool));
        }
    }

  printf ("\n");
  return SVN_NO_ERROR;
}


/* Print the author of the commit to stdout, followed by a newline. */
static svn_error_t *
do_author (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  svn_string_t *prop_value;

  SVN_ERR (get_property (&prop_value, c, SVN_PROP_REVISION_AUTHOR, pool));

  if (prop_value && prop_value->data) {
    const char *author_native;
    SVN_ERR (svn_utf_cstring_from_utf8 (&author_native, prop_value->data,
                                        pool));
    printf ("%s", author_native);
  }
  
  printf ("\n");
  return SVN_NO_ERROR;
}


/* Print a list of all directories in which files, or directory
   properties, have been modified. */
static svn_error_t *
do_dirs_changed (svnlook_ctxt_t *c, apr_pool_t *pool)
{ 
  svn_fs_root_t *root;
  svn_revnum_t base_rev_id;
  svn_repos_node_t *tree;

  SVN_ERR (get_root (&root, c, pool));
  if (c->is_revision)
    base_rev_id = c->rev_id - 1;
  else
    base_rev_id = svn_fs_txn_base_revision (c->txn);

  if (! SVN_IS_VALID_REVNUM (base_rev_id))
    return svn_error_createf 
      (SVN_ERR_FS_NO_SUCH_REVISION, 0, NULL, pool,
       "Transaction '%s' is not based on a revision.  How odd.",
       c->txn_name);
  
  SVN_ERR (generate_delta_tree (&tree, c->repos, root, base_rev_id, 
                                TRUE, pool)); 
  if (tree)
    SVN_ERR (print_dirs_changed_tree (tree, "", pool));

  return SVN_NO_ERROR;
}


/* Print a list of all paths modified in a format compatible with `svn
   update'. */
static svn_error_t *
do_changed (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  svn_fs_root_t *root;
  svn_revnum_t base_rev_id;
  svn_repos_node_t *tree;

  SVN_ERR (get_root (&root, c, pool));
  if (c->is_revision)
    base_rev_id = c->rev_id - 1;
  else
    base_rev_id = svn_fs_txn_base_revision (c->txn);

  if (! SVN_IS_VALID_REVNUM (base_rev_id))
    return svn_error_createf 
      (SVN_ERR_FS_NO_SUCH_REVISION, 0, NULL, pool,
       "Transaction '%s' is not based on a revision.  How odd.",
       c->txn_name);
  
  SVN_ERR (generate_delta_tree (&tree, c->repos, root, base_rev_id, 
                                TRUE, pool)); 
  if (tree)
    SVN_ERR (print_changed_tree (tree, "", pool));

  return SVN_NO_ERROR;
}


/* Print some diff-y stuff in a TBD way. :-) */
static svn_error_t *
do_diff (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  svn_fs_root_t *root, *base_root;
  svn_revnum_t base_rev_id;
  svn_repos_node_t *tree;

  SVN_ERR (get_root (&root, c, pool));
  if (c->is_revision)
    base_rev_id = c->rev_id - 1;
  else
    base_rev_id = svn_fs_txn_base_revision (c->txn);

  if (! SVN_IS_VALID_REVNUM (base_rev_id))
    return svn_error_createf 
      (SVN_ERR_FS_NO_SUCH_REVISION, 0, NULL, pool,
       "Transaction '%s' is not based on a revision.  How odd.",
       c->txn_name);
  
  SVN_ERR (generate_delta_tree (&tree, c->repos, root, base_rev_id, 
                                TRUE, pool)); 
  if (tree)
    {
      svn_node_kind_t kind;
      SVN_ERR (svn_fs_revision_root (&base_root, c->fs, base_rev_id, pool));
      SVN_ERR (print_diff_tree (root, base_root, tree, "", "", pool));
      SVN_ERR (svn_io_check_path (SVNLOOK_TMPDIR, &kind, pool));
      if (kind == svn_node_dir)
        SVN_ERR (svn_io_remove_dir (SVNLOOK_TMPDIR, pool));
    }
  return SVN_NO_ERROR;
}


/* Print the diff between revision 0 and our root. */
static svn_error_t *
do_tree (svnlook_ctxt_t *c, svn_boolean_t show_ids, apr_pool_t *pool)
{
  svn_fs_root_t *root;
  svn_repos_node_t *tree;

  SVN_ERR (get_root (&root, c, pool));
  SVN_ERR (generate_delta_tree (&tree, c->repos, root, 0, FALSE, pool)); 
  if (tree)
    {
      if (show_ids)
        {
          SVN_ERR (print_ids_tree (tree, root, "", 0, pool));
        }
      else
        {
          SVN_ERR (print_tree (tree, 0, pool));
        }
    }

  return SVN_NO_ERROR;
}


/* Print author, date, log-size, and log associated with the given
   revision or transaction. */
static svn_error_t *
do_info (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  SVN_ERR (do_author (c, pool));
  SVN_ERR (do_date (c, pool));
  SVN_ERR (do_log (c, TRUE, pool));
  return SVN_NO_ERROR;
}


/* Print author, date, log-size, log, and the tree associated with the
   given revision or transaction. */
static svn_error_t *
do_default (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  SVN_ERR (do_info (c,pool));
  SVN_ERR (do_tree (c, FALSE, pool));
  return SVN_NO_ERROR;
}




/*** Argument parsing and usage. ***/
static void
do_usage (const char *progname, int exit_code)
{
  fprintf
    (exit_code ? stderr : stdout,
     "usage: %s REPOS_PATH rev REV [COMMAND] - inspect revision REV\n"
     "       %s REPOS_PATH txn TXN [COMMAND] - inspect transaction TXN\n"
     "       %s REPOS_PATH [COMMAND] - inspect the youngest revision\n"
     "\n"
     "REV is a revision number > 0.\n"
     "TXN is a transaction name.\n"
     "\n"
     "If no command is given, the default output (which is the same as\n"
     "running the subcommands `info' then `tree') will be printed.\n"
     "\n"
     "COMMAND can be one of: \n"
     "\n"
     "   author:        print author.\n"
     "   changed:       print full change summary: all dirs & files changed.\n"
     "   date:          print the timestamp (revisions only).\n"
     "   diff:          print GNU-style diffs of changed files and props.\n"
     "   dirs-changed:  print changed directories.\n"
     "   ids:           print the tree, with nodes ids.\n"
     "   info:          print the author, data, log_size, and log message.\n"
     "   log:           print log message.\n"
     "   tree:          print the tree.\n"
     "\n",
     progname,
     progname,
     progname);

  exit (exit_code);
}



/*** Main. ***/

#define INT_ERR(expr)                              \
  do {                                             \
    svn_error_t *svn_err__temp = (expr);           \
    if (svn_err__temp) {                           \
      svn_handle_error (svn_err__temp, stderr, 0); \
      goto cleanup; }                              \
  } while (0)


int
main (int argc, const char * const *argv)
{
  apr_pool_t *pool;
  const char *repos_path = NULL, *repos_path_utf8;
  const char *txn_name = NULL;
  int cmd_offset = 4;
  svnlook_cmd_t command;
  svnlook_ctxt_t c;
  svn_boolean_t was_success = FALSE;

  setlocale (LC_CTYPE, "");

  /* Initialize context variable. */
  memset (&c, 0, sizeof (c));
  c.rev_id = SVN_INVALID_REVNUM;

  /* We require at least 1 arguments. */
  if (argc < 2)
    {
      do_usage (argv[0], 1);
      return EXIT_FAILURE;
    }

  /* Argument 1 is the repository path. */
  repos_path = argv[1];

  /* Argument 2 could be "rev" or "txn".  If "rev", Argument 3 is a
     numerical revision number.  If "txn", Argument 3 is a transaction
     name string.  If neither, this is an inspection of the youngest
     revision.  */
  if (argc > 3)
    {
      if (! strcmp (argv[2], "txn")) /* transaction */
        {
          c.is_revision = FALSE;
          txn_name = argv[3];
        }
      else if (! strcmp (argv[2], "rev")) /* revision */
        {
          c.is_revision = TRUE;
          c.rev_id = SVN_STR_TO_REV (argv[3]);
          if (c.rev_id < 1)
            {
              do_usage (argv[0], 1);
              return EXIT_FAILURE;
            }
        }
      else
        {
          c.is_revision = TRUE;
          cmd_offset = 2;
        }
    }
  else
    {
      c.is_revision = TRUE;
      cmd_offset = 2;
    }

  /* If there is a subcommand, parse it. */
  if (argc > cmd_offset)
    {
      if (! strcmp (argv[cmd_offset], "author"))
        command = svnlook_cmd_author;
      else if (! strcmp (argv[cmd_offset], "changed"))
        command = svnlook_cmd_changed;
      else if (! strcmp (argv[cmd_offset], "date"))
        command = svnlook_cmd_date;
      else if (! strcmp (argv[cmd_offset], "diff"))
        command = svnlook_cmd_diff;
      else if (! strcmp (argv[cmd_offset], "dirs-changed"))
        command = svnlook_cmd_dirs_changed;
      else if (! strcmp (argv[cmd_offset], "ids"))
        command = svnlook_cmd_ids;
      else if (! strcmp (argv[cmd_offset], "info"))
        command = svnlook_cmd_info;
      else if (! strcmp (argv[cmd_offset], "log"))
        command = svnlook_cmd_log;
      else if (! strcmp (argv[cmd_offset], "tree"))
        command = svnlook_cmd_tree;
      else
        {
          do_usage (argv[0], 2);
          return EXIT_FAILURE;
        }
    }
  else
    {
      command = svnlook_cmd_default;
    }

  /* Now, let's begin processing.  */

  /* Initialize APR and our top-level pool. */
  apr_initialize ();
  pool = svn_pool_create (NULL);

  /* Convert repository path and txn name (if present) to UTF-8 */
  INT_ERR (svn_utf_cstring_to_utf8 (&repos_path_utf8, repos_path, NULL, pool));
  repos_path_utf8 = svn_path_canonicalize_nts (repos_path_utf8, pool);
  if(txn_name)
    INT_ERR (svn_utf_cstring_to_utf8 (&c.txn_name, txn_name, NULL, pool));    

  /* Open the repository with the given path. */
  INT_ERR (svn_repos_open (&(c.repos), repos_path_utf8, pool));
  c.fs = svn_repos_fs (c.repos);

  /* If this is a transaction, open the transaction. */
  if (! c.is_revision)
    INT_ERR (svn_fs_open_txn (&(c.txn), c.fs, c.txn_name, pool));
 
  /* If this is a revision with an invalid revision number, just use
     the head revision. */
  if (c.is_revision && (! SVN_IS_VALID_REVNUM (c.rev_id)))
    INT_ERR (svn_fs_youngest_rev (&(c.rev_id), c.fs, pool));

  /* Now, out context variable is full of all the stuff we might need
     to know.  Get to work.  */
  switch (command)
    {
    case svnlook_cmd_author:
      INT_ERR (do_author (&c, pool));
      break;

    case svnlook_cmd_changed:
      INT_ERR (do_changed (&c, pool));
      break;

    case svnlook_cmd_date:
      INT_ERR (do_date (&c, pool));
      break;

    case svnlook_cmd_diff:
      INT_ERR (do_diff (&c, pool));
      break;

    case svnlook_cmd_dirs_changed:
      INT_ERR (do_dirs_changed (&c, pool));
      break;

    case svnlook_cmd_ids:
      INT_ERR (do_tree (&c, TRUE, pool));
      break;

    case svnlook_cmd_info:
      INT_ERR (do_info (&c, pool));
      break;

    case svnlook_cmd_log:
      INT_ERR (do_log (&c, FALSE, pool));
      break;

    case svnlook_cmd_tree:
      INT_ERR (do_tree (&c, FALSE, pool));
      break;

    case svnlook_cmd_default:
    default:
      INT_ERR (do_default (&c, pool));
      break;
    }

  /* We were successful in our subcommand. */
  was_success = TRUE;

 cleanup:  /* Cleanup after ourselves. */
  if (c.txn && (! c.is_revision))
    svn_fs_close_txn (c.txn);

  if (c.repos)
    svn_repos_close (c.repos);

  svn_pool_destroy (pool);
  apr_terminate ();

  return was_success ? EXIT_SUCCESS : EXIT_FAILURE;
}




/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */
