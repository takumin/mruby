/*
** io.c - IO class
**
** The stream object itself: the descriptors it holds, the mode it was
** opened with, and every method that opens one, gives one up, or asks it
** something other than for bytes. What moves bytes is in io_read.c and
** io_write.c; IO.select and IO.popen have a file each. io_internal.h says
** what the five of them share.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/internal.h>
#include <mruby/string.h>
#include <mruby/io.h>
#include "io_hal.h"
#include "io_internal.h"

#include <sys/types.h>
#include <sys/stat.h>

/* A descriptor and the bytes moved through it go through the HAL, so what a
   platform is asked for here is the open flags it names. */
#if defined(_WIN32)
  #include <winsock.h>    /* getsockopt(), to tell a socket from a descriptor */
  #include <stdlib.h>

  #ifndef O_TMPFILE
    #define O_TMPFILE O_TEMPORARY
  #endif

#else
  #include <unistd.h>
#endif

#include <fcntl.h>

#include <errno.h>
#include <string.h>

static void io_free(mrb_state *mrb, void *ptr);
struct mrb_data_type mrb_io_type = { "IO", io_free };

static void fptr_finalize(mrb_state *mrb, struct mrb_io *fptr, int quiet);

struct mrb_io*
mrb_io_get_fptr(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = (struct mrb_io*)mrb_data_get_ptr(mrb, io, &mrb_io_type);
  if (fptr == NULL) {
    mrb_raise(mrb, E_IO_ERROR, "uninitialized stream");
  }
  return fptr;
}

struct mrb_io*
mrb_io_get_open_fptr(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_fptr(mrb, io);
  if (fptr->fd < 0) {
    mrb_raise(mrb, E_IO_ERROR, "closed stream");
  }
  return fptr;
}

struct mrb_io*
mrb_io_get_read_fptr(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  if (!fptr->readable) {
    mrb_raise(mrb, E_IO_ERROR, "not opened for reading");
  }
  return fptr;
}

struct mrb_io*
mrb_io_get_write_fptr(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  if (!fptr->writable) {
    mrb_raise(mrb, E_IO_ERROR, "not opened for writing");
  }
  return fptr;
}

int
mrb_io_get_write_fd(struct mrb_io *fptr)
{
  if (fptr->fd2 == -1) {
    return fptr->fd;
  }
  else {
    return fptr->fd2;
  }
}

static mrb_noreturn void
mode_error(mrb_state *mrb, const char *mode)
{
  mrb_raisef(mrb, E_ARGUMENT_ERROR, "illegal access mode %s", mode);
}

static int
io_modestr_to_flags(mrb_state *mrb, const char *mode)
{
  int flags;
  const char *m = mode;

  switch (*m++) {
    case 'r':
      flags = O_RDONLY;
      break;
    case 'w':
      flags = O_WRONLY | O_CREAT | O_TRUNC;
      break;
    case 'a':
      flags = O_WRONLY | O_CREAT | O_APPEND;
      break;
    default:
      mode_error(mrb, mode);
  }

  while (*m) {
    switch (*m++) {
      case 'b':
#ifdef O_BINARY
        flags |= O_BINARY;
#endif
        break;
      case 'x':
        if (mode[0] != 'w') mode_error(mrb, mode);
        flags |= O_EXCL;
        break;
      case '+':
        flags = (flags & ~MRB_IO_ACCESS_MODE_FLAGS) | O_RDWR;
        break;
      case ':':
        /* XXX: PASSTHROUGH*/
      default:
        mode_error(mrb, mode);
    }
  }

  return flags;
}

int
mrb_io_mode_to_flags(mrb_state *mrb, mrb_value mode)
{
  if (mrb_nil_p(mode)) {
    return O_RDONLY;
  }
  else if (mrb_string_p(mode)) {
    return io_modestr_to_flags(mrb, RSTRING_CSTR(mrb, mode));
  }
  else {
    int flags = 0;
    mrb_int flags0 = mrb_as_int(mrb, mode);

    switch (flags0 & MRB_O_ACCMODE) {
      case MRB_O_RDONLY:
        flags |= O_RDONLY;
        break;
      case MRB_O_WRONLY:
        flags |= O_WRONLY;
        break;
      case MRB_O_RDWR:
        flags |= O_RDWR;
        break;
      default:
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "illegal access mode %v", mode);
    }

    if (flags0 & MRB_O_APPEND)        flags |= O_APPEND;
    if (flags0 & MRB_O_CREAT)         flags |= O_CREAT;
    if (flags0 & MRB_O_EXCL)          flags |= O_EXCL;
    if (flags0 & MRB_O_TRUNC)         flags |= O_TRUNC;
#ifdef O_NONBLOCK
    if (flags0 & MRB_O_NONBLOCK)      flags |= O_NONBLOCK;
#endif
#ifdef O_NOCTTY
    if (flags0 & MRB_O_NOCTTY)        flags |= O_NOCTTY;
#endif
#ifdef O_BINARY
    if (flags0 & MRB_O_BINARY)        flags |= O_BINARY;
#endif
#ifdef O_SHARE_DELETE
    if (flags0 & MRB_O_SHARE_DELETE)  flags |= O_SHARE_DELETE;
#endif
#ifdef O_SYNC
    if (flags0 & MRB_O_SYNC)          flags |= O_SYNC;
#endif
#ifdef O_DSYNC
    if (flags0 & MRB_O_DSYNC)         flags |= O_DSYNC;
#endif
#ifdef O_RSYNC
    if (flags0 & MRB_O_RSYNC)         flags |= O_RSYNC;
#endif
#ifdef O_NOFOLLOW
    if (flags0 & MRB_O_NOFOLLOW)      flags |= O_NOFOLLOW;
#endif
#ifdef O_NOATIME
    if (flags0 & MRB_O_NOATIME)       flags |= O_NOATIME;
#endif
#ifdef O_DIRECT
    if (flags0 & MRB_O_DIRECT)        flags |= O_DIRECT;
#endif
#ifdef O_TMPFILE
    if (flags0 & MRB_O_TMPFILE)       flags |= O_TMPFILE;
#endif

    return flags;
  }
}


static void
io_fd_cloexec(mrb_state *mrb, int fd)
{
#if defined(F_GETFD) && defined(F_SETFD) && defined(FD_CLOEXEC)
  int flags = mrb_hal_io_fcntl(mrb, fd, F_GETFD, 0);
  int flags2;

  if (flags < 0) {
    mrb_sys_fail(mrb, "cloexec GETFD");
  }
  if (fd <= 2) {
    flags2 = flags & ~FD_CLOEXEC; /* Clear CLOEXEC for standard file descriptors: 0, 1, 2. */
  }
  else {
    flags2 = flags | FD_CLOEXEC; /* Set CLOEXEC for non-standard file descriptors: 3, 4, 5, ... */
  }
  if (flags != flags2) {
    if (mrb_hal_io_fcntl(mrb, fd, F_SETFD, flags2) < 0) {
      mrb_sys_fail(mrb, "cloexec SETFD");
    }
  }
#endif
}

static void
io_free(mrb_state *mrb, void *ptr)
{
  struct mrb_io *io = (struct mrb_io*)ptr;
  if (io != NULL) {
    fptr_finalize(mrb, io, TRUE);
    mrb_free(mrb, io);
  }
}

void
mrb_io_init_buf(mrb_state *mrb, struct mrb_io *fptr)
{
  if (fptr->readable) {
    fptr->buf = (struct mrb_io_buf*)mrb_malloc(mrb, sizeof(struct mrb_io_buf));
    fptr->buf->start = 0;
    fptr->buf->len = 0;
  }
}

struct mrb_io *
mrb_io_alloc(mrb_state *mrb)
{
  struct mrb_io *fptr = (struct mrb_io*)mrb_malloc(mrb, sizeof(struct mrb_io));
  fptr->fd = -1;
  fptr->fd2 = -1;
  fptr->pid = 0;
  fptr->buf = 0;
  fptr->readable = 0;
  fptr->writable = 0;
  fptr->sync = 0;
  fptr->eof = 0;
  fptr->is_socket = 0;
  fptr->close_fd = 1;
  fptr->close_fd2 = 1;
  return fptr;
}

static void
fptr_finalize(mrb_state *mrb, struct mrb_io *fptr, int quiet)
{
  int saved_errno = 0;
  int limit = quiet ? 3 : 0;

  if (fptr == NULL) {
    return;
  }

  if (fptr->fd >= limit) {
#ifdef _WIN32
    if (fptr->is_socket) {
      if (fptr->close_fd && closesocket(fptr->fd) != 0) {
        saved_errno = WSAGetLastError();
      }
      fptr->fd = -1;
    }
#endif
    if (fptr->fd != -1 && fptr->close_fd) {
      if (mrb_hal_io_close(mrb, fptr->fd) == -1) {
        saved_errno = errno;
      }
    }
    fptr->fd = -1;
  }

  if (fptr->fd2 >= limit) {
    if (fptr->close_fd2 && mrb_hal_io_close(mrb, fptr->fd2) == -1) {
      if (saved_errno == 0) {
        saved_errno = errno;
      }
    }
    fptr->fd2 = -1;
  }

  mrb_io_reap_child(mrb, fptr, quiet);

  if (fptr->buf) {
    mrb_free(mrb, fptr->buf);
    fptr->buf = NULL;
  }

  if (!quiet && saved_errno != 0) {
    errno = saved_errno;
    mrb_sys_fail(mrb, "fptr_finalize failed");
  }
}

static mrb_noreturn void
badfd_error(mrb_state *mrb)
{
  mrb_sys_fail(mrb, "bad file descriptor");
}

#if defined(_WIN32) && defined(_MSC_VER)
/* Stands in for the CRT's default invalid parameter handler, which ends the
   process. Doing nothing lets the call that tripped it return an error. */
static void
ignore_invalid_parameter(const wchar_t *expression, const wchar_t *function,
                         const wchar_t *file, unsigned int line, uintptr_t reserved)
{
  (void)expression; (void)function; (void)file; (void)line; (void)reserved;
}
#endif

static void
check_file_descriptor(mrb_state *mrb, mrb_int fd)
{
  struct stat sb;
  int fdi = (int)fd;

#if MRB_INT_MIN < INT_MIN || MRB_INT_MAX > INT_MAX
  if (fdi != fd) {
    errno = EBADF;
    badfd_error(mrb);
  }
#endif

#ifdef _WIN32
  /* A Winsock handle is not a CRT file descriptor, and fstat below cannot
     vouch for one, so ask Winsock before the CRT gets a say. */
  {
    DWORD err;
    int len = sizeof(err);

    if (getsockopt(fdi, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0) {
      return;
    }
  }

  {
    int ok;
#ifdef _MSC_VER
    /* Asking the CRT about a descriptor it does not have invokes the invalid
       parameter handler, and the default one ends the process rather than
       returning: a bare IO.new(400) would take the program with it. A handler
       that does nothing leaves fstat to report the failure the way it reports
       any other. Thread-local, so an embedding program's own handler stands. */
    _invalid_parameter_handler prev =
      _set_thread_local_invalid_parameter_handler(ignore_invalid_parameter);
#endif
    ok = (fdi >= 0 && fstat(fdi, &sb) == 0);
#ifdef _MSC_VER
    _set_thread_local_invalid_parameter_handler(prev);
#endif
    if (ok) return;
    /* Whatever the CRT set errno to on the way out, what the caller asked is
       whether the descriptor is one, and it is not. */
    errno = EBADF;
    badfd_error(mrb);
  }
#endif /* _WIN32 */

  if (fstat(fdi, &sb) == 0) return;
  if (errno == EBADF) badfd_error(mrb);
}

/*
 * call-seq:
 *   IO.new(fd, mode="r") -> io
 *
 * Returns a new `IO` object for the given integer file descriptor `fd` and
 * `mode` string.
 *
 *   f = IO.new(1, "w")  # STDOUT
 *   f.puts "hello"
 */
static mrb_value
io_init(mrb_state *mrb, mrb_value io)
{
  mrb_int fd;
  mrb_value mode = mrb_nil_value();
  mrb_value opt = mrb_nil_value();

  if (mrb_block_given_p(mrb)) {
    mrb_warn(mrb, "File.new() does not take block; use File.open() instead");
  }
  mrb_get_args(mrb, "i|oH", &fd, &mode, &opt);
  switch (fd) {
    case 0: /* STDIN_FILENO */
    case 1: /* STDOUT_FILENO */
    case 2: /* STDERR_FILENO */
      break;
    default:
      check_file_descriptor(mrb, fd);
      break;
  }
  int flags = mrb_io_mode_to_flags(mrb, mode);

  struct mrb_io *fptr = (struct mrb_io*)DATA_PTR(io);
  if (fptr != NULL) {
    fptr_finalize(mrb, fptr, TRUE);
    mrb_free(mrb, fptr);
  }
  fptr = mrb_io_alloc(mrb);

  DATA_TYPE(io) = &mrb_io_type;
  DATA_PTR(io) = fptr;

  fptr->fd = (int)fd;
  fptr->readable = MRB_IO_READABLE_P(flags);
  fptr->writable = MRB_IO_WRITABLE_P(flags);
  mrb_io_init_buf(mrb, fptr);
  return io;
}

static mrb_value
io_isatty(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  if (mrb_hal_io_isatty(mrb, fptr->fd) == 0)
    return mrb_false_value();
  return mrb_true_value();
}

static mrb_value
io_s_for_fd(mrb_state *mrb, mrb_value klass)
{
  struct RClass *c = mrb_class_ptr(klass);
  enum mrb_vtype ttype = MRB_INSTANCE_TT(c);

  /* copied from mrb_instance_alloc() */
  if (ttype == 0) ttype = MRB_TT_OBJECT;

  mrb_value obj = mrb_obj_value((struct RObject*)mrb_obj_alloc(mrb, ttype, c));
  return io_init(mrb, obj);
}

static mrb_value
io_s_sysclose(mrb_state *mrb, mrb_value klass)
{
  mrb_int fd;
  mrb->c->ci->mid = 0;
  mrb_get_args(mrb, "i", &fd);
  if (mrb_hal_io_close(mrb, (int)fd) == -1) {
    mrb_sys_fail(mrb, "close");
  }
  return mrb_fixnum_value(0);
}

static int
io_cloexec_open(mrb_state *mrb, const char *pathname, int flags, mrb_int mode)
{
  int retry = FALSE;
  char *fname = mrb_locale_from_utf8(pathname, -1);
  int fd;

#ifdef O_CLOEXEC
  /* O_CLOEXEC is available since Linux 2.6.23.  Linux 2.6.18 silently ignore it. */
  flags |= O_CLOEXEC;
#elif defined O_NOINHERIT
  flags |= O_NOINHERIT;
#endif
reopen:
  fd = mrb_hal_io_open(mrb, fname, flags, mode);
  if (fd == -1) {
    if (!retry) {
      switch (errno) {
      case ENFILE:
      case EMFILE:
        mrb_garbage_collect(mrb);
        retry = TRUE;
        goto reopen;
      }
    }
    mrb_sys_fail(mrb, RSTRING_CSTR(mrb, mrb_format(mrb, "open %s", pathname)));
  }
  mrb_locale_free(fname);

  if (fd <= 2) {
    io_fd_cloexec(mrb, fd);
  }
  return fd;
}

static mrb_value
io_s_sysopen(mrb_state *mrb, mrb_value klass)
{
  mrb_value path = mrb_nil_value();
  mrb_value mode = mrb_nil_value();
  mrb_int perm = -1;

  mrb_get_args(mrb, "S|oi", &path, &mode, &perm);
  if (perm < 0) {
    perm = 0666;
  }

  const char *pat = RSTRING_CSTR(mrb, path);
  int flags = mrb_io_mode_to_flags(mrb, mode);
  mrb_int fd = io_cloexec_open(mrb, pat, flags, perm);
  return mrb_fixnum_value(fd);
}

static mrb_value
io_s_pipe(mrb_state *mrb, mrb_value klass)
{
  int pipes[2];

  if (mrb_hal_io_pipe(mrb, pipes) == -1) {
    mrb_sys_fail(mrb, "pipe");
  }

  mrb_value r = mrb_obj_value(mrb_data_object_alloc(mrb, mrb_class_ptr(klass), NULL, &mrb_io_type));
  struct mrb_io *fptr_r = mrb_io_alloc(mrb);
  fptr_r->fd = pipes[0];
  fptr_r->readable = 1;
  DATA_TYPE(r) = &mrb_io_type;
  DATA_PTR(r)  = fptr_r;
  mrb_io_init_buf(mrb, fptr_r);

  mrb_value w = mrb_obj_value(mrb_data_object_alloc(mrb, mrb_class_ptr(klass), NULL, &mrb_io_type));
  struct mrb_io *fptr_w = mrb_io_alloc(mrb);
  fptr_w->fd = pipes[1];
  fptr_w->writable = 1;
  fptr_w->sync = 1;
  DATA_TYPE(w) = &mrb_io_type;
  DATA_PTR(w)  = fptr_w;

  return mrb_assoc_new(mrb, r, w);
}

static int
symdup(mrb_state *mrb, int fd, mrb_bool *failed)
{
  int new_fd;

  *failed = TRUE;
  if (fd < 0)
    return fd;

  new_fd = mrb_hal_io_dup(mrb, fd);
  if (new_fd >= 0) *failed = FALSE;  /* 0 is a descriptor, not a failure */
  return new_fd;
}

static mrb_value
io_init_copy(mrb_state *mrb, mrb_value copy)
{
  mrb_value orig = mrb_get_arg1(mrb);
  struct mrb_io *fptr_copy;
  struct mrb_io *fptr_orig;
  mrb_bool failed = TRUE;

  fptr_orig = mrb_io_get_open_fptr(mrb, orig);
  fptr_copy = (struct mrb_io*)DATA_PTR(copy);
  if (fptr_orig == fptr_copy) return copy;
  if (fptr_copy != NULL) {
    fptr_finalize(mrb, fptr_copy, FALSE);
    mrb_free(mrb, fptr_copy);
  }
  fptr_copy = (struct mrb_io*)mrb_io_alloc(mrb);
  fptr_copy->pid = fptr_orig->pid;
  fptr_copy->readable = fptr_orig->readable;
  fptr_copy->writable = fptr_orig->writable;
  fptr_copy->sync = fptr_orig->sync;
  fptr_copy->is_socket = fptr_orig->is_socket;

  mrb_io_init_buf(mrb, fptr_copy);

  DATA_TYPE(copy) = &mrb_io_type;
  DATA_PTR(copy) = fptr_copy;

  fptr_copy->fd = symdup(mrb, fptr_orig->fd, &failed);
  if (failed) {
    mrb_sys_fail(mrb, 0);
  }
  io_fd_cloexec(mrb, fptr_copy->fd);

  if (fptr_orig->fd2 != -1) {
    fptr_copy->fd2 = symdup(mrb, fptr_orig->fd2, &failed);
    if (failed) {
      /* `copy` is already reachable from the GC, so it outlives this error and
         is finalized later. Give up the descriptor rather than leaving it in
         `fd`, or that finalizer closes whatever has since reused the number. */
      int err = errno;
      mrb_hal_io_close(mrb, fptr_copy->fd);
      fptr_copy->fd = -1;
      errno = err;
      mrb_sys_fail(mrb, 0);
    }
    io_fd_cloexec(mrb, fptr_copy->fd2);
  }

  return copy;
}

/*
 * call-seq:
 *   ios.close -> nil
 *
 * Closes the stream. A stream that is already closed is left as it is.
 *
 *   f = File.new("testfile")
 *   f.close         #=> nil
 *   f.close         #=> nil
 */
static mrb_value
io_close(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_fptr(mrb, io);

  /* Closing what is closed asks for nothing, and the stream already answers
     #closed? with true, so there is nothing for a raise to tell. */
  if (fptr->fd < 0) {
    return mrb_nil_value();
  }
  fptr_finalize(mrb, fptr, FALSE);
  return mrb_nil_value();
}

/*
 * call-seq:
 *   ios.close_write -> nil
 *
 * Closes the write end of a duplex I/O stream (i.e., a pipe).
 *
 * A stream that is not duplex has no write end of its own. If it cannot be
 * read from, or it was opened to a child process, its only end is the one
 * being closed and the whole stream is closed; otherwise an `IOError` is
 * raised. A stream that is already closed is left as it is.
 *
 *   r, w = IO.pipe
 *   w.close_write
 *   r.read #=> ""
 */
static mrb_value
io_close_write(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_fptr(mrb, io);

  /* A closed stream has no write end left to give up, duplex or not, so the
     answer is the one #close gives and nothing below it applies. */
  if (fptr->fd < 0) {
    return mrb_nil_value();
  }

  int fd2 = fptr->fd2;
  if (fd2 == -1) {
    /* No second descriptor, so writing goes through fd, which is also what
       reading goes through. There is a write end to give up only where
       nothing reads from the stream for the caller's own sake, and then it is
       the whole stream: what a stream opened to a child process reads is the
       child, which ends with the pipe. */
    if (fptr->readable && fptr->pid == 0) {
      mrb_raise(mrb, E_IO_ERROR, "closing non-duplex IO for writing");
    }
    fptr_finalize(mrb, fptr, FALSE);
    return mrb_nil_value();
  }
  /* The write end is gone whatever close(2) answers, and leaving its number
     behind would have #close try to close it a second time. */
  fptr->fd2 = -1;
  /* A stream that has an fd2 writes to it and reads from fd, so closing the
     write end leaves nowhere to write to: the flag every writer is gated on
     has to fall with the descriptor. */
  fptr->writable = 0;
  if (mrb_hal_io_close(mrb, fd2) == -1) {
    mrb_sys_fail(mrb, "close");
  }
  return mrb_nil_value();
}

/*
 * call-seq:
 *   ios.closed? -> true or false
 *
 * Returns `true` if the stream is closed, `false` otherwise.
 *
 *   f = File.new("testfile")
 *   f.close         #=> nil
 *   f.closed?       #=> true
 */
static mrb_value
io_closed(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = (struct mrb_io*)mrb_data_get_ptr(mrb, io, &mrb_io_type);
  if (fptr == NULL || fptr->fd >= 0) {
    return mrb_false_value();
  }

  return mrb_true_value();
}

static mrb_value
io_sysseek(mrb_state *mrb, mrb_value io)
{
  mrb_int offset, whence = -1;

  mrb_get_args(mrb, "i|i", &offset, &whence);
  if (whence < 0) {
    whence = 0;
  }

  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  off_t pos = (off_t)mrb_hal_io_lseek(mrb, fptr->fd, (mrb_int)offset, (int)whence);
  if (pos == -1) {
    mrb_sys_fail(mrb, "sysseek");
  }
  fptr->eof = 0;
  if (sizeof(off_t) > sizeof(mrb_int) && pos > (off_t)MRB_INT_MAX) {
    mrb_raise(mrb, E_IO_ERROR, "sysseek reached too far for mrb_int");
  }
  return mrb_int_value(mrb, (mrb_int)pos);
}

static mrb_value
io_seek(mrb_state *mrb, mrb_value io)
{
  mrb_value pos = io_sysseek(mrb, io);
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  if (fptr->buf) {
    mrb_io_buf_reset(fptr->buf);
  }
  return pos;
}

static mrb_value
io_pos(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  off_t pos = (off_t)mrb_hal_io_lseek(mrb, fptr->fd, 0, MRB_IO_SEEK_CUR);
  if (pos == -1) mrb_sys_fail(mrb, 0);

  if (fptr->buf) {
    return mrb_int_value(mrb, pos - fptr->buf->len);
  }
  else {
    return mrb_int_value(mrb, pos);
  }
}

/*
 * call-seq:
 *   ios.pid -> integer or nil
 *
 * Returns the process ID of a child process on a pipe, or `nil` if the
 * stream is not a pipe.
 *
 *   r, w = IO.pipe
 *   fork do
 *     r.close
 *     w.write "hello"
 *     w.close
 *   end
 *   w.close
 *   p r.pid   #=> 2056
 *   r.read    #=> "hello"
 *   r.close
 */
static mrb_value
io_pid(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);

  if (fptr->pid > 0) {
    return mrb_fixnum_value(fptr->pid);
  }

  return mrb_nil_value();
}

int
mrb_io_fileno(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  return fptr->fd;
}

/*
 * call-seq:
 *   ios.fileno -> integer
 *
 * Returns the integer file descriptor number for the `IO` object.
 *
 *   $stdin.fileno    #=> 0
 *   $stdout.fileno   #=> 1
 */
static mrb_value
io_fileno(mrb_state *mrb, mrb_value io)
{
  int fd = mrb_io_fileno(mrb, io);
  return mrb_fixnum_value(fd);
}

#if defined(F_GETFD) && defined(F_SETFD) && defined(FD_CLOEXEC)
/*
 * call-seq:
 *   ios.close_on_exec? -> true or false
 *
 * Returns `true` if the `FD_CLOEXEC` flag is set for the `IO` object, `false`
 * otherwise.
 *
 *   f = IO.new(1, "w")
 *   f.close_on_exec?   #=> true
 */
static mrb_value
io_close_on_exec_p(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  int ret;

  if (fptr->fd2 >= 0) {
    if ((ret = mrb_hal_io_fcntl(mrb, fptr->fd2, F_GETFD, 0)) == -1) mrb_sys_fail(mrb, "F_GETFD failed");
    if (!(ret & FD_CLOEXEC)) return mrb_false_value();
  }

  if ((ret = mrb_hal_io_fcntl(mrb, fptr->fd, F_GETFD, 0)) == -1) mrb_sys_fail(mrb, "F_GETFD failed");
  if (!(ret & FD_CLOEXEC)) return mrb_false_value();
  return mrb_true_value();
}
#else
# define io_close_on_exec_p mrb_notimplement_m
#endif

#if defined(F_GETFD) && defined(F_SETFD) && defined(FD_CLOEXEC)
/*
 * call-seq:
 *   ios.close_on_exec = bool -> bool
 *
 * Sets the `FD_CLOEXEC` flag on the `IO` object.
 *
 *   f = IO.new(1, "w")
 *   f.close_on_exec = false
 *   f.close_on_exec?   #=> false
 */
static mrb_value
io_set_close_on_exec(mrb_state *mrb, mrb_value io)
{

  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  mrb_bool b;

  mrb_get_args(mrb, "b", &b);

  int flag = b ? FD_CLOEXEC : 0;
  int ret;

  if (fptr->fd2 >= 0) {
    if ((ret = mrb_hal_io_fcntl(mrb, fptr->fd2, F_GETFD, 0)) == -1) mrb_sys_fail(mrb, "F_GETFD failed");
    if ((ret & FD_CLOEXEC) != flag) {
      ret = (ret & ~FD_CLOEXEC) | flag;
      ret = mrb_hal_io_fcntl(mrb, fptr->fd2, F_SETFD, ret);

      if (ret == -1) mrb_sys_fail(mrb, "F_SETFD failed");
    }
  }

  if ((ret = mrb_hal_io_fcntl(mrb, fptr->fd, F_GETFD, 0)) == -1) mrb_sys_fail(mrb, "F_GETFD failed");
  if ((ret & FD_CLOEXEC) != flag) {
    ret = (ret & ~FD_CLOEXEC) | flag;
    ret = mrb_hal_io_fcntl(mrb, fptr->fd, F_SETFD, ret);
    if (ret == -1) mrb_sys_fail(mrb, "F_SETFD failed");
  }

  return mrb_bool_value(b);
}
#else
# define io_set_close_on_exec mrb_notimplement_m
#endif

/*
 * call-seq:
 *   ios.sync = bool -> bool
 *
 * Sets the sync mode for the `IO` object.
 *
 * If `true`, all output is immediately flushed to the underlying operating
 * system and is not buffered internally.
 *
 *   f = File.new("testfile", "w")
 *   f.sync = true
 */
static mrb_value
io_set_sync(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  mrb_bool b;

  mrb_get_args(mrb, "b", &b);
  fptr->sync = b;
  return mrb_bool_value(b);
}

/*
 * call-seq:
 *   ios.sync -> true or false
 *
 * Returns the sync mode for the `IO` object.
 *
 *   f = File.new("testfile", "w")
 *   f.sync   #=> false
 */
static mrb_value
io_sync(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  return mrb_bool_value(fptr->sync);
}

/*
 * call-seq:
 *   ios.flush -> ios
 *
 * Flushes any buffered data within the `IO` object to the underlying
 * operating system.
 *
 *   $stdout.print "no newline"
 *   $stdout.flush
 */
static mrb_value
io_flush(mrb_state *mrb, mrb_value io)
{
  mrb_io_get_open_fptr(mrb, io);
  return io;
}

/*
 * call-seq:
 *   ios.autoclose = bool -> bool
 *
 * Sets the autoclose flag.
 *
 * If the autoclose flag is set, the underlying file descriptor(s) of +ios+
 * will be closed when +ios+ is closed (explicitly via +#close+, or implicitly
 * when the +IO+ object is garbage-collected). When unset, the file
 * descriptor(s) are left open.
 *
 *   f = File.open("testfile")
 *   IO.for_fd(f.fileno).autoclose = false
 */
static mrb_value
io_set_autoclose(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  mrb_bool b;

  mrb_get_args(mrb, "b", &b);
  fptr->close_fd = b;
  fptr->close_fd2 = b;
  return mrb_bool_value(b);
}

/*
 * call-seq:
 *   ios.autoclose? -> true or false
 *
 * Returns +true+ if the underlying file descriptor of +ios+ will be closed
 * when +ios+ is closed, otherwise +false+.
 *
 *   f = File.open("testfile")
 *   f.autoclose?         #=> true
 *   f.autoclose = false
 *   f.autoclose?         #=> false
 */
static mrb_value
io_autoclose_p(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_open_fptr(mrb, io);
  return mrb_bool_value(fptr->close_fd);
}

/* ---------------------------*/
static const mrb_mt_entry io_rom_entries[] = {
  MRB_MT_ENTRY(io_init,              MRB_SYM(initialize),      MRB_ARGS_ARG(1,2)),
  MRB_MT_ENTRY(io_init_copy,         MRB_SYM(initialize_copy), MRB_ARGS_REQ(1) | MRB_MT_PRIVATE),
  MRB_MT_ENTRY(io_isatty,            MRB_SYM(isatty),          MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_sync,              MRB_SYM(sync),            MRB_ARGS_NONE()),  /* 15.2.20.5.18 */
  MRB_MT_ENTRY(io_set_sync,          MRB_SYM_E(sync),          MRB_ARGS_REQ(1)),  /* 15.2.20.5.19 */
  MRB_MT_ENTRY(io_sysseek,           MRB_SYM(sysseek),         MRB_ARGS_ARG(1,1)),
  MRB_MT_ENTRY(io_seek,              MRB_SYM(seek),            MRB_ARGS_ARG(1,1)),
  MRB_MT_ENTRY(io_close,             MRB_SYM(close),           MRB_ARGS_NONE()),  /* 15.2.20.5.1 */
  MRB_MT_ENTRY(io_close_write,       MRB_SYM(close_write),     MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_set_close_on_exec, MRB_SYM_E(close_on_exec), MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(io_close_on_exec_p,   MRB_SYM_Q(close_on_exec), MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_closed,            MRB_SYM_Q(closed),        MRB_ARGS_NONE()),  /* 15.2.20.5.2 */
  MRB_MT_ENTRY(io_flush,             MRB_SYM(flush),           MRB_ARGS_NONE()),  /* 15.2.20.5.7 */
  MRB_MT_ENTRY(io_pos,               MRB_SYM(pos),             MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_pid,               MRB_SYM(pid),             MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_fileno,            MRB_SYM(fileno),          MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_set_autoclose,     MRB_SYM_E(autoclose),     MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(io_autoclose_p,       MRB_SYM_Q(autoclose),     MRB_ARGS_NONE()),
};

void
mrb_init_io(mrb_state *mrb)
{
  struct RClass *io = mrb_define_class_id(mrb, MRB_SYM(IO), mrb->object_class);
  MRB_SET_INSTANCE_TT(io, MRB_TT_CDATA);

  mrb_include_module(mrb, io, mrb_module_get_id(mrb, MRB_SYM(Enumerable))); /* 15.2.20.3 */
  mrb_define_class_method_id(mrb, io, MRB_SYM(_sysclose), io_s_sysclose, MRB_ARGS_REQ(1));
  mrb_define_class_method_id(mrb, io, MRB_SYM(for_fd),  io_s_for_fd,   MRB_ARGS_ARG(1,2));
  mrb_define_class_method_id(mrb, io, MRB_SYM(sysopen), io_s_sysopen, MRB_ARGS_ARG(1,2));
  mrb_define_class_method_id(mrb, io, MRB_SYM(_pipe), io_s_pipe, MRB_ARGS_NONE());

  MRB_MT_INIT_ROM(mrb, io, io_rom_entries);

  /* The rest of the class is defined by the file that implements it. */
  mrb_io_read_init(mrb, io);
  mrb_io_write_init(mrb, io);
  mrb_io_select_init(mrb, io);
  mrb_io_popen_init(mrb, io);

  /* Use the HAL's platform-independent whence values; mrb_hal_io_lseek()
     maps them back to the platform SEEK_* (these coincide on POSIX). */
  mrb_define_const_id(mrb, io, MRB_SYM(SEEK_SET), mrb_fixnum_value(MRB_IO_SEEK_SET));
  mrb_define_const_id(mrb, io, MRB_SYM(SEEK_CUR), mrb_fixnum_value(MRB_IO_SEEK_CUR));
  mrb_define_const_id(mrb, io, MRB_SYM(SEEK_END), mrb_fixnum_value(MRB_IO_SEEK_END));
}
