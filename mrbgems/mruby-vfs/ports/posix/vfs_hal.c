/*
** vfs_hal.c - POSIX HAL implementation for mruby-vfs
**
** See Copyright Notice in mruby.h
**
** POSIX implementation for reading the host's files.
** Supported platforms: Linux, macOS, BSD, Unix
*/

#include <mruby.h>
#include "vfs_hal.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* On POSIX, mrb_vfs_hal_file wraps a file descriptor */
struct mrb_vfs_hal_file {
  int fd;
};

int
mrb_hal_vfs_stat(mrb_state *mrb, const char *path, enum mrb_vfs_kind *kind, mrb_int *size)
{
  struct stat sb;
  (void)mrb;

  *size = -1;
  if (stat(path, &sb) == -1) {
    if (errno == ENOENT || errno == ENOTDIR) {
      *kind = MRB_VFS_NONE;
      return 0;
    }
    return -1;
  }
  if (S_ISREG(sb.st_mode)) {
    *kind = MRB_VFS_FILE;
    /* off_t may be wider than mrb_int; a size that does not fit is unknown */
    if (sb.st_size >= 0 && (uint64_t)sb.st_size <= (uint64_t)MRB_INT_MAX) {
      *size = (mrb_int)sb.st_size;
    }
  }
  else if (S_ISDIR(sb.st_mode)) {
    *kind = MRB_VFS_DIRECTORY;
  }
  else {
    *kind = MRB_VFS_OTHER;
  }
  return 0;
}

mrb_vfs_hal_file*
mrb_hal_vfs_open(mrb_state *mrb, const char *path)
{
  int flags = O_RDONLY;
  int fd;
  mrb_vfs_hal_file *file;

#ifdef O_BINARY
  flags |= O_BINARY;
#endif
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  do {
    fd = open(path, flags);
  } while (fd == -1 && errno == EINTR);
  if (fd == -1) {
    return NULL;
  }

  file = (mrb_vfs_hal_file*)mrb_malloc(mrb, sizeof(mrb_vfs_hal_file));
  file->fd = fd;
  return file;
}

mrb_int
mrb_hal_vfs_read(mrb_state *mrb, mrb_vfs_hal_file *file, void *buf, size_t count)
{
  ssize_t n;
  (void)mrb;

  if (count > (size_t)MRB_INT_MAX) {
    count = (size_t)MRB_INT_MAX;
  }
  do {
    n = read(file->fd, buf, count);
  } while (n == -1 && errno == EINTR);
  return (mrb_int)n;
}

int
mrb_hal_vfs_close(mrb_state *mrb, mrb_vfs_hal_file *file)
{
  int result = close(file->fd);
  mrb_free(mrb, file);
  return result;
}

/*
 * HAL Initialization/Finalization
 */

void
mrb_hal_vfs_init(mrb_state *mrb)
{
  (void)mrb;
  /* No initialization needed for POSIX */
}

void
mrb_hal_vfs_final(mrb_state *mrb)
{
  (void)mrb;
  /* No cleanup needed for POSIX */
}
