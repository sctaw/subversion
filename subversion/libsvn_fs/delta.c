/* delta.c --- comparing trees and files
 *
 * ================================================================
 * Copyright (c) 2000 Collab.Net.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. The end-user documentation included with the redistribution, if
 * any, must include the following acknowlegement: "This product includes
 * software developed by Collab.Net (http://www.Collab.Net/)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 * 
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 * 
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of Collab.Net.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL COLLABNET OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by many
 * individuals on behalf of Collab.Net.
 */

#include <string.h>
#include "apr_general.h"
#include "apr_pools.h"
#include "apr_hash.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_fs.h"
#include "fs.h"
#include "err.h"


/* Some datatypes and declarations used throughout the file.  */


/* Parameters which remain constant throughout a delta traversal.
   At the top of the recursion, we initialize one of these structures.
   Then, we pass it down, unchanged, to every call.  This way,
   functions invoked deep in the recursion can get access to this
   traversal's global parameters, without using global variables.  */
struct context {
  svn_delta_edit_fns_t *editor;
  apr_pool_t *pool;
};


/* The type of a function that accepts changes to an object's property
   list.  OBJECT is the object whose properties are being changed.
   NAME is the name of the property to change.  VALUE is the new value
   for the property, or zero if the property should be deleted.  */
typedef svn_error_t *proplist_change_fn_t (void *object,
					   svn_string_t *name,
					   svn_string_t *value);


/* This will soon be defined in svn_error.h.  When it is, delete this.  */
#define SVN_ERR(expr) \
  do { svn_error_t *svn_err = (expr); if (svn_err) return svn_err; } while (0)



/* Forward declarations for each section's public functions.  */

/* See the functions themselves for descriptions.  */
static svn_error_t *delta_dirs (struct context *c, void *dir_baton,
				svn_fs_dir_t *source,
				svn_string_t *source_path,
				svn_fs_dir_t *target);
static svn_error_t *replace (struct context *c, void *dir_baton,
			     svn_fs_dir_t *source, svn_string_t *source_path,
			     svn_fs_dir_t *target,
			     svn_fs_dirent_t *target_entry);
static svn_error_t *delete (struct context *c, void *dir_baton,
			    svn_string_t *name);
static svn_error_t *add (struct context *c, void *dir_baton,
			 svn_fs_dir_t *source, svn_string_t *source_path,
			 svn_fs_dir_t *target, svn_string_t *name);
static svn_error_t *delta_files (struct context *c, void *file_baton,
				 svn_fs_file_t *ancestor_file,
				 svn_fs_file_t *target_file);
static svn_error_t *file_from_scratch (struct context *c,
				       void *file_baton,
				       svn_fs_file_t *target_file);
static svn_error_t *delta_proplists (struct context *c,
				     svn_fs_proplist_t *source,
				     svn_fs_proplist_t *target,
				     proplist_change_fn_t *change_fn,
				     void *object);
static svn_error_t *dir_from_scratch (struct context *c,
				      void *dir_baton,
				      svn_fs_dir_t *target);


/* Public interface to delta computation.  */


svn_error_t *
svn_fs_dir_delta (svn_fs_dir_t *source,
		  svn_fs_dir_t *target,
		  svn_delta_edit_fns_t *editor,
		  void *edit_baton,
		  apr_pool_t *parent_pool)
{
  svn_error_t *svn_err = 0;
  apr_pool_t *pool = svn_pool_create (parent_pool, svn_fs__pool_abort);
  svn_string_t source_path;
  void *root_baton;
  struct context c;

  source_path.len = 0;

  svn_err = editor->replace_root (NULL, 0, edit_baton, &root_baton);
  if (svn_err) goto error;

  c.editor = editor;
  c.pool = pool;

  svn_err = delta_dirs (&c, root_baton, source, &source_path, target);
  if (svn_err) goto error;

  svn_err = editor->close_directory (root_baton);
  if (svn_err) goto error;

 error:
  apr_destroy_pool (pool);
  return svn_err;
}



/* Compare two directories.  */


/* Forward declarations for functions local to this section.
   See the functions themselves for descriptions.  */
static svn_error_t *delta_dir_props (struct context *c,
				     void *dir_baton,
				     svn_fs_dir_t *source,
				     svn_fs_dir_t *target);
static svn_error_t *delta_dirent_props (struct context *c, void *dir_baton,
					svn_fs_dir_t *source,
					svn_fs_dir_t *target,
					svn_string_t *name);
static svn_error_t *change_dirent_plist (void *object,
					 svn_string_t *name,
					 svn_string_t *value);


/* Emit deltas to turn SOURCE into TARGET_DIR.  Assume that DIR_BATON
   represents the directory we're constructing to the editor in the
   context C.  SOURCE_PATH is the path to SOURCE, relative to the top
   of the delta, or the empty string if SOURCE is the top itself.  */
static svn_error_t *
delta_dirs (struct context *c, void *dir_baton,
	    svn_fs_dir_t *source, svn_string_t *source_path,
	    svn_fs_dir_t *target)
{
  svn_fs_dirent_t **source_entries, **target_entries;
  int si, ti;

  /* Compare the property lists.  */
  SVN_ERR (delta_dir_props (c, dir_baton, source, target));

  /* Get the list of entries in each of source and target.  */
  SVN_ERR (svn_fs_dir_entries (&source_entries, source));
  SVN_ERR (svn_fs_dir_entries (&target_entries, target));

  si = 0, ti = 0;
  while (source_entries[si] || target_entries[ti])
    {
      /* Compare the names of the current directory entries in both
	 source and target.  If they're equal, then we've found an
	 entry common to both directories.  Otherwise, whichever entry
	 comes `earlier' in the sort order doesn't exist in the other
	 directory, so we've got an add or a delete.

	 (Note: it's okay if si or ti point at the zero that
	 terminates the arrays; see the comments for
	 svn_fs_compare_dirents.)  */
      int name_cmp = svn_fs_compare_dirents (source_entries[si],
					     target_entries[ti]);

      /* Does an entry by this name exist in both the source and the
         target?  */
      if (name_cmp == 0)
	{
	  /* Both the source and the target have a directory entry by
	     the same name.  Note any changes to the directory entry's
	     properties.  */
	  SVN_ERR (delta_dirent_props (c, dir_baton,
				       source, target,
				       source_entries[si]->name));

	  /* Compare the node numbers.  */
	  if (! svn_fs_id_eq (source_entries[si]->id, target_entries[ti]->id))
	    {
	      /* The name is the same, but the node has changed.
		 This is a replace.  */
	      SVN_ERR (replace (c, dir_baton,
				source, source_path,
				target, source_entries[si]));
	    }

	  /* This entry is now dealt with in both the source and target.  */
	  si++, ti++;
	}

      /* If the current source entry is "before" the current target
	 entry, then that source entry was deleted.  */
      else if (name_cmp < 0)
	{
	  SVN_ERR (delete (c, dir_baton, source_entries[si]->name));
	  si++;
	}

      /* A new entry has been added.  */
      else
	{
	  SVN_ERR (add (c, dir_baton,
			source, source_path, target,
			target_entries[ti]->name));
	  ti++;
	}
    }

  return 0;
}


/* Comparing directories' property lists.  */
static svn_error_t *
delta_dir_props (struct context *c,
		 void *dir_baton,
		 svn_fs_dir_t *source,
		 svn_fs_dir_t *target)
{
  svn_fs_proplist_t *source_props = svn_fs_dir_proplist (source);
  svn_fs_proplist_t *target_props = svn_fs_dir_proplist (target);

  return delta_proplists (c, source_props, target_props,
			  c->editor->change_dir_prop, dir_baton);
}



/* A temporary baton for changing directory entry property lists.  */
struct dirent_plist_baton {

  /* The editor for these changes.  */
  svn_delta_edit_fns_t *editor;
  
  /* The baton for the directory whose entry's properties are being
     changed.  */
  void *dir_baton;

  /* The name of the entry whose properties are being changed.  */
  svn_string_t *entry_name;
};


/* Given that both SOURCE and TARGET have a directory entry named
   NAME, compare the two entries' property lists.  Emit whatever edits
   are necessary to turn SOURCE's entry's property list into TARGET's
   entry's property list.  DIR_BATON is the directory baton
   corresponding to the target directory.  */
static svn_error_t *
delta_dirent_props (struct context *c, void *dir_baton,
		    svn_fs_dir_t *source, svn_fs_dir_t *target,
		    svn_string_t *name)
{
  svn_fs_proplist_t *source_props;
  svn_fs_proplist_t *target_props;
  struct dirent_plist_baton dirent;

  SVN_ERR (svn_fs_dirent_proplist (&source_props, source, name));
  SVN_ERR (svn_fs_dirent_proplist (&target_props, target, name));

  dirent.editor = c->editor;
  dirent.dir_baton = dir_baton;
  dirent.entry_name = name;

  return delta_proplists (c, source_props, target_props,
			  change_dirent_plist, &dirent);
}


static svn_error_t *
change_dirent_plist (void *object,
		     svn_string_t *name,
		     svn_string_t *value)
{
  struct dirent_plist_baton *dirent = (struct dirent_plist_baton *) object;

  return dirent->editor->change_dirent_prop (dirent->dir_baton,
					     dirent->entry_name,
					     name, value);
}


/* Doing replaces.  */


/* Forward declarations for functions local to this section.
   See the functions themselves for descriptions.  */
static svn_error_t *replace_related (struct context *c,
				     void *dir_baton,
				     svn_fs_dir_t *target,
				     svn_string_t *target_name,
				     svn_fs_dir_t *ancestor_dir,
				     svn_string_t *ancestor_dir_path,
				     svn_string_t *ancestor_name);
static svn_error_t *replace_from_scratch (struct context *c, void *dir_baton,
					  svn_fs_dir_t *target,
					  svn_string_t *name);


/* Do a `replace' edit in DIR_BATON turning the entry named
   TARGET_ENTRY->name in SOURCE into the corresponding entry in
   TARGET.  SOURCE_PATH is the path to SOURCE, relative to the top of
   the delta, or the empty string if SOURCE is the top itself.

   Emit a replace_dir or replace_file as needed.  Choose an
   appropriate ancestor, or describe the tree from scratch.  */
   
static svn_error_t *
replace (struct context *c, void *dir_baton,
	 svn_fs_dir_t *source, svn_string_t *source_path,
	 svn_fs_dir_t *target,
	 svn_fs_dirent_t *target_entry)
{
  svn_fs_dirent_t **source_entries;
  int best, best_distance;

  /* Get the list of entries in SOURCE.  */
  SVN_ERR (svn_fs_dir_entries (&source_entries, source));

  /* Find the closest relative to TARGET_ENTRY in SOURCE.
     
     In principle, a replace operation can choose the ancestor from
     anywhere in the delta's whole source tree.  In this
     implementation, we only search SOURCE for possible ancestors.
     This will need to improve, so we can find the best ancestor, no
     matter where it's hidden away in the source tree.  */
  {
    int i;

    best = -1;
    for (i = 0; source_entries[i]; i++)
      {
	/* Find the distance between the target entry and this source
	   entry.  This returns -1 if they're completely unrelated.
	   Here we're using ID distance as an approximation for delta
	   size.  */
	int distance = svn_fs_id_distance (target_entry->id,
					   source_entries[i]->id);

	if (distance != -1
	    && (best == -1 || distance < best_distance))
	  {
	    best = i;
	    best_distance = distance;
	  }
      }
  }

  if (best == -1)
    /* We can't find anything related to this file / directory.
       Send it from scratch.  */
    SVN_ERR (replace_from_scratch (c, dir_baton, target, target_entry->name));
  else
    /* We've found an ancestor; do a replace relative to that.  */
    SVN_ERR (replace_related (c, dir_baton,
			      target, target_entry->name, 
			      source, source_path,
			      source_entries[best]->name));

  return 0;
}


/* Replace the directory entry named NAME in DIR_BATON with a new
   node, for which we have no ancestor.  The new node is the entry
   named NAME in TARGET.  */
static svn_error_t *
replace_from_scratch (struct context *c, void *dir_baton,
		      svn_fs_dir_t *target, svn_string_t *name)
{
  svn_fs_node_kind_t kind;

  /* Is it a file or a directory?  */
  SVN_ERR (svn_fs_type (&kind, target, name));
  if (kind == svn_fs_node_file)
    {
      svn_fs_file_t *file;
      void *file_baton;

      SVN_ERR (svn_fs_open_file (&file, target, name, c->pool));
      SVN_ERR (c->editor->replace_file (name, dir_baton, 0, 0, &file_baton));
      SVN_ERR (file_from_scratch (c, file_baton, file));

      svn_fs_close_file (file);

      SVN_ERR (c->editor->close_file (file_baton));
    }
  else if (kind == svn_fs_node_dir)
    {
      svn_fs_dir_t *subdir;
      void *subdir_baton;

      SVN_ERR (svn_fs_open_subdir (&subdir, target, name, c->pool));
      SVN_ERR (c->editor->replace_directory (name, dir_baton,
					     0, 0, &subdir_baton));

      SVN_ERR (dir_from_scratch (c, subdir_baton, subdir));

      svn_fs_close_dir (subdir);

      SVN_ERR (c->editor->close_directory (subdir_baton));
    }
  else
    abort ();

  return 0;
}


/* Do a replace, with a known ancestor.

   Replace the entry named TARGET_NAME in the directory DIR_BATON with
   the node of the same name in TARGET, using the entry named
   ANCESTOR_NAME in ANCESTOR_DIR as the ancestor.  ANCESTOR_DIR_PATH
   is the path to ANCESTOR_DIR from the top of the delta.  */
static svn_error_t *
replace_related (struct context *c, void *dir_baton,
		 svn_fs_dir_t *target, svn_string_t *target_name,
		 svn_fs_dir_t *ancestor_dir, svn_string_t *ancestor_dir_path,
		 svn_string_t *ancestor_name)
{
  svn_string_t *ancestor_path;
  svn_fs_node_kind_t kind;

  /* Compute the full name of the ancestor.  */
  ancestor_path = svn_string_dup (ancestor_dir_path, c->pool);
  svn_path_add_component (ancestor_path, ancestor_name,
			  SVN_PATH_REPOS_STYLE, c->pool);

  SVN_ERR (svn_fs_type (&kind, target, target_name));

  if (kind == svn_fs_node_file)
    {
      svn_fs_file_t *ancestor_file;
      svn_fs_file_t *target_file;
      void *file_baton;
      svn_vernum_t ancestor_version;

      /* Open the ancestor file.  */
      SVN_ERR (svn_fs_open_file (&ancestor_file,
				 ancestor_dir, ancestor_name,
				 c->pool));

      /* Get the ancestor version.  */
      ancestor_version = svn_fs_file_version (ancestor_file);

      /* Open the target file.  */
      SVN_ERR (svn_fs_open_file (&target_file,
				 target, target_name,
				 c->pool));

      /* Do the replace, yielding a baton for the file.  */
      SVN_ERR (c->editor->replace_file (target_name, dir_baton,
					ancestor_path, ancestor_version,
					&file_baton));

      /* Apply the text delta.  */
      SVN_ERR (delta_files (c, file_baton, ancestor_file, target_file));

      /* Close the ancestor and target files.  */
      svn_fs_close_file (ancestor_file);
      svn_fs_close_file (target_file);

      /* Close the editor's file baton.  */
      SVN_ERR (c->editor->close_file (file_baton));
    }
  else if (kind == svn_fs_node_dir)
    {
      svn_fs_dir_t *ancestor_subdir;
      svn_fs_dir_t *target_subdir;
      void *subdir_baton;
      svn_vernum_t ancestor_version;

      /* Open the ancestor directory.  */
      SVN_ERR (svn_fs_open_subdir (&ancestor_subdir,
				   ancestor_dir, ancestor_name,
				   c->pool));

      /* Get the ancestor version.  */
      ancestor_version = svn_fs_dir_version (ancestor_subdir);

      /* Open the target directory.  */
      SVN_ERR (svn_fs_open_subdir (&target_subdir,
				   target, target_name,
				   c->pool));
      
      /* Do the replace, yielding a baton for the new subdirectory.  */
      SVN_ERR (c->editor->replace_directory (target_name,
					     dir_baton,
					     ancestor_path, ancestor_version,
					     &subdir_baton));

      /* Compute the delta for those subdirs.  */
      SVN_ERR (delta_dirs (c, subdir_baton,
			   ancestor_subdir, ancestor_path, target_subdir));

      /* Close the ancestor and target directories.  */
      svn_fs_close_dir (ancestor_subdir);
      svn_fs_close_dir (target_subdir);
      
      /* Close the editor's subdirectory baton.  */
      SVN_ERR (c->editor->close_directory (subdir_baton));
    }
  else
    abort ();

  return 0;
}



/* Doing deletes.  */


/* Emit a delta to delete the entry named NAME from DIR_BATON.  */
static svn_error_t *
delete (struct context *c, void *dir_baton,
	svn_string_t *name)
{
  return c->editor->delete (name, dir_baton);
}



/* Doing adds.  */

static svn_error_t *
add (struct context *c, void *dir_baton,
     svn_fs_dir_t *source, svn_string_t *source_path,
     svn_fs_dir_t *target, svn_string_t *name)
{
  /* ...; */
}


/* Compare two files.  */


/* Forward declarations for functions local to this section.
   See the functions themselves for descriptions.  */
static svn_error_t *delta_file_props (struct context *c,
				      void *file_baton,
				      svn_fs_file_t *ancestor_file,
				      svn_fs_file_t *target_file);
static svn_error_t *send_text_delta (struct context *c,
				     void *file_baton,
				     svn_read_fn_t *ancestor_read_fn,
				     void *ancestor_read_baton,
				     svn_read_fn_t *target_read_fn,
				     void *target_read_baton);
static svn_error_t *null_read_fn (void *baton,
				  char *buffer, apr_size_t *len,
				  apr_pool_t *pool);


/* Make the appropriate edits on FILE_BATON to change its contents and
   properties from those on ANCESTOR_FILE to those on TARGET_FILE.  */
static svn_error_t *
delta_files (struct context *c, void *file_baton,
	     svn_fs_file_t *ancestor_file,
	     svn_fs_file_t *target_file)
{
  svn_read_fn_t *ancestor_read_fn, *target_read_fn;
  void *ancestor_read_baton, *target_read_baton;

  /* Compare the files' property lists.  */
  SVN_ERR (delta_file_props (c, file_baton, ancestor_file, target_file));

  /* Get read functions for the file contents.  */
  SVN_ERR (svn_fs_file_contents (&ancestor_read_fn, ancestor_read_baton,
				 ancestor_file));
  SVN_ERR (svn_fs_file_contents (&target_read_fn, target_read_baton,
				 target_file));

  SVN_ERR (send_text_delta (c, file_baton,
			    ancestor_read_fn, ancestor_read_baton,
			    target_read_fn, target_read_baton));

  return 0;
}


/* Make the appropriate edits on FILE_BATON to change its contents and
   properties from the empty file (no contents, no properties) to
   those of TARGET_FILE.  */
static svn_error_t *
file_from_scratch (struct context *c,
		   void *file_baton,
		   svn_fs_file_t *target_file)
{
  svn_read_fn_t *target_read_fn;
  void *target_read_baton;

  /* Put the right properties on there.  */
  SVN_ERR (delta_file_props (c, file_baton, 0, target_file));

  /* Get a read function for the target file's contents.  */
  SVN_ERR (svn_fs_file_contents (&target_read_fn, target_read_baton,
				 target_file));

  SVN_ERR (send_text_delta (c, file_baton,
			    null_read_fn, 0,
			    target_read_fn, target_read_baton));

  return 0;
}


/* Generate the appropriate change_file_prop calls to turn the properties
   of ANCESTOR_FILE into those of TARGET_FILE.  If ANCESTOR_FILE is zero, 
   treat it as if it were a file with no properties.  */
static svn_error_t *
delta_file_props (struct context *c,
		  void *file_baton,
		  svn_fs_file_t *ancestor_file,
		  svn_fs_file_t *target_file)
{
  svn_fs_proplist_t *ancestor_props = svn_fs_file_proplist (ancestor_file);
  svn_fs_proplist_t *target_props = svn_fs_file_proplist (target_file);

  return delta_proplists (c, ancestor_props, target_props,
			  c->editor->change_file_prop, file_baton);
}


/* A read function representing the empty string/file.  */
static svn_error_t *
null_read_fn (void *baton,
	      char *buffer,
	      apr_size_t *len,
	      apr_pool_t *pool)
{
  *len = 0;

  return 0;
}


/* Generate a text delta that will turn the stream represented by
   ANCESTOR_READ_FN and ANCESTOR_READ_BATON into the stream
   represented by TARGET_READ_FN and TARGET_READ_BATON.  Apply that
   text delta to FILE_BATON.  */
static svn_error_t *
send_text_delta (struct context *c,
		 void *file_baton,
		 svn_read_fn_t *ancestor_read_fn,
		 void *ancestor_read_baton,
		 svn_read_fn_t *target_read_fn,
		 void *target_read_baton)
{
  svn_txdelta_stream_t *delta_stream;
  svn_txdelta_window_handler_t *delta_handler;
  void *delta_handler_baton;

  /* Create a delta stream that turns the ancestor into the target.  */
  SVN_ERR (svn_txdelta (&delta_stream,
			ancestor_read_fn, ancestor_read_baton,
			target_read_fn, target_read_baton,
			c->pool));

  /* Get a handler that will apply the delta to the file.  */
  SVN_ERR (c->editor->apply_textdelta (file_baton,
				       &delta_handler, &delta_handler_baton));

  /* Read windows from the delta stream, and apply them to the file.  */
  {
    svn_txdelta_window_t *window;
    do
      {
	SVN_ERR (svn_txdelta_next_window (&window, delta_stream));
	SVN_ERR (delta_handler (window, delta_handler_baton));
      }
    while (window);
  }

  svn_txdelta_free (delta_stream);

  return 0;
}



/* Compare two property lists.  */


/* Compare the two property lists SOURCE and TARGET.  For every
   difference found, generate an appropriate call to CHANGE_FN, on
   OBJECT.  */
static svn_error_t *
delta_proplists (struct context *c,
		 svn_fs_proplist_t *source,
		 svn_fs_proplist_t *target,
		 proplist_change_fn_t *change_fn,
		 void *object)
{
  /* It would be nice if we could figure out some way to use the
     history information to avoid reading in and scanning the entire
     property lists.  */

  svn_string_t **source_names, **target_names;
  apr_hash_t *source_values, *target_values;
  int si, ti;

  /* Get the names and values of the source file's properties.  If
     SOURCE is zero, treat that like an empty property list.  */
  if (source)
    {
      SVN_ERR (svn_fs_proplist_names (&source_names, source, c->pool));
      SVN_ERR (svn_fs_proplist_hash_table (&source_values, source, c->pool));
    }
  else
    {
      static svn_string_t *null_prop_name_list[] = { 0 };

      source_names = null_prop_name_list;
      /* It doesn't matter what we set source_values to, because we should
	 never fetch anything from it.  */
      source_values = 0;
    }

  /* Get the names and values of the target file's properties.  */
  SVN_ERR (svn_fs_proplist_names (&target_names, target, c->pool));
  SVN_ERR (svn_fs_proplist_hash_table (&target_values, target, c->pool));

  si = ti = 0;
  while (source_names[si] || target_names[ti])
    {
      svn_string_t *sn = source_names[si];
      svn_string_t *tn = target_names[ti];
      int cmp = svn_fs_compare_prop_names (sn, tn);

      /* If the two names are equal, then a property by the given name
         exists on both files.  */
      if (cmp == 0)
	{
	  /* Get the values of the property.  */
	  svn_string_t *sv = apr_hash_get (source_values, sn->data, sn->len);
	  svn_string_t *tv = apr_hash_get (target_values, tn->data, tn->len);

	  /* Does the property have the same value on both files?  */
	  if (! svn_string_compare (sv, tv))
	    SVN_ERR (change_fn (object, tn, tv));

	  si++, ti++;
	}
      /* If the source name comes earlier, then it's been deleted.  */
      else if (cmp < 0)
	{
	  SVN_ERR (change_fn (object, sn, 0));
	  si++;
	}
      /* If the target name comes earlier, then it's been added.  */
      else
	{
	  /* Get the value of the property.  */
	  svn_string_t *tv = apr_hash_get (target_values, tn->data, tn->len);
	  SVN_ERR (change_fn (object, tn, tv));
	  ti++;
	}
    }

  return 0;
}



/* Building directory trees from scratch.  */

static svn_error_t *
dir_from_scratch (struct context *c,
		  void *dir_baton,
		  svn_fs_dir_t *target)
{
  /* ...; */
}
