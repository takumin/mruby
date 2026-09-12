/*
** vfs_hal.h - Host filesystem HAL interface for mruby-vfs
**
** See Copyright Notice in mruby.h
**
** Hardware Abstraction Layer over the host's own files, for the `VFS::Host`
** backend.  The port answers what a path names and hands over the bytes of
** a regular file; the gem reads them whole.  All paths are UTF-8.
*/

#ifndef MRUBY_VFS_HAL_H
#define MRUBY_VFS_HAL_H

#include <mruby.h>
#include <mruby/vfs.h>

/*
 * What the port implements
 *
 * The port publishes it in a header under its include/, and the build puts
 * that directory on the include path of this gem and of every gem that
 * depends on it.  Whether the host's files are reachable is the port's to
 * say, because the port is what a build names: a cross build has no host to
 * detect one from, and a `hal-vfs-<conf>` gem may stand in for the bundled
 * ports altogether.  A port that declares nothing owes nothing; the gem then
 * has no `VFS::Host`, mounts nothing on its own, and a build mounts what it
 * has, a `VFS::Memory` or a backend of its own, from Ruby or C.
 */
#include "vfs_hal_features.h"

MRB_BEGIN_DECL

#ifdef MRB_HAL_VFS_HAS_HOST

/*
 * Platform-independent open file handle
 * Each HAL implementation defines this structure internally
 */
typedef struct mrb_vfs_hal_file mrb_vfs_hal_file;

/* What `path` names, and for a regular file its size in bytes, -1 when that
   is unknown or does not fit an mrb_int.  Returns 0, with MRB_VFS_NONE when
   there is no such entry, or -1 with errno set when the host could not say. */
int mrb_hal_vfs_stat(mrb_state *mrb, const char *path, enum mrb_vfs_kind *kind, mrb_int *size);

/* Open `path` for reading.  Returns NULL with errno set on error. */
mrb_vfs_hal_file* mrb_hal_vfs_open(mrb_state *mrb, const char *path);

/* Read up to `count` bytes into `buf`.  Returns the number read, 0 at end of
   file, or -1 with errno set on error; an interrupted read is retried by the
   port, not reported. */
mrb_int mrb_hal_vfs_read(mrb_state *mrb, mrb_vfs_hal_file *file, void *buf, size_t count);

/* Close `file` and release the handle.  Returns 0, or -1 with errno set. */
int mrb_hal_vfs_close(mrb_state *mrb, mrb_vfs_hal_file *file);

/*
 * HAL Initialization/Finalization
 */

/* Initialize HAL (called once at gem initialization) */
void mrb_hal_vfs_init(mrb_state *mrb);

/* Cleanup HAL (called once at gem finalization) */
void mrb_hal_vfs_final(mrb_state *mrb);

#endif /* MRB_HAL_VFS_HAS_HOST */

MRB_END_DECL

#endif /* MRUBY_VFS_HAL_H */
