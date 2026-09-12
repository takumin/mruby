/*
** process.c - Process module
**
** See Copyright Notice in mruby.h
**
** The common half of mruby-process: what Ruby promises about a process,
** expressed over the platform-neutral primitives in process_hal.h.
** Argument shapes, return conventions, `$?` and `$$` live here; what a pid
** or a signal or a wait status *is* stays behind the HAL.  The clocks are
** clock.c's and Process::Status is status.c's; this file defines the module
** and calls each in turn.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/error.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#ifdef MRB_USE_BIGINT
#include <mruby/internal.h>
#endif
#include "process_hal.h"
#include "process_internal.h"
#include "signal_hal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

/* `$?` and `$$` are not word names, so MRB_GVSYM() cannot spell them and
   they are interned where they are used. */
void
mrb_process_set_last_status(mrb_state *mrb, mrb_value status)
{
  mrb_gv_set(mrb, mrb_intern_lit(mrb, "$?"), status);
}

/*
 * Refuse a value that would not survive the narrowing a port has to do with
 * it.
 *
 * What is wrong with such a value is its size, and size is not something a
 * port can report: the HAL answers with an `errno`, which has no spelling for
 * "that was never a pid" and would have to borrow one that also means
 * something else.  So the value is refused here, where `RangeError` can be
 * said, and a port keeps `errno` for what the platform actually answered.
 * mruby-socket checks the `int` fields of a getaddrinfo hint the same way.
 */
mrb_int
mrb_process_int_arg(mrb_state *mrb, mrb_int v, const char *what)
{
#if MRB_INT_MAX > INT_MAX
  if (v < (mrb_int)INT_MIN || v > (mrb_int)INT_MAX) {
    mrb_raisef(mrb, E_RANGE_ERROR, "%s out of range: %i", what, v);
  }
#else
  (void)mrb;
  (void)what;
#endif
  return v;
}

/*
 * Read a signal argument into a number.
 *
 * Ruby lets a signal be an Integer, or a name as a String or Symbol, with or
 * without the "SIG" prefix.  Deciding that much is this gem's work; which
 * number a name stands for is mruby-signal's, through the signal HAL.
 */
static mrb_int
signal_to_number(mrb_state *mrb, mrb_value sig)
{
  const char *name;
  mrb_int len, signo;
  char bare[32];

  if (mrb_integer_p(sig)) {
    signo = mrb_integer(sig);
    if (signo < 0) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "signalling a process group is not supported");
    }
    return mrb_process_int_arg(mrb, signo, "signal number");
  }

  if (mrb_symbol_p(sig)) {
    name = mrb_sym_name_len(mrb, mrb_symbol(sig), &len);
  }
  else if (mrb_string_p(sig)) {
    name = RSTRING_PTR(sig);
    len = RSTRING_LEN(sig);
  }
  else {
    /* Anything else is refused by its class, as Ruby reports it.  A bigint
       lands here too: it is an Integer that no signal number can be, and
       `mrb_integer_p` is false for one, which is also how CRuby comes to
       report a value like 2**70 by class rather than by size. */
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "bad signal type %C", mrb_obj_class(mrb, sig));
  }
  if (name == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "bad signal name");
  }

  /* The HAL is asked in C strings, so a name holding a NUL would be read as
     the part before it and would signal something that was never asked for.
     Checked before the name is taken apart, as CRuby checks it. */
  if (memchr(name, '\0', (size_t)len) != NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "signal name with null byte");
  }

  /* A leading "-" asks for the process group, which this gem does not do
     yet; say so rather than quietly signalling the process instead. */
  if (len > 0 && name[0] == '-') {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "signalling a process group is not supported");
  }
  /* A name that is exactly "SIG" loses the prefix like any other, leaving
     nothing to look up.  Nothing is not an error of its own: Ruby reports the
     empty name the way it reports the empty string, as the message below
     writing "SIG" and then nothing, so it goes on to the lookup and fails
     there. */
  if (len >= 3 && memcmp(name, "SIG", 3) == 0) {
    name += 3;
    len -= 3;
  }
  if ((size_t)len >= sizeof(bare)) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unsupported signal 'SIG%l'", name, (size_t)len);
  }
  /* The HAL takes a C string, and `name` is a slice of a longer one. */
  memcpy(bare, name, (size_t)len);
  bare[len] = '\0';

  if (mrb_hal_signal_number(mrb, bare, &signo) != 0) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unsupported signal 'SIG%s'", bare);
  }
  return signo;
}

/*
 * call-seq:
 *   Process.pid -> integer
 *
 * The process ID of the running process.  Also available as <code>$$</code>.
 */
static mrb_value
process_pid(mrb_state *mrb, mrb_value self)
{
  mrb_int pid = mrb_hal_process_pid(mrb);

  if (pid < 0) mrb_sys_fail(mrb, "getpid");
  return mrb_int_value(mrb, pid);
}

/*
 * call-seq:
 *   Process.ppid -> integer
 *
 * The process ID of the parent of the running process.
 */
static mrb_value
process_ppid(mrb_state *mrb, mrb_value self)
{
  mrb_int ppid = mrb_hal_process_ppid(mrb);

  if (ppid < 0) mrb_sys_fail(mrb, "getppid");
  return mrb_int_value(mrb, ppid);
}

/*
 * call-seq:
 *   Process.kill(signal, pid, ...) -> integer
 *
 * Sends +signal+ to each process, and returns how many it was sent to.
 * At least one process must be named.  +signal+ is a number, or a name as a
 * String or Symbol with or without the "SIG" prefix.  Signal 0 sends nothing
 * and only checks that the process is there to be signalled; it is spelled
 * as the number, since "EXIT" names it only where a handler is being set.
 *
 *   Process.kill(:TERM, pid)
 *   Process.kill("SIGTERM", pid)
 *   Process.kill(0, pid)          # => 1 if pid exists, Errno::ESRCH if not
 *
 * Which processes a +pid+ names is what its sign says, and what a sign says
 * is the platform's to answer.  A positive number names the process with that
 * ID everywhere; where the platform reads the rest as POSIX does, 0 names
 * every process in the caller's process group, -1 every process the caller
 * has permission to signal, and a number below -1 every process in the
 * process group whose ID is -pid.  Windows has no such selectors and answers
 * Errno::ESRCH for every one of them.
 *
 * Naming a process group through the signal instead, which is a negative
 * signal number or a name written with a leading "-" and asks for the group
 * of each +pid+ given, is not supported yet and raises ArgumentError.  A
 * signal of any other class, a big integer included, raises ArgumentError
 * naming that class.  A signal number or a pid too large for the platform
 * to carry raises RangeError.
 */
static mrb_value
process_kill(mrb_state *mrb, mrb_value self)
{
  mrb_value sig, *pids;
  mrb_int argc, i, signo;

  mrb_get_args(mrb, "o*", &sig, &pids, &argc);
  /* The rest is what names the processes, so an empty one leaves nothing to
     signal.  Counting the signal back in makes the message read as the call
     was written. */
  if (argc == 0) {
    mrb_argnum_error(mrb, 1, 2, -1);
  }
  signo = signal_to_number(mrb, sig);

  for (i = 0; i < argc; i++) {
    mrb_int pid = mrb_process_int_arg(mrb, mrb_as_int(mrb, pids[i]), "pid");
    if (mrb_hal_process_kill(mrb, pid, signo) != 0) {
      /* What a SystemCallError message carries after the error itself is the
         object the call was working on, the way `File.open` names the path it
         could not open.  Signalling a process works on no such object, and
         CRuby names nothing here.  Passing the name of the call instead would
         put a word in the message that CRuby never prints. */
      mrb_sys_fail(mrb, NULL);
    }
  }
  return mrb_int_value(mrb, argc);
}

#ifdef MRB_HAL_PROCESS_HAS_WAIT
/* The wait itself.  Two module functions differ only in whether the status
   is handed back beside the pid, so the wait is done here; the wait publishes
   it through `$?`, and both of them leave it set. */
static mrb_value
wait_for_child(mrb_state *mrb, mrb_value *statusp)
{
  mrb_int pid = -1;
  mrb_int flags = 0;
  mrb_process_record *record;
  mrb_value status;

  mrb_get_args(mrb, "|ii", &pid, &flags);
  pid = mrb_process_int_arg(mrb, pid, "pid");

  /* A port is told what a wait means in mruby's own bits and answers only for
     the ones it was given, so a bit that stands for nothing has to be refused
     before it reaches one.  Checked here rather than in each port, so that
     every port refuses the same values. */
  if (flags < 0 || (flags & ~(mrb_int)MRB_PROCESS_WAIT_FLAGS) != 0) {
    errno = EINVAL;
    mrb_sys_fail(mrb, NULL);
  }

  /* A pid this interpreter spawned and still owes a wait for is waited on
     through its record, which is what makes reaching the same child twice
     harmless.  Every other pid goes to the platform as written: a number is
     not a claim on a process, but it is what waitpid(2) takes, and refusing
     one here would leave a child of the host application waitable through the
     ANY selector and not through its own number. */
  record = (pid > 0) ? mrb_process_record_find(mrb_process_table_get(mrb), pid) : NULL;
  if (record != NULL) {
    status = mrb_process_record_wait(mrb, record, (unsigned int)flags);
  }
  else {
    mrb_process_wait_target target;

    target.child = NULL;
    target.pid = pid;
    target.group = 0;
    if (pid > 0) {
      target.scope = MRB_PROCESS_WAIT_SCOPE_PID;
    }
    else if (pid == -1) {
      target.scope = MRB_PROCESS_WAIT_SCOPE_ANY;
    }
    else {
      /* The smallest Integer has no negation, and no process group has an ID
         that far from zero either, so the number names no group.  Answered
         as one rather than negated, which would be undefined. */
      if (pid == MRB_INT_MIN) {
        errno = ECHILD;
        mrb_sys_fail(mrb, "waitpid");
      }
      target.scope = MRB_PROCESS_WAIT_SCOPE_GROUP;
      target.group = -pid;  /* 0 stays 0, the caller's own group */
    }
    status = mrb_process_wait_set(mrb, &target, (unsigned int)flags);
  }
  *statusp = status;
  if (mrb_nil_p(status)) return mrb_nil_value();
  return mrb_int_value(mrb, mrb_process_status_pid(mrb, status));
}

/*
 * call-seq:
 *   Process.waitpid(pid = -1, flags = 0) -> integer or nil
 *   Process.wait(pid = -1, flags = 0)    -> integer or nil
 *
 * Waits for a child process to finish and returns its process ID, setting
 * <code>$?</code> to the Process::Status it finished with.  Which children
 * are waited for is what +pid+ chooses, as the platform reads it: a positive
 * number names one process, 0 the caller's process group, -1 (the default)
 * any child, and a number below -1 the process group whose ID is -pid.  The
 * children a selector draws from are the running process's, not only the
 * ones Process.spawn created, so an embedding application's own child can be
 * waited for here as well.
 *
 * A child this interpreter did spawn is also known to it by more than its
 * number, and waiting for it here goes through that: the wait is given the
 * child itself, so a number the platform has since handed to a stranger
 * cannot be what is waited for.
 *
 * With Process::WNOHANG among +flags+, returns nil and sets <code>$?</code>
 * to nil when no child is ready.  With Process::WUNTRACED, a stopped child
 * is reported too, where the platform has such a thing; a stopped child has
 * not finished, so it can still be waited for again.
 *
 * Raises Errno::ECHILD when +pid+ names no child of this process, including
 * one already reaped and, on a platform that can only wait for a child it
 * holds a handle to, one this interpreter did not spawn.  Raises
 * Errno::EINVAL when +flags+ holds a bit that is not one of the two,
 * Errno::ENOSYS on a platform whose waits cannot be narrowed to a process
 * group, and RangeError when +pid+ is too large for the platform to carry.
 */
static mrb_value
process_waitpid(mrb_state *mrb, mrb_value self)
{
  mrb_value status;

  return wait_for_child(mrb, &status);
}

/*
 * call-seq:
 *   Process.waitpid2(pid = -1, flags = 0) -> [pid, status] or nil
 *   Process.wait2(pid = -1, flags = 0)    -> [pid, status] or nil
 *
 * Waits as Process.waitpid does and returns the process ID and the
 * Process::Status together, rather than leaving the status to be read from
 * <code>$?</code>, which is set either way.  With Process::WNOHANG among
 * +flags+ and no child ready, returns nil.
 *
 *   pid, status = Process.wait2
 *   status.exitstatus   #=> 0
 */
static mrb_value
process_waitpid2(mrb_state *mrb, mrb_value self)
{
  mrb_value status, pid = wait_for_child(mrb, &status);

  if (mrb_nil_p(pid)) return mrb_nil_value();
  return mrb_assoc_new(mrb, pid, status);
}
#else
/* A port that declares no wait has no children to wait for.  The four
   spellings are defined as `mrb_notimplement_m`, the mark of a body this
   build does not supply: `respond_to?` answers false for them and a call
   raises NotImplementedError, as mruby-dir and mruby-io mark theirs. */
# define process_waitpid  mrb_notimplement_m
# define process_waitpid2 mrb_notimplement_m
#endif

void
mrb_mruby_process_gem_init(mrb_state *mrb)
{
  struct RClass *process;
  mrb_int pid;

  process = mrb_define_module_id(mrb, MRB_SYM(Process));

  /* The wait flags are mruby's own bits, not the host's: a program that
     passes Process::WNOHANG means the same thing on every port. */
  mrb_define_const_id(mrb, process, MRB_SYM(WNOHANG),
                      mrb_fixnum_value(MRB_PROCESS_WAIT_NOHANG));
  mrb_define_const_id(mrb, process, MRB_SYM(WUNTRACED),
                      mrb_fixnum_value(MRB_PROCESS_WAIT_UNTRACED));
  mrb_define_module_function_id(mrb, process, MRB_SYM(pid),     process_pid,     MRB_ARGS_NONE());
  mrb_define_module_function_id(mrb, process, MRB_SYM(ppid),    process_ppid,    MRB_ARGS_NONE());
  mrb_define_module_function_id(mrb, process, MRB_SYM(kill),    process_kill,    MRB_ARGS_REQ(2)|MRB_ARGS_REST());
  mrb_define_module_function_id(mrb, process, MRB_SYM(waitpid),  process_waitpid,  MRB_ARGS_OPT(2));
  mrb_define_module_function_id(mrb, process, MRB_SYM(wait),     process_waitpid,  MRB_ARGS_OPT(2));
  mrb_define_module_function_id(mrb, process, MRB_SYM(waitpid2), process_waitpid2, MRB_ARGS_OPT(2));
  mrb_define_module_function_id(mrb, process, MRB_SYM(wait2),    process_waitpid2, MRB_ARGS_OPT(2));

  mrb_process_clock_init(mrb, process);
  mrb_process_status_init(mrb, process);
  mrb_process_spawn_init(mrb, process);
  mrb_process_table_init(mrb, process);

  pid = mrb_hal_process_pid(mrb);
  if (pid >= 0) {
    mrb_gv_set(mrb, mrb_intern_lit(mrb, "$$"), mrb_int_value(mrb, pid));
  }
}

void
mrb_mruby_process_gem_final(mrb_state *mrb)
{
  mrb_process_table_final(mrb);
}
