/*
** status.c - Process::Status
**
** See Copyright Notice in mruby.h
**
** A status is a snapshot, taken by the port that produced it, of how a
** process left the CPU.  It is decoded once, where the platform's bits are
** still the platform's business, and never afterwards: a `Process::Status`
** outlives the child it came from, and a question asked of it later cannot
** be answered by asking the OS about a pid that may since have been reused.
**
** So there is no way to build one from a raw platform value.  A status comes
** from a wait, and only from a wait.
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"

static void
status_free(mrb_state *mrb, void *ptr)
{
  mrb_free(mrb, ptr);
}

static const mrb_data_type status_type = { "Process::Status", status_free };

const mrb_process_status*
mrb_process_status_ptr(mrb_state *mrb, mrb_value status)
{
  mrb_process_status *st = (mrb_process_status*)mrb_data_get_ptr(mrb, status, &status_type);

  if (st == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "uninitialized Process::Status");
  }
  return st;
}

mrb_value
mrb_process_status_new(mrb_state *mrb, const mrb_process_status *status)
{
  struct RClass *process = mrb_module_get_id(mrb, MRB_SYM(Process));
  struct RClass *klass = mrb_class_get_under_id(mrb, process, MRB_SYM(Status));
  struct RData *data = mrb_data_object_alloc(mrb, klass, NULL, &status_type);
  mrb_process_status *copy = (mrb_process_status*)mrb_malloc(mrb, sizeof(mrb_process_status));

  *copy = *status;
  data->data = copy;
  return mrb_obj_value(data);
}

static mrb_value
status_flag(mrb_state *mrb, mrb_value self, unsigned int flag)
{
  return mrb_bool_value((mrb_process_status_ptr(mrb, self)->flags & flag) != 0);
}

/*
 * call-seq:
 *   status.pid -> integer
 *
 * The process ID this status came from.
 */
static mrb_value
status_pid(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, mrb_process_status_ptr(mrb, self)->pid);
}

/*
 * call-seq:
 *   status.to_i -> integer
 *
 * The platform status as it was reported, unread.  Its layout is the
 * platform's business, which is why nothing above the port takes it apart.
 */
static mrb_value
status_to_i(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, mrb_process_status_ptr(mrb, self)->raw_status);
}

/*
 * call-seq:
 *   status.exited? -> true or false
 *
 * Whether the process ran to completion rather than being signalled.
 */
static mrb_value
status_exited_p(mrb_state *mrb, mrb_value self)
{
  return status_flag(mrb, self, MRB_PROCESS_STATUS_EXITED);
}

/*
 * call-seq:
 *   status.exitstatus -> integer or nil
 *
 * The status the process exited with, or nil if it did not exit.
 */
static mrb_value
status_exitstatus(mrb_state *mrb, mrb_value self)
{
  const mrb_process_status *st = mrb_process_status_ptr(mrb, self);

  if (!(st->flags & MRB_PROCESS_STATUS_EXITED)) return mrb_nil_value();
  return mrb_int_value(mrb, st->exitstatus);
}

/*
 * call-seq:
 *   status.signaled? -> true or false
 *
 * Whether an uncaught signal ended the process.
 */
static mrb_value
status_signaled_p(mrb_state *mrb, mrb_value self)
{
  return status_flag(mrb, self, MRB_PROCESS_STATUS_SIGNALED);
}

/*
 * call-seq:
 *   status.termsig -> integer or nil
 *
 * The signal that ended the process, or nil if none did.
 */
static mrb_value
status_termsig(mrb_state *mrb, mrb_value self)
{
  const mrb_process_status *st = mrb_process_status_ptr(mrb, self);

  if (!(st->flags & MRB_PROCESS_STATUS_SIGNALED)) return mrb_nil_value();
  return mrb_int_value(mrb, st->termsig);
}

/*
 * call-seq:
 *   status.stopped? -> true or false
 *
 * Whether the process is stopped rather than finished.  Only a wait made
 * with Process::WUNTRACED reports one.
 */
static mrb_value
status_stopped_p(mrb_state *mrb, mrb_value self)
{
  return status_flag(mrb, self, MRB_PROCESS_STATUS_STOPPED);
}

/*
 * call-seq:
 *   status.stopsig -> integer or nil
 *
 * The signal that stopped the process, or nil if it is not stopped.
 */
static mrb_value
status_stopsig(mrb_state *mrb, mrb_value self)
{
  const mrb_process_status *st = mrb_process_status_ptr(mrb, self);

  if (!(st->flags & MRB_PROCESS_STATUS_STOPPED)) return mrb_nil_value();
  return mrb_int_value(mrb, st->stopsig);
}

/*
 * call-seq:
 *   status.coredump? -> true or false
 *
 * Whether the signal that ended the process also dumped core.  Platforms
 * that cannot tell answer false.
 */
static mrb_value
status_coredump_p(mrb_state *mrb, mrb_value self)
{
  return status_flag(mrb, self, MRB_PROCESS_STATUS_COREDUMP);
}

/*
 * call-seq:
 *   status == other -> true or false
 *
 * Two statuses are equal when they describe the same process left the same
 * way.  Against an Integer, the raw status alone decides.
 */
static mrb_value
status_eq(mrb_state *mrb, mrb_value self)
{
  const mrb_process_status *st = mrb_process_status_ptr(mrb, self);
  mrb_value other;

  mrb_get_args(mrb, "o", &other);
  if (mrb_integer_p(other)) {
    return mrb_bool_value(st->raw_status == mrb_integer(other));
  }
  if (mrb_obj_class(mrb, self) != mrb_obj_class(mrb, other)) {
    return mrb_false_value();
  }
  {
    const mrb_process_status *o = mrb_process_status_ptr(mrb, other);
    return mrb_bool_value(st->pid == o->pid && st->raw_status == o->raw_status);
  }
}

/*
 * call-seq:
 *   Process::Status._signame(signo) -> string or nil
 *
 * The bare name this platform gives signal +signo+, without the "SIG"
 * prefix, or nil where the number names no signal.  Process::Status#to_s
 * uses it to spell a signal out; it is not a signal API of its own.
 */
static mrb_value
status_s_signame(mrb_state *mrb, mrb_value self)
{
  mrb_int signo;
  const char *name;

  mrb_get_args(mrb, "i", &signo);
  name = mrb_hal_process_signal_name(mrb, signo);
  if (name == NULL) return mrb_nil_value();
  return mrb_str_new_cstr(mrb, name);
}

void
mrb_process_status_init(mrb_state *mrb, struct RClass *process)
{
  struct RClass *status;

  status = mrb_define_class_under_id(mrb, process, MRB_SYM(Status), mrb->object_class);
  MRB_SET_INSTANCE_TT(status, MRB_TT_CDATA);

  /* A status describes a process this interpreter waited for.  There is
     nothing to build one out of but such a wait, so there is no way to build
     one by hand. */
  mrb_undef_class_method_id(mrb, status, MRB_SYM(new));

  mrb_define_class_method_id(mrb, status, MRB_SYM(_signame), status_s_signame, MRB_ARGS_REQ(1));

  mrb_define_method_id(mrb, status, MRB_SYM(pid),        status_pid,        MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(to_i),       status_to_i,       MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(to_int),     status_to_i,       MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(exited),   status_exited_p,   MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(exitstatus), status_exitstatus, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(signaled), status_signaled_p, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(termsig),    status_termsig,    MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(stopped),  status_stopped_p,  MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(stopsig),    status_stopsig,    MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(coredump), status_coredump_p, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_OPSYM(eq),       status_eq,         MRB_ARGS_REQ(1));
}
