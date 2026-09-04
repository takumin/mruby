/*
** io_read.c - reading from an IO
**
** Every read but #sysread and #pread comes out of the buffer this file
** keeps.  A read that finds it empty asks the descriptor for a whole
** MRB_IO_BUF_SIZE of bytes and answers out of what came back, which is what
** lets #gets look for a line without a system call per byte, and what
** leaves the descriptor further along than the reader is (see
** io_prepare_write() in io_write.c, which puts it back).
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

#include <limits.h>
#include <string.h>

static void
eof_error(mrb_state *mrb)
{
  mrb_raise(mrb, E_EOF_ERROR, "end of file reached");
}

/* The buffer: what fills it, and what takes bytes back out of it. */

static void
io_buf_shift(struct mrb_io_buf *buf, mrb_int n)
{
  mrb_assert(n <= SHRT_MAX);
  buf->start += (short)n;
  buf->len -= (short)n;
}

#ifdef MRB_UTF8_STRING
static void
io_fill_buf_comp(mrb_state *mrb, struct mrb_io *fptr)
{
  struct mrb_io_buf *buf = fptr->buf;
  int keep = buf->len;

  memmove(buf->mem, buf->mem+buf->start, keep);
  int n = mrb_hal_io_read(mrb, fptr->fd, buf->mem+keep, MRB_IO_BUF_SIZE-keep);
  if (n < 0) mrb_sys_fail(mrb, 0);
  if (n == 0) fptr->eof = 1;
  buf->start = 0;
  buf->len += (short)n;
}
#endif

static void
io_fill_buf(mrb_state *mrb, struct mrb_io *fptr)
{
  struct mrb_io_buf *buf = fptr->buf;

  if (buf->len > 0) return;

  int n = mrb_hal_io_read(mrb, fptr->fd, buf->mem, MRB_IO_BUF_SIZE);
  if (n < 0) mrb_sys_fail(mrb, 0);
  if (n == 0) fptr->eof = 1;
  buf->start = 0;
  buf->len = (short)n;
}

static void
io_buf_cat(mrb_state *mrb, mrb_value outbuf, struct mrb_io_buf *buf, mrb_int n)
{
  mrb_assert(n <= buf->len);
  mrb_str_cat(mrb, outbuf, buf->mem+buf->start, n);
  io_buf_shift(buf, n);
}

static void
io_buf_cat_all(mrb_state *mrb, mrb_value outbuf, struct mrb_io_buf *buf)
{
  mrb_str_cat(mrb, outbuf, buf->mem+buf->start, buf->len);
  mrb_io_buf_reset(buf);
}

static mrb_value
io_read_all(mrb_state *mrb, struct mrb_io *fptr, mrb_value outbuf)
{
  for (;;) {
    io_fill_buf(mrb, fptr);
    if (fptr->eof) {
      return outbuf;
    }
    io_buf_cat_all(mrb, outbuf, fptr->buf);
  }
}

static mrb_value
io_reset_outbuf(mrb_state *mrb, mrb_value outbuf, mrb_int len)
{
  if (mrb_nil_p(outbuf)) {
    outbuf = mrb_str_new(mrb, NULL, 0);
  }
  else {
    mrb_str_modify(mrb, mrb_str_ptr(outbuf));
    RSTR_SET_LEN(mrb_str_ptr(outbuf), 0);
  }
  return outbuf;
}

static mrb_int
io_find_index(struct mrb_io *fptr, const char *rs, mrb_int rslen)
{
  struct mrb_io_buf *buf = fptr->buf;

  mrb_assert(rslen > 0);
  const char c = rs[0];
  const mrb_int limit = buf->len - rslen + 1;
  const char *p = buf->mem+buf->start;
  for (mrb_int i=0; i<limit; i++) {
    if (p[i] == c && (rslen == 1 || memcmp(p+i, rs, rslen) == 0)) {
      return i;
    }
  }
  return -1;
}

/* Helper function for ungetc operations with raw data */
static void
io_unget_data(mrb_state *mrb, struct mrb_io *fptr, const char *ptr, mrb_int len)
{
  struct mrb_io_buf *buf = fptr->buf;

  if (len > SHRT_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "string too long to ungetc");
  }
  if (buf->len + len > SHRT_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "total ungetc buffer exceeds maximum size");
  }
  if (buf->len + len > MRB_IO_BUF_SIZE) {
    /* Compact the live bytes to the front before the realloc. The realloc is
       sized from len, but the memmove below reads buf->len bytes starting at
       buf->start, and a prior grow followed by a partial read can leave
       start+len past the new (possibly smaller) block. Compacting first, the
       way io_fill_buf_comp does, keeps that read in bounds (#6964). */
    if (buf->start > 0) {
      memmove(buf->mem, buf->mem+buf->start, buf->len);
      buf->start = 0;
    }
    fptr->buf = (struct mrb_io_buf*)mrb_realloc(mrb, buf, sizeof(struct mrb_io_buf)+buf->len+len-MRB_IO_BUF_SIZE);
    buf = fptr->buf;
  }
  memmove(buf->mem+len, buf->mem+buf->start, buf->len);
  memcpy(buf->mem, ptr, len);
  buf->start = 0;
  buf->len += (short)len;
}

/* The methods that read through the buffer. */

/*
 * call-seq:
 *   ios.read(length = nil, outbuf = "") -> string, outbuf, or nil
 *
 * Reads `length` bytes from the I/O stream.
 *
 * If `length` is `nil`, it reads until end of file.
 * If `outbuf` is given, it will be used as the buffer.
 *
 *   f = File.new("testfile")
 *   f.read(16)   #=> "This is line one"
 */
static mrb_value
io_read(mrb_state *mrb, mrb_value io)
{
  mrb_value outbuf = mrb_nil_value();
  mrb_value len;
  mrb_int length = 0;
  mrb_bool length_given;
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);

  mrb_get_args(mrb, "|o?S", &len, &length_given, &outbuf);
  if (length_given) {
    if (mrb_nil_p(len)) {
      length_given = FALSE;
    }
    else {
      length = mrb_as_int(mrb, len);
      if (length < 0) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative length %i given", length);
      }
      if (length == 0) {
        return io_reset_outbuf(mrb, outbuf, 0);
      }
    }
  }

  outbuf = io_reset_outbuf(mrb, outbuf, MRB_IO_BUF_SIZE);
  if (!length_given) {          /* read as much as possible */
    return io_read_all(mrb, fptr, outbuf);
  }

  struct mrb_io_buf *buf = fptr->buf;

  for (;;) {
    io_fill_buf(mrb, fptr);
    if (fptr->eof || length == 0) {
      if (RSTRING_LEN(outbuf) == 0)
        return mrb_nil_value();
      return outbuf;
    }
    if (buf->len < length) {
      length -= buf->len;
      io_buf_cat_all(mrb, outbuf, buf);
    }
    else {
      io_buf_cat(mrb, outbuf, buf, length);
      return outbuf;
    }
  }
}

static mrb_value
io_gets(mrb_state *mrb, mrb_value io)
{
  mrb_value rs = mrb_nil_value();
  mrb_bool rs_given = FALSE;    /* newline break */
  mrb_int limit = 0;
  mrb_bool limit_given = FALSE; /* no limit */
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);
  struct mrb_io_buf *buf = fptr->buf;

  mrb_get_args(mrb, "|o?i?", &rs, &rs_given, &limit, &limit_given);

  if (limit_given == FALSE) {
    if (rs_given) {
      if (mrb_nil_p(rs)) {
        rs_given = FALSE;
      }
      else if (mrb_integer_p(rs)) {
        limit = mrb_integer(rs);
        limit_given = TRUE;
        rs = mrb_nil_value();
      }
      else if (!mrb_string_p(rs)) {
        mrb_ensure_int_type(mrb, rs);
      }
    }
  }
  if (rs_given) {
    if (mrb_nil_p(rs)) {
      rs_given = FALSE;
    }
    else {
      mrb_ensure_string_type(mrb, rs);
      if (RSTRING_LEN(rs) == 0) { /* paragraph mode */
        rs = mrb_str_new_lit(mrb, "\n\n");
      }
    }
  }
  else {
    rs = mrb_str_new_lit(mrb, "\n");
    rs_given = TRUE;
  }

  /* from now on rs_given==FALSE means no RS */
  if (mrb_nil_p(rs) && !limit_given) {
    return io_read_all(mrb, fptr, mrb_str_new_capa(mrb, MRB_IO_BUF_SIZE));
  }

  io_fill_buf(mrb, fptr);
  if (fptr->eof) return mrb_nil_value();

  mrb_value outbuf;
  if (limit_given) {
    if (limit < 0) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative length %i given", limit);
    }
    if (limit == 0) return mrb_str_new(mrb, NULL, 0);
    outbuf = mrb_str_new_capa(mrb, limit);
  }
  else {
    outbuf = mrb_str_new(mrb, NULL, 0);
  }

  for (;;) {
    if (rs_given) {                /* with RS */
      mrb_int rslen = RSTRING_LEN(rs);
      mrb_int idx = io_find_index(fptr, RSTRING_PTR(rs), rslen);
      if (idx >= 0) {              /* found */
        mrb_int n = idx+rslen;
        if (limit_given && limit < n) {
          n = limit;
        }
        io_buf_cat(mrb, outbuf, buf, n);
        return outbuf;
      }
    }
    if (limit_given) {
      if (limit <= buf->len) {
        io_buf_cat(mrb, outbuf, buf, limit);
        return outbuf;
      }
      limit -= buf->len;
    }
    io_buf_cat_all(mrb, outbuf, buf);
    io_fill_buf(mrb, fptr);
    if (fptr->eof) {
      if (RSTRING_LEN(outbuf) == 0) return mrb_nil_value();
      return outbuf;
    }
  }
}

static mrb_value
io_readline(mrb_state *mrb, mrb_value io)
{
  mrb_value result = io_gets(mrb, io);
  if (mrb_nil_p(result)) {
    eof_error(mrb);
  }
  return result;
}

static mrb_value
io_readlines(mrb_state *mrb, mrb_value io)
{
  mrb_value ary = mrb_ary_new(mrb);
  for (;;) {
    mrb_value line = io_gets(mrb, io);

    if (mrb_nil_p(line)) return ary;
    mrb_ary_push(mrb, ary, line);
  }
}

static mrb_value
io_getc(mrb_state *mrb, mrb_value io)
{
  mrb_int len = 1;
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);
  struct mrb_io_buf *buf = fptr->buf;

  io_fill_buf(mrb, fptr);
  if (fptr->eof) return mrb_nil_value();
#ifdef MRB_UTF8_STRING
  const char *p = &buf->mem[buf->start];
  if ((*p) & 0x80) {
    len = mrb_utf8len(p, p+buf->len);
    if (len == 1 && buf->len < 4) { /* partial UTF-8 */
      io_fill_buf_comp(mrb, fptr);
      p = &buf->mem[buf->start];
      len = mrb_utf8len(p, p+buf->len);
    }
  }
#endif
  mrb_value str = mrb_str_new(mrb, buf->mem+buf->start, len);
  io_buf_shift(buf, len);
  return str;
}

static mrb_value
io_readchar(mrb_state *mrb, mrb_value io)
{
  mrb_value result = io_getc(mrb, io);
  if (mrb_nil_p(result)) {
    eof_error(mrb);
  }
  return result;
}

/*
 * call-seq:
 *   ios.getbyte -> integer or nil
 *
 * Reads a byte from the `IO` stream.
 *
 * Returns the byte as an integer, or `nil` at end of file.
 *
 *   f = File.new("testfile")
 *   f.getbyte   #=> 72
 */
static mrb_value
io_getbyte(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);
  struct mrb_io_buf *buf = fptr->buf;

  io_fill_buf(mrb, fptr);
  if (fptr->eof) return mrb_nil_value();

  unsigned char c = buf->mem[buf->start];
  io_buf_shift(buf, 1);
  return mrb_int_value(mrb, (mrb_int)c);
}

/*
 * call-seq:
 *   ios.readbyte -> integer
 *
 * Reads a byte from the `IO` stream.
 *
 * Returns the byte as an integer. Raises `EOFError` at end of file.
 *
 *   f = File.new("testfile")
 *   f.readbyte   #=> 72
 */
static mrb_value
io_readbyte(mrb_state *mrb, mrb_value io)
{
  mrb_value result = io_getbyte(mrb, io);
  if (mrb_nil_p(result)) {
    eof_error(mrb);
  }
  return result;
}

static mrb_value
io_eof(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);

  if (fptr->eof) return mrb_true_value();
  if (fptr->buf->len > 0) return mrb_false_value();
  io_fill_buf(mrb, fptr);
  return mrb_bool_value(fptr->eof);
}

/*
 * call-seq:
 *   ios.ungetc(string)   -> nil
 *
 * Pushes back characters (passed as a parameter) onto ios, such that a
 * subsequent buffered character read will return it. Has no effect with
 * unbuffered reads (such as IO#sysread).
 *
 *   f = File.new("testfile")   #=> #<File:testfile>
 *   c = f.getc                 #=> "H"
 *   f.ungetc(c)                #=> nil
 *   f.getc                     #=> "H"
 */
static mrb_value
io_ungetc(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);
  mrb_value str;

  mrb_get_args(mrb, "S", &str);
  io_unget_data(mrb, fptr, RSTRING_PTR(str), RSTRING_LEN(str));
  return mrb_nil_value();
}

/*
 * call-seq:
 *   ios.ungetbyte(string)   -> nil
 *   ios.ungetbyte(integer)  -> nil
 *
 * Pushes back bytes (passed as a parameter) onto ios, such that a subsequent
 * buffered character read will return it. Only one byte may be pushed back
 * before a subsequent read operation (that is, you will be able to read only
 * the last of several bytes that have been pushed back). Has no effect with
 * unbuffered reads (such as IO#sysread).
 */
static mrb_value
io_ungetbyte(mrb_state *mrb, mrb_value io)
{
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);
  mrb_value c = mrb_get_arg1(mrb);
  unsigned char byte_val;

  if (mrb_string_p(c)) {
    if (RSTRING_LEN(c) == 0) {
      return mrb_nil_value(); /* Empty string, do nothing */
    }
    byte_val = (unsigned char)RSTRING_PTR(c)[0];
  }
  else {
    mrb_int val = mrb_integer(c);
    byte_val = (unsigned char)(val & 0xff);
  }

  /* Use helper function with single byte */
  io_unget_data(mrb, fptr, (const char*)&byte_val, 1);
  return mrb_nil_value();
}

/* #sysread and #pread go to the descriptor directly, leaving the buffer
   as it was: what they answer is whatever one read(2) returned. */

/* The unbuffered readers (#sysread, #pread) hand the descriptor a string of
   `maxlen` bytes and trim it to what arrived. Sizing the string comes first;
   a read of nothing is answered here with an empty string and asks the
   stream for nothing, not even to be open. */
static mrb_value
io_sysread_buf(mrb_state *mrb, mrb_value buf, mrb_int maxlen)
{
  if (maxlen < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative expanding string size");
  }
  else if (maxlen == 0) {
    return mrb_str_new(mrb, NULL, 0);
  }

  if (mrb_nil_p(buf)) {
    buf = mrb_str_new(mrb, NULL, maxlen);
  }

  if (RSTRING_LEN(buf) != maxlen) {
    buf = mrb_str_resize(mrb, buf, maxlen);
  }
  else {
    mrb_str_modify(mrb, RSTRING(buf));
  }
  return buf;
}

static mrb_value
io_sysread_done(mrb_state *mrb, struct mrb_io *fptr, mrb_value buf, mrb_int n)
{
  if (n < 0) {
    mrb_sys_fail(mrb, "sysread failed");
  }
  if (RSTRING_LEN(buf) != n) {
    buf = mrb_str_resize(mrb, buf, n);
  }
  if (n == 0) {
    fptr->eof = 1;
    eof_error(mrb);
  }
  return buf;
}

static mrb_value
io_sysread(mrb_state *mrb, mrb_value io)
{
  mrb_value buf = mrb_nil_value();
  mrb_int maxlen;

  mrb_get_args(mrb, "i|S", &maxlen, &buf);

  buf = io_sysread_buf(mrb, buf, maxlen);
  if (maxlen == 0) return buf;
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);
  mrb_int n = mrb_hal_io_read(mrb, fptr->fd, RSTRING_PTR(buf), (size_t)maxlen);
  return io_sysread_done(mrb, fptr, buf, n);
}

/* Positional reading, where the platform has pread(2) to do it with; see
   MRB_USE_IO_PREAD_PWRITE in mruby/io.h for what decides that and why the
   call is made directly rather than through the HAL. */
#ifndef MRB_USE_IO_PREAD_PWRITE
# define io_pread mrb_notimplement_m
#else
#include <sys/types.h>
#include <unistd.h>

/*
 * call-seq:
 *  pread(maxlen, offset, outbuf = "") -> outbuf
 */
static mrb_value
io_pread(mrb_state *mrb, mrb_value io)
{
  mrb_value buf = mrb_nil_value();
  mrb_value off;
  mrb_int maxlen;

  mrb_get_args(mrb, "io|S!", &maxlen, &off, &buf);

  off_t offset = (off_t)mrb_as_int(mrb, off);
  buf = io_sysread_buf(mrb, buf, maxlen);
  if (maxlen == 0) return buf;
  struct mrb_io *fptr = mrb_io_get_read_fptr(mrb, io);
  mrb_int n = (mrb_int)pread(fptr->fd, RSTRING_PTR(buf), (size_t)maxlen, offset);
  return io_sysread_done(mrb, fptr, buf, n);
}
#endif /* MRB_USE_IO_PREAD_PWRITE */

static const mrb_mt_entry io_read_rom_entries[] = {
  MRB_MT_ENTRY(io_eof,       MRB_SYM_Q(eof),   MRB_ARGS_NONE()),     /* 15.2.20.5.6 */
  MRB_MT_ENTRY(io_getc,      MRB_SYM(getc),    MRB_ARGS_NONE()),     /* 15.2.20.5.8 */
  MRB_MT_ENTRY(io_gets,      MRB_SYM(gets),    MRB_ARGS_OPT(2)),     /* 15.2.20.5.9 */
  MRB_MT_ENTRY(io_read,      MRB_SYM(read),    MRB_ARGS_OPT(2)),     /* 15.2.20.5.14 */
  MRB_MT_ENTRY(io_readchar,  MRB_SYM(readchar), MRB_ARGS_NONE()),    /* 15.2.20.5.15 */
  MRB_MT_ENTRY(io_readline,  MRB_SYM(readline), MRB_ARGS_OPT(2)),    /* 15.2.20.5.16 */
  MRB_MT_ENTRY(io_readlines, MRB_SYM(readlines), MRB_ARGS_OPT(2)),   /* 15.2.20.5.17 */
  MRB_MT_ENTRY(io_getbyte,   MRB_SYM(getbyte), MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_readbyte,  MRB_SYM(readbyte), MRB_ARGS_NONE()),
  MRB_MT_ENTRY(io_ungetc,    MRB_SYM(ungetc),  MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(io_ungetbyte, MRB_SYM(ungetbyte), MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(io_sysread,   MRB_SYM(sysread), MRB_ARGS_ARG(1,1)),
  MRB_MT_ENTRY(io_pread,     MRB_SYM(pread),   MRB_ARGS_ANY()),      /* Ruby 2.5 feature */
};

void
mrb_io_read_init(mrb_state *mrb, struct RClass *io)
{
  MRB_MT_INIT_ROM(mrb, io, io_read_rom_entries);
}
