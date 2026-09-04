/*
** io_select.c - IO.select
**
** The one place a program waits on several streams at once. Which
** descriptors a set holds, and what waiting on them costs, is the port's to
** say: the sets are opaque here and every question about one goes through
** mrb_hal_io_fdset_*().
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/internal.h>
#include <mruby/io.h>
#include "io_hal.h"
#include "io_internal.h"

#include <errno.h>

static mrb_io_timeval
time2timeval(mrb_state *mrb, mrb_value time)
{
  mrb_io_timeval t = { 0, 0 };

  switch (mrb_type(time)) {
    case MRB_TT_INTEGER:
      t.tv_sec = mrb_integer(time);
      t.tv_usec = 0;
      break;

#ifndef MRB_NO_FLOAT
    case MRB_TT_FLOAT:
      t.tv_sec = (mrb_int)mrb_float(time);
      t.tv_usec = (mrb_int)((mrb_float(time) - t.tv_sec) * 1000000.0);
      break;
#endif

    default:
      mrb_raise(mrb, E_TYPE_ERROR, "wrong argument class");
  }

  return t;
}

static int
mrb_io_read_data_pending(mrb_state *mrb, struct mrb_io *fptr)
{
  if (fptr->buf && fptr->buf->len > 0) return 1;
  return 0;
}

/*
 * call-seq:
 *   IO.select(read_array, write_array=nil, error_array=nil, timeout=nil) -> array or nil
 *
 * Performs a `select(2)` system call on the given arrays of `IO` objects.
 *
 * For each array, it can contain `IO` objects or `nil`.
 *
 * The `timeout` argument is a number of seconds.
 *
 * It returns a three-element array containing the `IO` objects that are
 * ready for reading, writing, or have an error, respectively.
 *
 * If the `timeout` is reached, it returns `nil`.
 *
 *   r, w = IO.pipe
 *   IO.select([r], [w])   #=> [[#<IO:fd 6>], [#<IO:fd 7>], []]
 */
static mrb_value
io_s_select(mrb_state *mrb, mrb_value klass)
{
  const mrb_value *argv;
  mrb_int argc;
  mrb_value read_io, list;
  struct mrb_io *fptr;
  int pending = 0;
  mrb_value result;
  int max = 0;
  int interrupt_flag = 0;

  mrb_get_args(mrb, "*", &argv, &argc);

  if (argc < 1 || argc > 4) {
    mrb_argnum_error(mrb, argc, 1, 4);
  }

  mrb_value timeout = mrb_nil_value();
  mrb_value except = mrb_nil_value();
  mrb_value write = mrb_nil_value();
  if (argc > 3)
    timeout = argv[3];
  if (argc > 2)
    except = argv[2];
  if (argc > 1)
    write = argv[1];
  mrb_value read = argv[0];

  mrb_io_timeval *tp, timerec;
  if (mrb_nil_p(timeout)) {
    tp = NULL;
  }
  else {
    timerec = time2timeval(mrb, timeout);
    tp = &timerec;
  }

  mrb_io_fdset *pset = mrb_hal_io_fdset_alloc(mrb);
  mrb_io_fdset *rset = NULL;
  mrb_io_fdset *rp = NULL;
  mrb_hal_io_fdset_zero(mrb, pset);
  if (!mrb_nil_p(read)) {
    mrb_check_type(mrb, read, MRB_TT_ARRAY);
    rset = mrb_hal_io_fdset_alloc(mrb);
    rp = rset;
    mrb_hal_io_fdset_zero(mrb, rp);
    /* Hoist pointer retrieval outside loop */
    mrb_value *read_ptr = RARRAY_PTR(read);
    for (int i = 0; i < RARRAY_LEN(read); i++) {
      read_io = read_ptr[i];
      fptr = mrb_io_get_open_fptr(mrb, read_io);
      mrb_hal_io_fdset_set(mrb, fptr->fd, rp);
      if (mrb_io_read_data_pending(mrb, fptr)) {
        pending++;
        mrb_hal_io_fdset_set(mrb, fptr->fd, pset);
      }
      if (max < fptr->fd)
        max = fptr->fd;
    }
    if (pending) {
      timerec.tv_sec = timerec.tv_usec = 0;
      tp = &timerec;
    }
  }

  mrb_io_fdset *wset = NULL;
  mrb_io_fdset *wp = NULL;
  if (!mrb_nil_p(write)) {
    mrb_check_type(mrb, write, MRB_TT_ARRAY);
    wset = mrb_hal_io_fdset_alloc(mrb);
    wp = wset;
    mrb_hal_io_fdset_zero(mrb, wp);
    /* Hoist pointer retrieval outside loop */
    mrb_value *write_ptr = RARRAY_PTR(write);
    for (int i = 0; i < RARRAY_LEN(write); i++) {
      fptr = mrb_io_get_open_fptr(mrb, write_ptr[i]);
      mrb_hal_io_fdset_set(mrb, fptr->fd, wp);
      if (max < fptr->fd)
        max = fptr->fd;
      if (fptr->fd2 >= 0) {
        mrb_hal_io_fdset_set(mrb, fptr->fd2, wp);
        if (max < fptr->fd2)
          max = fptr->fd2;
      }
    }
  }

  mrb_io_fdset *eset = NULL;
  mrb_io_fdset *ep = NULL;
  if (!mrb_nil_p(except)) {
    mrb_check_type(mrb, except, MRB_TT_ARRAY);
    eset = mrb_hal_io_fdset_alloc(mrb);
    ep = eset;
    mrb_hal_io_fdset_zero(mrb, ep);
    /* Hoist pointer retrieval outside loop */
    mrb_value *except_ptr = RARRAY_PTR(except);
    for (int i = 0; i < RARRAY_LEN(except); i++) {
      fptr = mrb_io_get_open_fptr(mrb, except_ptr[i]);
      mrb_hal_io_fdset_set(mrb, fptr->fd, ep);
      if (max < fptr->fd)
        max = fptr->fd;
      if (fptr->fd2 >= 0) {
        mrb_hal_io_fdset_set(mrb, fptr->fd2, ep);
        if (max < fptr->fd2)
          max = fptr->fd2;
      }
    }
  }

  max++;

  int n;
retry:
  n = mrb_hal_io_select(mrb, max, rp, wp, ep, tp);
  if (n < 0) {
    if (errno != EINTR) {
      mrb_hal_io_fdset_free(mrb, pset);
      mrb_hal_io_fdset_free(mrb, rset);
      mrb_hal_io_fdset_free(mrb, wset);
      mrb_hal_io_fdset_free(mrb, eset);
      mrb_sys_fail(mrb, "select failed");
    }
    if (tp == NULL)
      goto retry;
    interrupt_flag = 1;
  }

  if (!pending && n == 0) {
    mrb_hal_io_fdset_free(mrb, pset);
    mrb_hal_io_fdset_free(mrb, rset);
    mrb_hal_io_fdset_free(mrb, wset);
    mrb_hal_io_fdset_free(mrb, eset);
    return mrb_nil_value();
  }

  result = mrb_ary_new_capa(mrb, 3);
  mrb_ary_push(mrb, result, rp ? mrb_ary_new(mrb) : mrb_ary_new_capa(mrb, 0));
  mrb_ary_push(mrb, result, wp ? mrb_ary_new(mrb) : mrb_ary_new_capa(mrb, 0));
  mrb_ary_push(mrb, result, ep ? mrb_ary_new(mrb) : mrb_ary_new_capa(mrb, 0));

  if (interrupt_flag == 0) {
    if (rp) {
      list = RARRAY_PTR(result)[0];
      /* Hoist pointer retrieval outside loop */
      mrb_value *read_ptr = RARRAY_PTR(read);
      for (int i = 0; i < RARRAY_LEN(read); i++) {
        mrb_value io = read_ptr[i];
        fptr = mrb_io_get_open_fptr(mrb, io);
        if (mrb_hal_io_fdset_isset(mrb, fptr->fd, rp) ||
            mrb_hal_io_fdset_isset(mrb, fptr->fd, pset)) {
          mrb_ary_push(mrb, list, io);
        }
      }
    }

    if (wp) {
      list = RARRAY_PTR(result)[1];
      /* Hoist pointer retrieval outside loop */
      mrb_value *write_ptr = RARRAY_PTR(write);
      for (int i = 0; i < RARRAY_LEN(write); i++) {
        mrb_value io = write_ptr[i];
        fptr = mrb_io_get_open_fptr(mrb, io);
        if (mrb_hal_io_fdset_isset(mrb, fptr->fd, wp)) {
          mrb_ary_push(mrb, list, io);
        }
        else if (fptr->fd2 >= 0 && mrb_hal_io_fdset_isset(mrb, fptr->fd2, wp)) {
          mrb_ary_push(mrb, list, io);
        }
      }
    }

    if (ep) {
      list = RARRAY_PTR(result)[2];
      /* Hoist pointer retrieval outside loop */
      mrb_value *except_ptr = RARRAY_PTR(except);
      for (int i = 0; i < RARRAY_LEN(except); i++) {
        mrb_value io = except_ptr[i];
        fptr = mrb_io_get_open_fptr(mrb, io);
        if (mrb_hal_io_fdset_isset(mrb, fptr->fd, ep)) {
          mrb_ary_push(mrb, list, io);
        }
        else if (fptr->fd2 >= 0 && mrb_hal_io_fdset_isset(mrb, fptr->fd2, ep)) {
          mrb_ary_push(mrb, list, io);
        }
      }
    }
  }

  mrb_hal_io_fdset_free(mrb, pset);
  mrb_hal_io_fdset_free(mrb, rset);
  mrb_hal_io_fdset_free(mrb, wset);
  mrb_hal_io_fdset_free(mrb, eset);

  return result;
}

void
mrb_io_select_init(mrb_state *mrb, struct RClass *io)
{
  mrb_define_class_method_id(mrb, io, MRB_SYM(select), io_s_select, MRB_ARGS_ARG(1,3));
}
