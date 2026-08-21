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
#include <mruby/numeric.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"
#include "signal_hal.h"

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
  /* What a process did is over by the time there is a status for it, and
     every question a status answers is read back from the snapshot set here.
     CRuby freezes the status it leaves in $? the same way. */
  mrb_obj_freeze(mrb, mrb_obj_value(data));
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
 * Whether +other+ equals the raw status, which is what #to_i gives back.
 * The pid takes no part: two statuses holding the same platform value are
 * equal whichever processes they came from.
 */
static mrb_value
status_eq(mrb_state *mrb, mrb_value self)
{
  mrb_value raw = mrb_int_value(mrb, mrb_process_status_ptr(mrb, self)->raw_status);
  mrb_value other;

  mrb_get_args(mrb, "o", &other);
  /* Ruby answers this question as `to_i == other`, and reaches a status on
     the right through Integer#== asking it back.  mrb_equal() answers false
     for anything that is not a number instead of asking, so a status is
     unwrapped here rather than left to it. */
  if (mrb_obj_class(mrb, self) == mrb_obj_class(mrb, other)) {
    other = mrb_int_value(mrb, mrb_process_status_ptr(mrb, other)->raw_status);
  }
  return mrb_bool_value(mrb_equal(mrb, raw, other));
}

/*
 * Append a number to the description being built.
 *
 * CRuby writes this description into one buffer as it goes, and so does this.
 * mrb_format() would be shorter to read, but it builds a String per piece and
 * concatenates it, which costs an allocation for every number written; the
 * buffer below is large enough for any mrb_int in base 10, sign included, so
 * the conversion cannot fail.
 */
static void
status_cat_int(mrb_state *mrb, mrb_value str, mrb_int n)
{
  char buf[MRB_INT_BIT / 3 + 3];

  mrb_str_cat_cstr(mrb, str, mrb_int_to_cstr(buf, sizeof(buf), n, 10));
}

/*
 * Spell a signal out the way Ruby does: " SIGKILL (signal 9)", or " signal 9"
 * where this platform gives the number no name.  `lead` is what stands
 * between the pid and the signal, which is " stopped" for a process that
 * stopped and nothing for one that was killed.
 */
static void
status_cat_signal(mrb_state *mrb, mrb_value str, const char *lead, mrb_int signo)
{
  const char *name = mrb_hal_signal_name(mrb, signo);

  mrb_str_cat_cstr(mrb, str, lead);
  if (name == NULL) {
    mrb_str_cat_lit(mrb, str, " signal ");
    status_cat_int(mrb, str, signo);
    return;
  }
  mrb_str_cat_lit(mrb, str, " SIG");
  mrb_str_cat_cstr(mrb, str, name);
  mrb_str_cat_lit(mrb, str, " (signal ");
  status_cat_int(mrb, str, signo);
  mrb_str_cat_lit(mrb, str, ")");
}

/*
 * call-seq:
 *   status.to_s -> string
 *
 * A description of how the process finished:
 *
 *   pid 1234 exit 0
 *   pid 1234 SIGKILL (signal 9)
 *   pid 1234 SIGSEGV (signal 11) (core dumped)
 *   pid 1234 stopped SIGSTOP (signal 19)
 *
 * A status the platform said nothing about is just "pid 1234".
 */
static mrb_value
status_to_s(mrb_state *mrb, mrb_value self)
{
  const mrb_process_status *st = mrb_process_status_ptr(mrb, self);
  mrb_value str;

  str = mrb_str_new_lit(mrb, "pid ");
  status_cat_int(mrb, str, st->pid);

  /* Each part is asked about on its own, as CRuby asks, rather than in an
     if/else chain: a port that reports two of them at once is then described
     twice over instead of having all but the first dropped. */
  if (st->flags & MRB_PROCESS_STATUS_STOPPED) {
    status_cat_signal(mrb, str, " stopped", st->stopsig);
  }
  if (st->flags & MRB_PROCESS_STATUS_SIGNALED) {
    status_cat_signal(mrb, str, "", st->termsig);
  }
  if (st->flags & MRB_PROCESS_STATUS_EXITED) {
    mrb_str_cat_lit(mrb, str, " exit ");
    status_cat_int(mrb, str, st->exitstatus);
  }
  if (st->flags & MRB_PROCESS_STATUS_COREDUMP) {
    mrb_str_cat_lit(mrb, str, " (core dumped)");
  }
  return str;
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

  mrb_define_method_id(mrb, status, MRB_SYM(pid),        status_pid,        MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(to_i),       status_to_i,       MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(to_s),       status_to_s,       MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(exited),   status_exited_p,   MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(exitstatus), status_exitstatus, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(signaled), status_signaled_p, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(termsig),    status_termsig,    MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(stopped),  status_stopped_p,  MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM(stopsig),    status_stopsig,    MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_SYM_Q(coredump), status_coredump_p, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, status, MRB_OPSYM(eq),       status_eq,         MRB_ARGS_REQ(1));
}
