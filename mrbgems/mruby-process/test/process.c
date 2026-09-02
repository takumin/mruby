#include <mruby.h>
#include <mruby/error.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

/* Read a decimal into an int64_t, refusing anything the type cannot hold. */
static int64_t
test_clock_int64(mrb_state *mrb, const char *s, const char *what)
{
  char *end;
  long long v;

  errno = 0;
  v = strtoll(s, &end, 10);
  if (*s == '\0' || *end != '\0' || errno == ERANGE) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "%s is not a number an int64_t holds: %s", what, s);
  }
  return (int64_t)v;
}

/* ProcessClockTest.convert(sec, nsec, unit, resolution = false) -> number
 *
 * The reading of `sec` seconds and `nsec` nanoseconds, answered in `unit` by
 * the very code a clock reading is answered by.  What a reading becomes at
 * the ends of an int64_t and at the ends of this build's Integer is decided
 * there, and no clock comes within centuries of either end, so the endings
 * are handed to it rather than waited for.
 *
 * `sec` is a String because a build whose Integer is 32 bits cannot write
 * the seconds this is about, and those are exactly the ones worth asking
 * about.  `nsec` is nanoseconds within one second, which every build can
 * write and which is what a port promises to report; a number outside that
 * is refused here rather than passed on, a port that broke the promise being
 * the bug in that case.
 */
static mrb_value
test_clock_convert(mrb_state *mrb, mrb_value self)
{
  const char *sec;
  mrb_int nsec;
  mrb_value unit;
  mrb_bool resolution = FALSE;
  mrb_process_clock_time t;

  mrb_get_args(mrb, "zio|b", &sec, &nsec, &unit, &resolution);
  if (nsec < 0 || nsec >= NSEC_PER_SEC) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "nsec outside one second: %i", nsec);
  }
  t.sec = test_clock_int64(mrb, sec, "sec");
  t.nsec = (int64_t)nsec;
  return mrb_process_clock_result(mrb, unit, &t, resolution);
}

/* ProcessClockTest.fits?(decimal) -> true or false
 *
 * Whether an Integer in this build holds the number `decimal` spells, so
 * that a test can say which of the two answers a reading is owed without
 * knowing how wide an mrb_int is here or whether there are bigints.  It
 * deliberately shares no line with the conversion it is used to check.
 */
static mrb_value
test_clock_fits(mrb_state *mrb, mrb_value self)
{
  const char *decimal;

  mrb_get_args(mrb, "z", &decimal);
#ifdef MRB_USE_BIGINT
  (void)decimal;
  return mrb_true_value(); /* an Integer here is as wide as it needs to be */
#else
  {
    char *end;
    long long v;

    errno = 0;
    v = strtoll(decimal, &end, 10);
    if (*decimal == '\0' || *end != '\0' || errno == ERANGE) return mrb_false_value();
    return mrb_bool_value(v >= MRB_INT_MIN && v <= MRB_INT_MAX);
  }
#endif
}

#ifndef _WIN32

/*
 * The signal state a child is spawned from, set from Ruby so that a test
 * can ask what a child inherits of it.  Nothing in mruby ignores or blocks
 * a signal, so the state an embedding process would hand over has to be
 * made here: each helper sets it, runs the block, and puts it back however
 * the block ended.
 */

struct signal_state {
  int signo;
  struct sigaction action;
  sigset_t mask;
  mrb_value blk;
};

static mrb_value
signal_state_yield(mrb_state *mrb, void *p)
{
  return mrb_yield_argv(mrb, ((struct signal_state*)p)->blk, 0, NULL);
}

/* Run the block, then put back whatever `restore` puts back, and only then
   let an exception the block raised go on. */
static mrb_value
signal_state_run(mrb_state *mrb, struct signal_state *st, void (*restore)(struct signal_state*))
{
  mrb_bool raised = FALSE;
  mrb_value result = mrb_protect_error(mrb, signal_state_yield, st, &raised);

  restore(st);
  if (raised) mrb_exc_raise(mrb, result);
  return result;
}

static void
signal_state_restore_action(struct signal_state *st)
{
  sigaction(st->signo, &st->action, NULL);
}

static void
signal_state_restore_mask(struct signal_state *st)
{
  sigprocmask(SIG_SETMASK, &st->mask, NULL);
}

/* ProcessSignalTest.ignoring(signo) { ... }
 *
 * Runs the block with `signo` ignored by this process, which is what a child
 * inherits across an exec unless the spawn undoes it.
 */
static mrb_value
test_signal_ignoring(mrb_state *mrb, mrb_value self)
{
  struct signal_state st;
  struct sigaction ignore;
  mrb_int signo;

  mrb_get_args(mrb, "i&!", &signo, &st.blk);
  st.signo = (int)signo;
  memset(&ignore, 0, sizeof(ignore));
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (sigaction(st.signo, &ignore, &st.action) == -1) mrb_sys_fail(mrb, "sigaction");
  return signal_state_run(mrb, &st, signal_state_restore_action);
}

/* ProcessSignalTest.blocking(signo) { ... }
 *
 * Runs the block with `signo` blocked in this thread, which is what a child
 * inherits across an exec unless the spawn undoes it.
 */
static mrb_value
test_signal_blocking(mrb_state *mrb, mrb_value self)
{
  struct signal_state st;
  sigset_t block;
  mrb_int signo;

  mrb_get_args(mrb, "i&!", &signo, &st.blk);
  st.signo = (int)signo;
  sigemptyset(&block);
  if (sigaddset(&block, st.signo) == -1) mrb_sys_fail(mrb, "sigaddset");
  if (sigprocmask(SIG_BLOCK, &block, &st.mask) == -1) mrb_sys_fail(mrb, "sigprocmask");
  return signal_state_run(mrb, &st, signal_state_restore_mask);
}

/* ProcessIdentityTest.uid, .gid -> Integer
 *
 * Who this process is, so that a test can ask a child to become that and
 * know the request is one an unprivileged process may make: setuid() to
 * the identity one already has is allowed to anyone.  This gem defines no
 * Process.uid yet, and a test of the spawn option should not wait on it. */
static mrb_value
test_identity_uid(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, (mrb_int)getuid());
}

static mrb_value
test_identity_gid(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, (mrb_int)getgid());
}

#endif /* !_WIN32 */

void
mrb_mruby_process_gem_test(mrb_state *mrb)
{
  struct RClass *clock = mrb_define_module(mrb, "ProcessClockTest");

  mrb_define_module_function(mrb, clock, "convert", test_clock_convert,
                             MRB_ARGS_ARG(3, 1));
  mrb_define_module_function(mrb, clock, "fits?", test_clock_fits, MRB_ARGS_REQ(1));

#ifndef _WIN32
  {
    /* Defined only where there are signals to set, so that a test asks
       whether the module is there rather than which platform this is. */
    struct RClass *sig = mrb_define_module(mrb, "ProcessSignalTest");

    mrb_define_module_function(mrb, sig, "ignoring", test_signal_ignoring,
                               MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
    mrb_define_module_function(mrb, sig, "blocking", test_signal_blocking,
                               MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
  }
  {
    struct RClass *id = mrb_define_module(mrb, "ProcessIdentityTest");

    mrb_define_module_function(mrb, id, "uid", test_identity_uid, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, id, "gid", test_identity_gid, MRB_ARGS_NONE());
  }
#endif
}
