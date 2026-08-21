/*
** process.c - Process module
**
** See Copyright Notice in mruby.h
**
** The common half of mruby-process: everything Ruby promises about a
** process, expressed over the platform-neutral primitives in process_hal.h.
** Argument shapes, return conventions, `$?` and `$$` live here; what a pid
** or a signal or a wait status *is* stays behind the HAL.
*/

#include <mruby.h>
#include <mruby/error.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <string.h>

/* `$?` and `$$` are not word names, so MRB_GVSYM() cannot spell them and
   they are interned where they are used. */
void
mrb_process_set_last_status(mrb_state *mrb, mrb_value status)
{
  mrb_gv_set(mrb, mrb_intern_lit(mrb, "$?"), status);
}

/*
 * Read a signal argument into a number.
 *
 * Ruby lets a signal be an Integer, or a name as a String or Symbol, with or
 * without the "SIG" prefix.  Deciding that much is common-layer work; which
 * number a name stands for is the port's.
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
    return signo;
  }

  if (mrb_symbol_p(sig)) {
    name = mrb_sym_name_len(mrb, mrb_symbol(sig), &len);
  }
  else {
    sig = mrb_ensure_string_type(mrb, sig);
    name = RSTRING_PTR(sig);
    len = RSTRING_LEN(sig);
  }
  if (name == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "bad signal name");
  }

  /* A leading "-" asks for the process group, which this gem does not do
     yet; say so rather than quietly signalling the process instead. */
  if (len > 0 && name[0] == '-') {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "signalling a process group is not supported");
  }
  if (len > 3 && memcmp(name, "SIG", 3) == 0) {
    name += 3;
    len -= 3;
  }
  if (len <= 0 || (size_t)len >= sizeof(bare)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "bad signal name");
  }
  /* The HAL takes a C string, and `name` is a slice of a longer one. */
  memcpy(bare, name, (size_t)len);
  bare[len] = '\0';

  /* "EXIT" is Ruby's spelling of signal 0 -- the one that asks whether a
     process can be signalled rather than signalling it -- and stands for
     the same thing everywhere, so no port needs to know it. */
  if (strcmp(bare, "EXIT") == 0) return 0;

  if (mrb_hal_process_signal_number(mrb, bare, &signo) != 0) {
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
 * +signal+ is a number, or a name as a String or Symbol with or without the
 * "SIG" prefix.  Signal 0 (also spelled "EXIT") sends nothing and only
 * checks that the process is there to be signalled.
 *
 *   Process.kill(:TERM, pid)
 *   Process.kill("SIGTERM", pid)
 *   Process.kill(0, pid)          # => 1 if pid exists, Errno::ESRCH if not
 *
 * Signalling a process group -- a negative signal, or a name written with a
 * leading "-" -- is not supported yet and raises ArgumentError.
 */
static mrb_value
process_kill(mrb_state *mrb, mrb_value self)
{
  mrb_value sig, *pids;
  mrb_int argc, i, signo;

  mrb_get_args(mrb, "o*", &sig, &pids, &argc);
  signo = signal_to_number(mrb, sig);

  for (i = 0; i < argc; i++) {
    mrb_int pid = mrb_as_int(mrb, pids[i]);
    if (mrb_hal_process_kill(mrb, pid, signo) != 0) {
      mrb_sys_fail(mrb, "kill");
    }
  }
  return mrb_int_value(mrb, argc);
}

/*
 * A pid this interpreter may wait on: one it spawned and has not yet
 * accounted for.  Anything else is Errno::ECHILD, as it is in Ruby -- a
 * number is not a claim on a process, and the child it once named may since
 * have been reaped and the number handed to someone else.
 */
static mrb_process_record*
live_record(mrb_state *mrb, mrb_int pid)
{
  mrb_process_record *record = mrb_process_record_find(mrb_process_table_get(mrb), pid);

  if (record == NULL) {
    errno = ECHILD;
    mrb_sys_fail(mrb, "waitpid");
  }
  return record;
}

/* -1 is wait-any; 0 and the other negative pids name a process group, which
   this gem does not do yet. */
static void
check_wait_pid(mrb_state *mrb, mrb_int pid)
{
  if (pid <= 0 && pid != -1) {
    mrb_raise(mrb, E_NOTIMP_ERROR, "waiting for a process group is not supported");
  }
}

/*
 * call-seq:
 *   Process.waitpid(pid = -1, flags = 0) -> integer or nil
 *   Process.wait(pid = -1, flags = 0)    -> integer or nil
 *
 * Waits for a child process to finish and returns its process ID, setting
 * <code>$?</code> to the Process::Status it finished with.  A +pid+ of -1
 * waits for whichever child finishes first.
 *
 * With Process::WNOHANG among +flags+, returns nil and sets <code>$?</code>
 * to nil when no child is ready.  With Process::WUNTRACED, a stopped child
 * is reported too, where the platform has such a thing; a stopped child has
 * not finished, so it can still be waited for again.
 *
 * Raises Errno::ECHILD when +pid+ is not a child this interpreter spawned
 * and still owes a wait for -- including a child it has already reaped.
 */
static mrb_value
process_waitpid(mrb_state *mrb, mrb_value self)
{
  mrb_int pid = -1;
  mrb_int flags = 0;
  mrb_value status;

  mrb_get_args(mrb, "|ii", &pid, &flags);
  check_wait_pid(mrb, pid);

  status = (pid == -1)
             ? mrb_process_wait_any(mrb, (unsigned int)flags)
             : mrb_process_record_wait(mrb, live_record(mrb, pid), (unsigned int)flags);
  if (mrb_nil_p(status)) return mrb_nil_value();
  return mrb_int_value(mrb, mrb_process_status_ptr(mrb, status)->pid);
}

/*
 * call-seq:
 *   Process.child(pid) -> Process::Child or nil
 *
 * The child +pid+ names, as the object that owns the wait it owes, or nil
 * when this interpreter has no such live child.  Waiting through the object
 * rather than the number is what makes waiting twice harmless.
 *
 *   pid = Process.spawn("sleep 1")
 *   child = Process.child(pid)
 *   child.wait          #=> #<Process::Status: pid 1234 exit 0>
 *   child.wait          #=> the same status, and no second wait
 */
static mrb_value
process_child(mrb_state *mrb, mrb_value self)
{
  mrb_int pid;
  mrb_process_record *record;

  mrb_get_args(mrb, "i", &pid);
  record = mrb_process_record_find(mrb_process_table_get(mrb), pid);
  if (record == NULL) return mrb_nil_value();
  return mrb_process_child_new(mrb, record);
}

/*
 * call-seq:
 *   Process.detach(pid) -> nil
 *
 * Gives up the obligation to wait for the child +pid+ names.  Nothing here
 * waits for it afterwards, and on POSIX its status slot stays until the host
 * process exits.  CRuby returns a Thread that does the waiting; mruby has no
 * threads, so this returns nil.
 */
static mrb_value
process_detach(mrb_state *mrb, mrb_value self)
{
  mrb_int pid;
  mrb_process_record *record;

  mrb_get_args(mrb, "i", &pid);
  record = mrb_process_record_find(mrb_process_table_get(mrb), pid);
  if (record != NULL) mrb_process_record_detach(mrb, record);
  return mrb_nil_value();
}

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
  mrb_define_module_function_id(mrb, process, MRB_SYM(kill),    process_kill,    MRB_ARGS_REQ(1)|MRB_ARGS_REST());
  mrb_define_module_function_id(mrb, process, MRB_SYM(waitpid), process_waitpid, MRB_ARGS_OPT(2));
  mrb_define_module_function_id(mrb, process, MRB_SYM(wait),    process_waitpid, MRB_ARGS_OPT(2));
  mrb_define_module_function_id(mrb, process, MRB_SYM(child),   process_child,   MRB_ARGS_REQ(1));
  mrb_define_module_function_id(mrb, process, MRB_SYM(detach),  process_detach,  MRB_ARGS_REQ(1));

  mrb_process_status_init(mrb, process);
  mrb_process_child_init(mrb, process);
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
