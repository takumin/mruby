/*
** io_write.c - writing to an IO
**
** Nothing here is buffered: every method writes what it was given straight
** to the descriptor, looping only where write(2) took less than all of it.
** What the read buffer holds is another matter: a stream read from and then
** written to has a descriptor standing further along than the reader got to,
** and io_prepare_write() is what puts it back.
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

#include <errno.h>

static mrb_int
fd_write_buf(mrb_state *mrb, int fd, const char *ptr, mrb_int len)
{
  if (len == 0) return 0;
  mrb_int sum = 0;
  while (sum < len) {
    mrb_int n = mrb_hal_io_write(mrb, fd, ptr + sum, (size_t)(len - sum));
    if (n == -1) {
      if (errno == EINTR) continue;
      mrb_sys_fail(mrb, "syswrite");
    }
    sum += n;
  }
  return len;
}

static mrb_int
fd_write(mrb_state *mrb, int fd, mrb_value str)
{
  str = mrb_obj_as_string(mrb, str);
  return fd_write_buf(mrb, fd, RSTRING_PTR(str), RSTRING_LEN(str));
}

#define FD_WRITE_LIT(mrb, fd, s) fd_write_buf(mrb, fd, "" s "", sizeof(s) - 1)

/* A stream reads ahead: what its buffer holds, the descriptor has already
   moved past. Before a write goes out, put the descriptor back where the
   reader stands and give the buffer up, so what is written lands where the
   reading left off rather than after the bytes nobody has read yet. */
static void
io_prepare_write(mrb_state *mrb, struct mrb_io *fptr)
{
  if (fptr->buf && fptr->buf->len > 0) {
    int fd = mrb_io_get_write_fd(fptr);
    off_t n;

    /* get current position */
    n = (off_t)mrb_hal_io_lseek(mrb, fd, 0, MRB_IO_SEEK_CUR);
    if (n == -1) mrb_sys_fail(mrb, "lseek");
    /* move cursor */
    n = (off_t)mrb_hal_io_lseek(mrb, fd, (mrb_int)(n - fptr->buf->len), MRB_IO_SEEK_SET);
    if (n == -1) mrb_sys_fail(mrb, "lseek(2)");
    mrb_io_buf_reset(fptr->buf);
  }
}


static mrb_value
io_write(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_write_fptr(mrb, io);
  int fd = mrb_io_get_write_fd(fptr);

  io_prepare_write(mrb, fptr);

  mrb_int len = 0;
  if (mrb_get_argc(mrb) == 1) {
    len = fd_write(mrb, fd, mrb_get_arg1(mrb));
  }
  else {
    mrb_value *argv;
    mrb_int argc;

    mrb_get_args(mrb, "*", &argv, &argc);
    while (argc--) {
      len += fd_write(mrb, fd, *argv++);
    }
  }
  return mrb_int_value(mrb, len);
}


static mrb_value
io_syswrite(mrb_state *mrb, mrb_value io)
{
  mrb_value buf;

  mrb_get_args(mrb, "S", &buf);

  int fd = mrb_io_get_write_fd(mrb_io_get_write_fptr(mrb, io));
  mrb_int n = mrb_hal_io_write(mrb, fd, RSTRING_PTR(buf), (size_t)RSTRING_LEN(buf));
  if (n == -1) {
    mrb_sys_fail(mrb, "syswrite");
  }
  return mrb_int_value(mrb, n);
}

/* Helper function to write a string followed by newline if needed */
static void
io_puts_str(mrb_state *mrb, int fd, mrb_value str)
{
  str = mrb_obj_as_string(mrb, str);
  const char *ptr = RSTRING_PTR(str);
  mrb_int len = RSTRING_LEN(str);

  /* Write the original string */
  fd_write(mrb, fd, str);

  /* Add newline if string doesn't end with one */
  if (len == 0 || ptr[len-1] != '\n') {
    FD_WRITE_LIT(mrb, fd, "\n");
  }
}

/* Maximum nesting depth for puts with arrays; guards against cyclic and
   pathologically deep arrays causing C stack overflow. */
#define IO_PUTS_MAX_DEPTH 16

/* Recursive helper for puts with arrays */
static void
io_puts_ary(mrb_state *mrb, int fd, mrb_value ary, int depth)
{
  if (depth >= IO_PUTS_MAX_DEPTH) {
    FD_WRITE_LIT(mrb, fd, "[...]\n");
    return;
  }

  mrb_int len = RARRAY_LEN(ary);

  if (len == 0) {
    /* Empty array - write a single newline */
    FD_WRITE_LIT(mrb, fd, "\n");
    return;
  }

  /* An element's #to_s can replace `ary` with a shorter array, which moves the
     buffer as well as the length, so the next index has to be checked against
     what RARRAY_PTR() now points at. The saved length stays as the upper
     bound: an element that grows the array does not extend the traversal. */
  for (mrb_int i = 0; i < len && i < RARRAY_LEN(ary); i++) {
    mrb_value elem = RARRAY_PTR(ary)[i];
    if (mrb_array_p(elem)) {
      io_puts_ary(mrb, fd, elem, depth + 1);
    }
    else {
      io_puts_str(mrb, fd, elem);
    }
  }
}

static mrb_value
io_puts(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_write_fptr(mrb, io);
  int fd = mrb_io_get_write_fd(fptr);

  /* Prepare IO for writing (handle read buffer adjustment) */
  io_prepare_write(mrb, fptr);

  mrb_value *argv;
  mrb_int argc;
  mrb_get_args(mrb, "*", &argv, &argc);

  if (argc == 0) {
    /* No arguments - just write a newline */
    FD_WRITE_LIT(mrb, fd, "\n");
    return mrb_nil_value();
  }

  /* Process each argument */
  for (mrb_int i = 0; i < argc; i++) {
    mrb_value arg = argv[i];
    if (mrb_array_p(arg)) {
      io_puts_ary(mrb, fd, arg, 0);
    }
    else {
      io_puts_str(mrb, fd, arg);
    }
  }

  return mrb_nil_value();
}

/*
 * call-seq:
 *   ios.print()             -> nil
 *   ios.print(obj, ...)     -> nil
 *
 * Writes the given object(s) to ios. Objects that aren't strings will be
 * converted by calling their to_s method.
 */
static mrb_value
io_print(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_write_fptr(mrb, io);
  int fd = mrb_io_get_write_fd(fptr);

  /* Prepare IO for writing (handle read buffer adjustment) */
  io_prepare_write(mrb, fptr);

  mrb_value *argv;
  mrb_int argc;
  mrb_get_args(mrb, "*", &argv, &argc);

  /* Convert each argument to string and write it */
  for (mrb_int i = 0; i < argc; i++) {
    mrb_value str = mrb_obj_as_string(mrb, argv[i]);
    fd_write(mrb, fd, str);
  }

  return mrb_nil_value();
}

/*
 * call-seq:
 *   ios.putc(obj)  -> obj
 *
 * If obj is Integer, write the byte (mod 256).
 * If obj is String, write the first character.
 * Returns obj.
 */
static mrb_value
io_putc(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_write_fptr(mrb, io);
  int fd = mrb_io_get_write_fd(fptr);
  mrb_value c = mrb_get_arg1(mrb);
  const char *ptr;
  mrb_int write_len;

  io_prepare_write(mrb, fptr);

  if (mrb_integer_p(c)) {
    unsigned char byte = (unsigned char)(mrb_integer(c) & 0xff);
    ssize_t n;
    do {
      n = mrb_hal_io_write(mrb, fd, &byte, 1);
    } while (n == -1 && errno == EINTR);
    if (n == -1) mrb_sys_fail(mrb, "write");
    return c;
  }

  mrb_value str;
  if (mrb_string_p(c)) {
    str = c;
  }
  else {
    str = mrb_obj_as_string(mrb, c);
  }

  ptr = RSTRING_PTR(str);
  mrb_int len = RSTRING_LEN(str);

  if (len == 0) return c;

#ifdef MRB_UTF8_STRING
  write_len = mrb_utf8len(ptr, ptr + len);
#else
  write_len = 1;          /* Non-UTF8: write single byte */
#endif

  /* Write the character bytes */
  while (write_len > 0) {
    ssize_t n = mrb_hal_io_write(mrb, fd, ptr, write_len);
    if (n == -1) {
      if (errno == EINTR) continue;
      mrb_sys_fail(mrb, "write");
    }
    ptr += n;
    write_len -= n;
  }

  return c;
}

/*
 * call-seq:
 *   ios << obj     -> ios
 *
 * String Output - Writes obj to ios. obj will be converted to a string using
 * to_s.
 */
static mrb_value
io_lshift(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_write_fptr(mrb, io);
  int fd = mrb_io_get_write_fd(fptr);

  /* Prepare IO for writing (handle read buffer adjustment) */
  io_prepare_write(mrb, fptr);

  mrb_value str = mrb_get_arg1(mrb);
  str = mrb_obj_as_string(mrb, str);
  fd_write(mrb, fd, str);

  return io;
}

/* Positional writing, where the platform has pwrite(2) to do it with; see
   MRB_USE_IO_PREAD_PWRITE in mruby/io.h for what decides that and why the
   call is made directly rather than through the HAL. */
#ifndef MRB_USE_IO_PREAD_PWRITE
# define io_pwrite mrb_notimplement_m
#else
#include <unistd.h>

/*
 * call-seq:
 *  pwrite(buffer, offset) -> wrote_bytes
 */
static mrb_value
io_pwrite(mrb_state *mrb, mrb_value io)
{
  mrb_value buf, off;

  mrb_get_args(mrb, "So", &buf, &off);

  off_t offset = (off_t)mrb_as_int(mrb, off);
  int fd = mrb_io_get_write_fd(mrb_io_get_write_fptr(mrb, io));
  mrb_int n = (mrb_int)pwrite(fd, RSTRING_PTR(buf), (size_t)RSTRING_LEN(buf), offset);
  if (n == -1) {
    mrb_sys_fail(mrb, "syswrite");
  }
  return mrb_int_value(mrb, n);
}
#endif /* MRB_USE_IO_PREAD_PWRITE */

static const mrb_mt_entry io_write_rom_entries[] = {
  MRB_MT_ENTRY(io_write,    MRB_SYM(write),    MRB_ARGS_ANY()),   /* 15.2.20.5.20 */
  MRB_MT_ENTRY(io_syswrite, MRB_SYM(syswrite), MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(io_puts,     MRB_SYM(puts),     MRB_ARGS_ANY()),
  MRB_MT_ENTRY(io_print,    MRB_SYM(print),    MRB_ARGS_ANY()),
  MRB_MT_ENTRY(io_putc,     MRB_SYM(putc),     MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(io_lshift,   MRB_OPSYM(lshift), MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(io_pwrite,   MRB_SYM(pwrite),   MRB_ARGS_ANY()),   /* Ruby 2.5 feature */
};

void
mrb_io_write_init(mrb_state *mrb, struct RClass *io)
{
  MRB_MT_INIT_ROM(mrb, io, io_write_rom_entries);
}
