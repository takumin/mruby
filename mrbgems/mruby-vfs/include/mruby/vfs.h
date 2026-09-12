/*
** mruby/vfs.h - virtual filesystem for mruby
**
** See Copyright Notice in mruby.h
**
** The VFS is a table of backends mounted at path prefixes.  A path is
** answered by the mount with the longest prefix that covers it; the mount
** at "/" (the root) covers every path no other mount claims, and the port's
** host filesystem sits there when the port has one.  A backend answers two
** questions, what kind of entry a path names and what bytes a regular file
** holds, and `require`, `require_relative` and `load` ask nothing else.
**
** A backend written in C is a table of operations wrapped by
** mrb_vfs_backend_new(); one written in Ruby is any object that responds to
** `stat(path)` with :file, :directory, :other or nil, and to `read(path)`
** with a String or nil.  What the backend sees is the path below its mount,
** starting with the slash; the root mount sees the path as it was given.
*/

#ifndef MRUBY_VFS_H
#define MRUBY_VFS_H

#include <mruby.h>

MRB_BEGIN_DECL

/* What a path names. */
enum mrb_vfs_kind {
  MRB_VFS_NONE = 0,       /* no such entry */
  MRB_VFS_FILE,           /* a regular file, which read() can answer */
  MRB_VFS_DIRECTORY,
  MRB_VFS_OTHER           /* a device, a socket, a fifo: anything else */
};

/*
 * Operations of a backend written in C.
 *
 * `data` is what mrb_vfs_backend_new() was handed.  `path` is the path the
 * backend sees, a NUL-terminated UTF-8 string.
 */
typedef struct mrb_vfs_ops {
  /* Write what `path` names to `*kind` and return 0; MRB_VFS_NONE when there
     is no such entry.  Return -1 with errno set when the question itself
     could not be answered. */
  int (*stat)(mrb_state *mrb, void *data, const char *path, enum mrb_vfs_kind *kind);

  /* The whole content of the regular file at `path` as a String, or nil when
     there is no such entry.  A file that cannot be read raises, through
     mrb_sys_fail() when errno says why. */
  mrb_value (*read)(mrb_state *mrb, void *data, const char *path);

  /* Release `data` when the backend object is collected.  May be NULL.
     Named as mrb_data_type names its own, and not `free`, which is a macro
     in a build that compiles everything as one translation unit. */
  void (*dfree)(mrb_state *mrb, void *data);
} mrb_vfs_ops;

/* A VFS::Backend object over `ops`; `ops` must outlive it. */
MRB_API mrb_value mrb_vfs_backend_new(mrb_state *mrb, const mrb_vfs_ops *ops, void *data);

/* Mount `backend` at `prefix`, replacing whatever was mounted there.  The
   prefix is an absolute path; "/" is the root.  Returns the backend. */
MRB_API mrb_value mrb_vfs_mount(mrb_state *mrb, const char *prefix, mrb_value backend);

/* Unmount whatever is at `prefix`.  Returns the backend, or nil. */
MRB_API mrb_value mrb_vfs_umount(mrb_state *mrb, const char *prefix);

/* What `path` names, through the mount that covers it: 0 with `*kind` set,
   MRB_VFS_NONE when nothing is mounted there or the entry is absent; -1 with
   errno when the backend could not answer. */
MRB_API int mrb_vfs_stat(mrb_state *mrb, const char *path, enum mrb_vfs_kind *kind);
MRB_API int mrb_vfs_stat_str(mrb_state *mrb, mrb_value path, enum mrb_vfs_kind *kind);

/* The content of the regular file at `path` as a String, or nil when there
   is no such entry.  Raises when the file cannot be read. */
MRB_API mrb_value mrb_vfs_read(mrb_state *mrb, const char *path);
MRB_API mrb_value mrb_vfs_read_str(mrb_state *mrb, mrb_value path);

/* Run the Ruby source or RITE bytecode at `path` at the top level, as
   `Kernel#load` does, and return its value.  Raises LoadError when there is
   no such file, and whatever the file raises. */
MRB_API mrb_value mrb_vfs_load(mrb_state *mrb, const char *path);

/* What `Kernel#require` does: TRUE when the feature was loaded now, FALSE
   when it already had been.  Raises LoadError when it is not found. */
MRB_API mrb_bool mrb_vfs_require(mrb_state *mrb, const char *feature);

/* Append `dir` to $LOAD_PATH. */
MRB_API void mrb_vfs_load_path_push(mrb_state *mrb, const char *dir);

MRB_END_DECL

#endif /* MRUBY_VFS_H */
