/*
** io_internal.h - shared declarations within mruby-io
**
** See Copyright Notice in mruby.h
**
** Not part of the HAL and not for other gems: these are the things the
** gem's own sources say to each other.  Each source owns one part of the
** stream and keeps the rest of what it does to itself:
**
**   io.c         the descriptors a stream holds, the mode it was opened
**                with, and everything that opens one or gives one up
**   io_read.c    the read buffer and every method that reads through it,
**                plus the unbuffered #sysread and #pread
**   io_write.c   every method that writes, all of them straight through to
**                the descriptor
**   io_select.c  IO.select
**   io_popen.c   IO.popen and the child a popen stream carries
**   file.c       the File class
**   file_test.c  the FileTest predicates
*/

#ifndef MRUBY_IO_INTERNAL_H
#define MRUBY_IO_INTERNAL_H

#include <mruby.h>
#include <mruby/io.h>
#include <fcntl.h>
#include "io_hal.h"

MRB_BEGIN_DECL

/* The data type every IO carries.  A File is an IO, so file.c and
   file_test.c reach an fptr through this too. */
extern struct mrb_data_type mrb_io_type;

/* What a set of open(2) flags says about the stream it opened. */
#define MRB_IO_ACCESS_MODE_FLAGS (O_RDONLY | O_WRONLY | O_RDWR)
#define MRB_IO_RDONLY_P(f)   ((mrb_bool)(((f) & MRB_IO_ACCESS_MODE_FLAGS) == O_RDONLY))
#define MRB_IO_WRONLY_P(f)   ((mrb_bool)(((f) & MRB_IO_ACCESS_MODE_FLAGS) == O_WRONLY))
#define MRB_IO_RDWR_P(f)     ((mrb_bool)(((f) & MRB_IO_ACCESS_MODE_FLAGS) == O_RDWR))
#define MRB_IO_READABLE_P(f) ((mrb_bool)(MRB_IO_RDONLY_P(f) || MRB_IO_RDWR_P(f)))
#define MRB_IO_WRITABLE_P(f) ((mrb_bool)(MRB_IO_WRONLY_P(f) || MRB_IO_RDWR_P(f)))

/* Every method that touches the stream starts at one of these; only the ones
   that give a descriptor up rather than use it go on to accept a closed
   stream.  Each raises rather than answer NULL. */
struct mrb_io *mrb_io_get_fptr(mrb_state *mrb, mrb_value io);
struct mrb_io *mrb_io_get_open_fptr(mrb_state *mrb, mrb_value io);
struct mrb_io *mrb_io_get_read_fptr(mrb_state *mrb, mrb_value io);
struct mrb_io *mrb_io_get_write_fptr(mrb_state *mrb, mrb_value io);

/* The descriptor a write goes to: a stream with two of them reads from one
   and writes to the other. */
int mrb_io_get_write_fd(struct mrb_io *fptr);

/* A closed stream with nothing in it yet, and the read buffer it takes only
   if it is readable.  Whoever allocates one fills in the descriptors. */
struct mrb_io *mrb_io_alloc(mrb_state *mrb);
void mrb_io_init_buf(mrb_state *mrb, struct mrb_io *fptr);

/* The open(2) flags a mode argument asks for: nil, a mode string ("r+b"),
   or File::Constants numbers. */
int mrb_io_mode_to_flags(mrb_state *mrb, mrb_value mode);

/* Forget what the read buffer holds.  Whoever moves the descriptor out from
   under the buffer says so with this. */
static inline void
mrb_io_buf_reset(struct mrb_io_buf *buf)
{
  buf->start = 0;
  buf->len = 0;
}

#ifdef MRB_HAL_IO_HAS_SPAWN_PROCESS
/* Wait for the child a popen stream was opened to and let it go.  Unless
   `quiet` (the finalizer of a stream nobody closed), the status it was
   reaped with becomes `$?`. */
void mrb_io_reap_child(mrb_state *mrb, struct mrb_io *fptr, int quiet);
#else
/* No port to spawn a child with, so no stream carries one to reap. */
# define mrb_io_reap_child(mrb, fptr, quiet) ((void)0)
#endif

/* Each defines its own methods on `io`, once, from mrb_init_io(). */
void mrb_io_read_init(mrb_state *mrb, struct RClass *io);
void mrb_io_write_init(mrb_state *mrb, struct RClass *io);
void mrb_io_select_init(mrb_state *mrb, struct RClass *io);
void mrb_io_popen_init(mrb_state *mrb, struct RClass *io);

MRB_END_DECL

#endif /* MRUBY_IO_INTERNAL_H */
