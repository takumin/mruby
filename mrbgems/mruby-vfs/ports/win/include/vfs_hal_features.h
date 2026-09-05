/*
** vfs_hal_features.h - what the Windows port of mruby-vfs implements
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

/* _wstat64(), _wopen() and _read() over the host's files, with UTF-8 paths
   converted to UTF-16: `VFS::Host`, mounted at the root when the gem
   initializes. */
#define MRB_HAL_VFS_HAS_HOST

#endif /* MRUBY_VFS_HAL_FEATURES_H */
