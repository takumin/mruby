/*
** io_popen.c - IO.popen, and the child a popen stream carries
**
** A stream opened to a command is an ordinary IO over a pipe, with one
** thing no other stream has: a child process to wait for. Spawning it and
** reaping it are the port's, so what is left here is which pipes the two
** ends get and what becomes of `$?`.
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/internal.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/io.h>
#include "io_hal.h"
#include "io_internal.h"

#ifdef MRB_HAL_IO_HAS_SPAWN_PROCESS

#include <errno.h>
#include <string.h>

/* Only how a child's exit status reads is asked of the platform here; the
   waiting itself goes through the HAL. */
#if defined(_WIN32)
# define WEXITSTATUS(x) (x)
# ifdef _MSC_VER
typedef mrb_int pid_t;
# endif
#else
# include <sys/wait.h>   /* WEXITSTATUS() */
#endif

struct popen_params {
  mrb_value klass;
  const char *cmd;
  int flags;
  int doexec;
  int opt_in, opt_out, opt_err;
};

static int
option_to_fd(mrb_state *mrb, mrb_value v)
{
  if (mrb_undef_p(v)) return -1;
  if (mrb_nil_p(v)) return -1;

  switch (mrb_type(v)) {
    case MRB_TT_CDATA: /* IO */
      return mrb_io_fileno(mrb, v);
    case MRB_TT_INTEGER:
      return (int)mrb_integer(v);
    default:
      mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong exec redirect action");
      break;
  }
  return -1; /* never reached */
}

static void
parse_popen_args(mrb_state *mrb, struct popen_params *p)
{
  mrb_value mode = mrb_nil_value();
  struct { mrb_value opt_in, opt_out, opt_err; } kv;
  mrb_sym knames[3] = {MRB_SYM(in), MRB_SYM(out), MRB_SYM(err)};
  const mrb_kwargs kw = {
    3, 0,
    knames,
    &kv.opt_in,
    NULL,
  };

  mrb_get_args(mrb, "zo:", &p->cmd, &mode, &kw);

  p->flags = mrb_io_mode_to_flags(mrb, mode);
  p->doexec = (strcmp("-", p->cmd) != 0);
  p->opt_in = option_to_fd(mrb, kv.opt_in);
  p->opt_out = option_to_fd(mrb, kv.opt_out);
  p->opt_err = option_to_fd(mrb, kv.opt_err);
}

static mrb_value
io_s_popen(mrb_state *mrb, mrb_value klass)
{
  struct popen_params p;
  p.klass = klass;
  int pid = 0;
  int pr[2] = { -1, -1 };  /* read pipe: parent reads, child writes */
  int pw[2] = { -1, -1 };  /* write pipe: parent writes, child reads */
  int readable, writable;
  int stdin_fd = -1, stdout_fd = -1, stderr_fd = -1;

  mrb->c->ci->mid = 0;
  parse_popen_args(mrb, &p);

  readable = MRB_IO_READABLE_P(p.flags);
  writable = MRB_IO_WRITABLE_P(p.flags);

  /* Create pipes for communication */
  if (readable) {
    if (mrb_hal_io_pipe(mrb, pr) == -1) {
      mrb_sys_fail(mrb, "pipe");
    }
  }

  if (writable) {
    if (mrb_hal_io_pipe(mrb, pw) == -1) {
      if (pr[0] != -1) {
        mrb_hal_io_close(mrb, pr[0]);
        mrb_hal_io_close(mrb, pr[1]);
      }
      mrb_sys_fail(mrb, "pipe");
    }
  }

  /* Set up child process file descriptors */
  if (p.doexec) {
    /* Child stdin: either write pipe read end or opt_in */
    stdin_fd = (p.opt_in != -1) ? p.opt_in : (writable ? pw[0] : -1);

    /* Child stdout: either read pipe write end or opt_out */
    stdout_fd = (p.opt_out != -1) ? p.opt_out : (readable ? pr[1] : -1);

    /* Child stderr: opt_err or stdout */
    stderr_fd = (p.opt_err != -1) ? p.opt_err : stdout_fd;

    /* Spawn child process using HAL */
    if (mrb_hal_io_spawn_process(mrb, p.cmd, stdin_fd, stdout_fd, stderr_fd, &pid) == -1) {
      int saved_errno = errno;
      if (readable) {
        mrb_hal_io_close(mrb, pr[0]);
        mrb_hal_io_close(mrb, pr[1]);
      }
      if (writable) {
        mrb_hal_io_close(mrb, pw[0]);
        mrb_hal_io_close(mrb, pw[1]);
      }
      errno = saved_errno;
      mrb_raisef(mrb, E_IO_ERROR, "command not found: %s", p.cmd);
    }

    /* Close child ends of pipes in parent */
    if (readable) {
      mrb_hal_io_close(mrb, pr[1]);  /* close write end */
    }
    if (writable) {
      mrb_hal_io_close(mrb, pw[0]);  /* close read end */
    }
  }

  /* Set up parent IO object */
  mrb_value io = mrb_obj_value(mrb_data_object_alloc(mrb, mrb_class_ptr(klass), NULL, &mrb_io_type));
  struct mrb_io *fptr = mrb_io_alloc(mrb);

  if (readable && writable) {
    fptr->fd = pr[0];      /* parent reads from here */
    fptr->fd2 = pw[1];     /* parent writes to here */
  }
  else if (readable) {
    fptr->fd = pr[0];      /* parent reads from here */
    fptr->fd2 = -1;
  }
  else {
    fptr->fd = pw[1];      /* parent writes to here */
    fptr->fd2 = -1;
  }

  fptr->pid = pid;
  fptr->readable = readable;
  fptr->writable = writable;
  mrb_io_init_buf(mrb, fptr);

  DATA_TYPE(io) = &mrb_io_type;
  DATA_PTR(io)  = fptr;
  return io;
}

static void
io_set_process_status(mrb_state *mrb, pid_t pid, int status)
{
  struct RClass *c_status = NULL;
  mrb_value v;

  if (mrb_class_defined_id(mrb, MRB_SYM(Process))) {
    struct RClass *c_process = mrb_module_get_id(mrb, MRB_SYM(Process));
    if (mrb_const_defined(mrb, mrb_obj_value(c_process), MRB_SYM(Status))) {
      c_status = mrb_class_get_under_id(mrb, c_process, MRB_SYM(Status));
    }
  }
  if (c_status != NULL) {
    /* What this needs is the status object, and asking the class for `new` is
       not the way to it: mrb_obj_new() allocates one and hands #initialize the
       pid and the raw status, which is the path mruby-process takes itself.
       CRuby's Process::Status has no `new` to call. */
    mrb_value argv[2];

    argv[0] = mrb_fixnum_value(pid);
    argv[1] = mrb_fixnum_value(status);
    v = mrb_obj_new(mrb, c_status, 2, argv);
  }
  else {
    v = mrb_fixnum_value(WEXITSTATUS(status));
  }
  mrb_gv_set(mrb, mrb_intern_lit(mrb, "$?"), v);
}

void
mrb_io_reap_child(mrb_state *mrb, struct mrb_io *fptr, int quiet)
{
  if (fptr->pid == 0) return;

  /* The pid is whatever mrb_hal_io_spawn_process() handed out, and only
     the port that made it knows what it names and how to wait on it. */
  int pid, status;
  do {
    pid = mrb_hal_io_waitpid(mrb, fptr->pid, &status, 0);
  } while (pid == -1 && errno == EINTR);
  if (!quiet && pid == fptr->pid) {
    io_set_process_status(mrb, pid, status);
  }
  fptr->pid = 0;
  /* Note: we don't raise an exception when the wait fails */
}

#else
/* this port runs no command: unimplemented, and named as such so
   `respond_to?` can answer false */
# define io_s_popen mrb_notimplement_m
#endif /* MRB_HAL_IO_HAS_SPAWN_PROCESS */

void
mrb_io_popen_init(mrb_state *mrb, struct RClass *io)
{
  mrb_define_class_method_id(mrb, io, MRB_SYM(_popen), io_s_popen, MRB_ARGS_ARG(1,2));
}
