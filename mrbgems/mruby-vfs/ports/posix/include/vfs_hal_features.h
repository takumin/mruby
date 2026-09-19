/*
** vfs_hal_features.h - what the POSIX port of mruby-vfs implements
**
** See Copyright Notice in mruby.h
**
** The gem's include/vfs_hal.h reads this before it declares anything.  A
** macro defined here guards three things at once: the prototypes there, the
** implementation in vfs_hal.c, and the `VFS::Host` class and its root mount
** under src/.  A port that declared a capability and did not implement it
** would fail to link, and one that declares nothing owes nothing.
*/

#ifndef MRUBY_VFS_HAL_FEATURES_H
#define MRUBY_VFS_HAL_FEATURES_H

/* stat(2), open(2) and read(2) over the host's files: `VFS::Host`, mounted
   at the root when the gem initializes. */
#define MRB_HAL_VFS_HAS_HOST

#endif /* MRUBY_VFS_HAL_FEATURES_H */
