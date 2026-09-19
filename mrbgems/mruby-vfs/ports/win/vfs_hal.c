/*
** vfs_hal.c - Windows HAL implementation for mruby-vfs
**
** See Copyright Notice in mruby.h
**
** Windows implementation for reading the host's files through the wide
** character CRT calls, so that a UTF-8 path reaches the file it names
** whatever the console code page is.
*/

#include <mruby.h>
#include <mruby/internal.h>
#include "vfs_hal.h"

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* On Windows, mrb_vfs_hal_file wraps a CRT file descriptor */
struct mrb_vfs_hal_file {
  int fd;
};

static wchar_t*
utf8_to_utf16(mrb_state *mrb, const char *utf8)
{
  wchar_t *utf16;

  /* MB_ERR_INVALID_CHARS: a path is not a place to accept the replacement
     character a byte the code page cannot read would otherwise become. The
     caller frees with mrb_free(), which is why this takes the mrb_malloc()
     variant. */
  if (mrb_mbs_to_wcs_m(mrb, utf8, -1, &utf16, CP_UTF8, MB_ERR_INVALID_CHARS) < 0) {
    errno = EINVAL;
    return NULL;
  }
  return utf16;
}

int
mrb_hal_vfs_stat(mrb_state *mrb, const char *path, enum mrb_vfs_kind *kind, mrb_int *size)
{
  struct _stat64 sb;
  wchar_t *utf16 = utf8_to_utf16(mrb, path);
  int result;
  int saved_errno;

  *size = -1;
  if (utf16 == NULL) return -1;
  result = _wstat64(utf16, &sb);
  saved_errno = errno;
  mrb_free(mrb, utf16);
  if (result == -1) {
    if (saved_errno == ENOENT || saved_errno == ENOTDIR) {
      *kind = MRB_VFS_NONE;
      return 0;
    }
    errno = saved_errno;
    return -1;
  }
  if ((sb.st_mode & _S_IFMT) == _S_IFREG) {
    *kind = MRB_VFS_FILE;
    if (sb.st_size >= 0 && (uint64_t)sb.st_size <= (uint64_t)MRB_INT_MAX) {
      *size = (mrb_int)sb.st_size;
    }
  }
  else if ((sb.st_mode & _S_IFMT) == _S_IFDIR) {
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
  wchar_t *utf16 = utf8_to_utf16(mrb, path);
  mrb_vfs_hal_file *file;
  int fd;
  int saved_errno;

  if (utf16 == NULL) return NULL;
  fd = _wopen(utf16, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  saved_errno = errno;
  mrb_free(mrb, utf16);
  if (fd == -1) {
    errno = saved_errno;
    return NULL;
  }

  file = (mrb_vfs_hal_file*)mrb_malloc(mrb, sizeof(mrb_vfs_hal_file));
  file->fd = fd;
  return file;
}

mrb_int
mrb_hal_vfs_read(mrb_state *mrb, mrb_vfs_hal_file *file, void *buf, size_t count)
{
  (void)mrb;

  /* _read() takes an unsigned int, and INT_MAX is what it can report back */
  if (count > (size_t)INT_MAX) {
    count = (size_t)INT_MAX;
  }
  return (mrb_int)_read(file->fd, buf, (unsigned int)count);
}

int
mrb_hal_vfs_close(mrb_state *mrb, mrb_vfs_hal_file *file)
{
  int result = _close(file->fd);
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
  /* No initialization needed for Windows */
}

void
mrb_hal_vfs_final(mrb_state *mrb)
{
  (void)mrb;
  /* No cleanup needed for Windows */
}
