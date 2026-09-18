#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/error.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ProcessStatusTest.build(pid, raw_status, klass) -> status
 *
 * `Process::Status.new` is undefined, as it is in CRuby, and the statuses a
 * test can have a child report are the exited ones: signalled, stopped and
 * core dumped statuses have to be written from a raw value to be examined at
 * all.  This writes one the way the gem and mruby-io do, by allocating an
 * instance and initializing it, so what the tests read is the construction
 * path that is left rather than a door held open for them.
 *
 * Both numbers are passed on as they were written, so a value #initialize
 * turns away by type or by size is turned away here just the same.
 */
static mrb_value
test_status_build(mrb_state *mrb, mrb_value self)
{
  mrb_value argv[2];
  struct RClass *klass;

  mrb_get_args(mrb, "ooc", &argv[0], &argv[1], &klass);
  return mrb_obj_new(mrb, klass, 2, argv);
}

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

#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) || defined(MRB_HAL_PROCESS_HAS_SETRLIMIT)
/* Read a decimal into a uint64_t, refusing anything the type cannot hold.
   A String, so that a build whose Integer is narrower can write every one. */
static uint64_t
test_rlimit_uint64(mrb_state *mrb, mrb_value s)
{
  const char *p = mrb_string_cstr(mrb, s);
  char *end;
  unsigned long long v;

  errno = 0;
  v = strtoull(p, &end, 10);
  if (*p < '0' || *p > '9' || *end != '\0' || errno == ERANGE) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "not a number a uint64_t holds: %v", s);
  }
  return (uint64_t)v;
}

static mrb_value
test_rlimit_decimal(mrb_state *mrb, uint64_t v)
{
  char buf[21];
  char *p = buf + sizeof(buf);

  *--p = '\0';
  do {
    *--p = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  return mrb_str_new_cstr(mrb, p);
}

static const char *const test_rlimit_kinds[] = {
  "VALUE", "INFINITY", "SAVED_CUR", "SAVED_MAX"
};

/* The spelling [widest, infinity, saved_cur, saved_max] as decimals, a saved
   limit the platform does not name being nil. */
static void
test_rlimit_spelling(mrb_state *mrb, mrb_value ary, mrb_process_rlimit_spelling *s)
{
  mrb_value v;

  if (!mrb_array_p(ary) || RARRAY_LEN(ary) != 4) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "a spelling is [widest, infinity, saved_cur, saved_max]");
  }
  s->widest = test_rlimit_uint64(mrb, RARRAY_PTR(ary)[0]);
  s->infinity = test_rlimit_uint64(mrb, RARRAY_PTR(ary)[1]);
  v = RARRAY_PTR(ary)[2];
  s->has_saved_cur = !mrb_nil_p(v);
  s->saved_cur = s->has_saved_cur ? test_rlimit_uint64(mrb, v) : 0;
  v = RARRAY_PTR(ary)[3];
  s->has_saved_max = !mrb_nil_p(v);
  s->saved_max = s->has_saved_max ? test_rlimit_uint64(mrb, v) : 0;
}

/* ProcessRlimitTest.from_platform(number, spelling) -> [kind, value]
 *
 * What the common layer makes of `number` reported by a platform that spells
 * the other kinds as `spelling` says.  `kind` is a Symbol and `value` the
 * decimal, nil for a kind other than :VALUE.  The platforms a test runs on
 * spell all three alike, so the ones that do not are written here.
 */
static mrb_value
test_rlimit_from_platform(mrb_state *mrb, mrb_value self)
{
  mrb_value number, spelling;
  mrb_process_rlimit_spelling s;
  mrb_process_rlimit_value v;

  mrb_get_args(mrb, "SA", &number, &spelling);
  test_rlimit_spelling(mrb, spelling, &s);
  mrb_process_rlimit_from_platform(&s, test_rlimit_uint64(mrb, number), &v);
  return mrb_assoc_new(mrb, mrb_symbol_value(mrb_intern_cstr(mrb, test_rlimit_kinds[v.kind])),
                       v.kind == MRB_PROCESS_RLIMIT_VALUE ? test_rlimit_decimal(mrb, v.value)
                                                          : mrb_nil_value());
}

/* ProcessRlimitTest.to_platform(kind, value, spelling) -> decimal or nil
 *
 * The number the common layer hands a platform spelling as `spelling` says
 * for a limit of `kind`, `value` being read for :VALUE alone; nil where it
 * has none to hand.
 */
static mrb_value
test_rlimit_to_platform(mrb_state *mrb, mrb_value self)
{
  mrb_sym kind;
  mrb_value value, spelling;
  mrb_process_rlimit_spelling s;
  mrb_process_rlimit_value v;
  uint64_t out;
  size_t i;

  mrb_get_args(mrb, "noA", &kind, &value, &spelling);
  test_rlimit_spelling(mrb, spelling, &s);
  for (i = 0; i < sizeof(test_rlimit_kinds) / sizeof(test_rlimit_kinds[0]); i++) {
    if (kind == mrb_intern_cstr(mrb, test_rlimit_kinds[i])) break;
  }
  if (i == sizeof(test_rlimit_kinds) / sizeof(test_rlimit_kinds[0])) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "no such kind: %n", kind);
  }
  v.kind = (mrb_process_rlimit_kind)i;
  v.value = v.kind == MRB_PROCESS_RLIMIT_VALUE ? test_rlimit_uint64(mrb, value) : 0;
  if (mrb_process_rlimit_to_platform(&s, &v, &out) != 0) return mrb_nil_value();
  return test_rlimit_decimal(mrb, out);
}
#endif

#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) && defined(HAVE_GETRLIMIT) && defined(HAVE_SETRLIMIT)
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The calls the POSIX port reads and writes through, as mrbgem.rake had it
   choose them.  A resource is named apart from the port below; the width a
   limit is read in is not, since the narrower calls report a limit past
   their type as no limit and a hard limit written back as that is a raise. */
#if !defined(HAVE_WIDE_RLIM_T) && defined(HAVE_GETRLIMIT64) && defined(HAVE_SETRLIMIT64)
typedef rlim64_t test_rlim_t;
# define test_rlimit      rlimit64
# define test_getrlimit   getrlimit64
# define test_setrlimit   setrlimit64
# define TEST_RLIM_INFINITY RLIM64_INFINITY
#else
typedef rlim_t test_rlim_t;
# define test_rlimit      rlimit
# define test_getrlimit   getrlimit
# define test_setrlimit   setrlimit
# define TEST_RLIM_INFINITY RLIM_INFINITY
#endif

/* Every resource by name and by this host's own number, written apart from
   the port's table so that the two can be compared. */
static const struct {
  const char *name;
  int number;
} test_rlimit_natives[] = {
#ifdef RLIMIT_AS
  { "AS", RLIMIT_AS },
#endif
#ifdef RLIMIT_CORE
  { "CORE", RLIMIT_CORE },
#endif
#ifdef RLIMIT_CPU
  { "CPU", RLIMIT_CPU },
#endif
#ifdef RLIMIT_DATA
  { "DATA", RLIMIT_DATA },
#endif
#ifdef RLIMIT_FSIZE
  { "FSIZE", RLIMIT_FSIZE },
#endif
#ifdef RLIMIT_MEMLOCK
  { "MEMLOCK", RLIMIT_MEMLOCK },
#endif
#ifdef RLIMIT_MSGQUEUE
  { "MSGQUEUE", RLIMIT_MSGQUEUE },
#endif
#ifdef RLIMIT_NICE
  { "NICE", RLIMIT_NICE },
#endif
#ifdef RLIMIT_NOFILE
  { "NOFILE", RLIMIT_NOFILE },
#endif
#ifdef RLIMIT_NPROC
  { "NPROC", RLIMIT_NPROC },
#endif
#ifdef RLIMIT_NPTS
  { "NPTS", RLIMIT_NPTS },
#endif
#ifdef RLIMIT_RSS
  { "RSS", RLIMIT_RSS },
#endif
#ifdef RLIMIT_RTPRIO
  { "RTPRIO", RLIMIT_RTPRIO },
#endif
#ifdef RLIMIT_RTTIME
  { "RTTIME", RLIMIT_RTTIME },
#endif
#ifdef RLIMIT_SBSIZE
  { "SBSIZE", RLIMIT_SBSIZE },
#endif
#ifdef RLIMIT_SIGPENDING
  { "SIGPENDING", RLIMIT_SIGPENDING },
#endif
#ifdef RLIMIT_STACK
  { "STACK", RLIMIT_STACK },
#endif
};

#define TEST_RLIMIT_NATIVE_COUNT (sizeof(test_rlimit_natives) / sizeof(test_rlimit_natives[0]))

/* Whether getrlimit(2) reports `v` as the soft limit of a native resource
   other than the one at `index`. */
static mrb_bool
test_rlimit_other_soft_is(size_t index, test_rlim_t v)
{
  struct test_rlimit rlim;
  size_t other;

  for (other = 0; other < TEST_RLIMIT_NATIVE_COUNT; other++) {
    if (other != index && test_getrlimit(test_rlimit_natives[other].number, &rlim) == 0 &&
        rlim.rlim_cur == v) {
      return TRUE;
    }
  }
  return FALSE;
}

/* What a child comparing one resource tells its parent, as its exit status. */
enum test_rlimit_verdict {
  TEST_RLIMIT_SAME = 0,
  TEST_RLIMIT_DIFFERENT = 1,
  TEST_RLIMIT_UNCOMPARED = 2,
  TEST_RLIMIT_UNREAD = 3
};

/* The kind and number the HAL carries a native limit as. */
static void
test_rlimit_value(test_rlim_t native, mrb_process_rlimit_value *v)
{
  if (native == TEST_RLIM_INFINITY) {
    v->kind = MRB_PROCESS_RLIMIT_INFINITY;
    v->value = 0;
  }
  else {
    v->kind = MRB_PROCESS_RLIMIT_VALUE;
    v->value = (uint64_t)native;
  }
}

/* Whether the HAL reads both limits of `native` for the resource `id`
   numbers. */
static mrb_bool
test_rlimit_hal_reads(mrb_state *mrb, mrb_int id, const struct test_rlimit *native)
{
  mrb_process_rlimit r;
  mrb_process_rlimit_value cur, max;

  test_rlimit_value(native->rlim_cur, &cur);
  test_rlimit_value(native->rlim_max, &max);
  return mrb_hal_process_getrlimit(mrb, id, &r) == 0 &&
         r.cur.kind == cur.kind && r.cur.value == cur.value &&
         r.max.kind == max.kind && r.max.value == max.value;
}

/* A soft limit under the hard limit `max` that no other resource's soft
   limit is at, looked for below the one there was and then above it; one
   other than `old` unless the hard limit is moving. */
static mrb_bool
test_rlimit_soft_for(size_t index, const struct test_rlimit *old, test_rlim_t max,
                     test_rlim_t *cur)
{
  test_rlim_t start = old->rlim_cur, v;
  mrb_bool moving = max != old->rlim_max;

  if (start == TEST_RLIM_INFINITY || start >= max) start = max - 1;
  for (v = start; ; v--) {
    if ((moving || v != old->rlim_cur) && !test_rlimit_other_soft_is(index, v)) {
      *cur = v;
      return TRUE;
    }
    if (v == 0) break;
  }
  for (v = start + 1; v < max; v++) {
    if ((moving || v != old->rlim_cur) && !test_rlimit_other_soft_is(index, v)) {
      *cur = v;
      return TRUE;
    }
  }
  return FALSE;
}

/* The comparison native_is? makes, run in a child whose limits nothing reads
   afterwards, so that both limits can move and neither has to be put back.
   Through setrlimit(2) itself the hard limit comes down below the one there
   was, and the soft limit to a number under it that no other resource's soft
   limit is at; the HAL then reads both.  Where the port writes limits, it
   moves them again for getrlimit(2) to read.  A HAL that numbers another
   resource under the id reads or writes that resource instead, whose soft
   limit is elsewhere and whose hard limit did not move.

   What the platform holds after the write is what the HAL is compared with,
   rather than what was asked for: macOS cuts a hard `RLIMIT_NPROC` above
   `kern.maxprocperuid` down to it (`dosetrlimit()` in xnu), and that limit
   has still moved.  A platform that refuses to lower a hard limit from no
   limit to a number it will not hold has the soft limit moved alone. */
static enum test_rlimit_verdict
test_rlimit_compare(mrb_state *mrb, mrb_int id, size_t index)
{
  int number = test_rlimit_natives[index].number;
  struct test_rlimit old, probe, moved;

  if (test_getrlimit(number, &old) != 0) return TEST_RLIMIT_UNREAD;
  if (old.rlim_max == 0) return TEST_RLIMIT_UNCOMPARED;
  if (old.rlim_max == TEST_RLIM_INFINITY) {
    probe.rlim_max = TEST_RLIM_INFINITY / 2;
  }
  else {
    probe.rlim_max = old.rlim_max > 1 ? old.rlim_max - 1 : old.rlim_max;
  }
  if (!test_rlimit_soft_for(index, &old, probe.rlim_max, &probe.rlim_cur)) {
    return TEST_RLIMIT_UNCOMPARED;
  }
  if (test_setrlimit(number, &probe) != 0) {
    probe.rlim_max = old.rlim_max;
    if (!test_rlimit_soft_for(index, &old, probe.rlim_max, &probe.rlim_cur) ||
        test_setrlimit(number, &probe) != 0) {
      return TEST_RLIMIT_UNCOMPARED;
    }
  }
  if (test_getrlimit(number, &moved) != 0) return TEST_RLIMIT_UNREAD;
  /* qemu-user answers success for `RLIMIT_AS`, `RLIMIT_DATA` and
     `RLIMIT_STACK`, the limits it would itself be held to, without moving
     them. */
  if (moved.rlim_cur == old.rlim_cur && moved.rlim_max == old.rlim_max) {
    return TEST_RLIMIT_UNCOMPARED;
  }
  if (moved.rlim_cur != probe.rlim_cur && test_rlimit_other_soft_is(index, moved.rlim_cur)) {
    return TEST_RLIMIT_UNCOMPARED;
  }
  if (!test_rlimit_hal_reads(mrb, id, &moved)) return TEST_RLIMIT_DIFFERENT;

#ifdef MRB_HAL_PROCESS_HAS_SETRLIMIT
  {
    struct test_rlimit next, written;
    mrb_process_rlimit r;

    next = moved;
    if (next.rlim_max != TEST_RLIM_INFINITY && next.rlim_max > 1) next.rlim_max--;
    if (next.rlim_cur > next.rlim_max) next.rlim_cur = next.rlim_max;
    if (next.rlim_cur == moved.rlim_cur) {
      if (next.rlim_cur > 0) {
        next.rlim_cur--;
      }
      else if (next.rlim_max == TEST_RLIM_INFINITY || next.rlim_max > 0) {
        next.rlim_cur = 1;
      }
    }
    if (next.rlim_cur == moved.rlim_cur) return TEST_RLIMIT_UNCOMPARED;
    test_rlimit_value(next.rlim_cur, &r.cur);
    test_rlimit_value(next.rlim_max, &r.max);
    if (mrb_hal_process_setrlimit(mrb, id, &r) != 0) return TEST_RLIMIT_DIFFERENT;
    if (test_getrlimit(number, &written) != 0) return TEST_RLIMIT_UNREAD;
    if (written.rlim_cur != next.rlim_cur || written.rlim_max != next.rlim_max) {
      return TEST_RLIMIT_DIFFERENT;
    }
  }
#endif
  return TEST_RLIMIT_SAME;
}

/* Wait for the child `pid` and answer the status it exited with, raising
   where it ended any other way. */
static int
test_rlimit_reap(mrb_state *mrb, pid_t pid)
{
  int status;

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) mrb_sys_fail(mrb, "waitpid");
  }
  if (!WIFEXITED(status)) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "child %d did not exit: raw status %d", (int)pid, status);
  }
  return WEXITSTATUS(status);
}

/* ProcessRlimitTest.native_is?(id, name) -> true, false or nil
 *
 * Whether the resource `id` numbers through the HAL is the one this host's
 * <sys/resource.h> calls RLIMIT_`name`, for reading both limits and, where
 * the port writes limits, for writing them.  The limits are moved in a child,
 * since a hard limit lowered cannot be raised again by a process that is not
 * privileged.  nil where the host has no RLIMIT_`name`, where a limit cannot
 * be moved, a hard limit of 0 being the usual case, where the platform
 * answers success without moving it, and where no child can be made.
 */
static mrb_value
test_rlimit_native_is(mrb_state *mrb, mrb_value self)
{
  mrb_int id;
  const char *name;
  size_t index;
  pid_t pid;

  mrb_get_args(mrb, "iz", &id, &name);
  for (index = 0; index < TEST_RLIMIT_NATIVE_COUNT; index++) {
    if (strcmp(test_rlimit_natives[index].name, name) == 0) break;
  }
  if (index == TEST_RLIMIT_NATIVE_COUNT) return mrb_nil_value();

  pid = fork();
  if (pid < 0) return mrb_nil_value();
  if (pid == 0) _exit(test_rlimit_compare(mrb, id, index));
  switch (test_rlimit_reap(mrb, pid)) {
    case TEST_RLIMIT_SAME:
      return mrb_true_value();
    case TEST_RLIMIT_DIFFERENT:
      return mrb_false_value();
    case TEST_RLIMIT_UNCOMPARED:
      return mrb_nil_value();
    default:
      mrb_raisef(mrb, E_RUNTIME_ERROR, "getrlimit(2) failed on RLIMIT_%s", name);
      return mrb_nil_value(); /* not reached */
  }
}

/* ProcessRlimitTest.__isolate -> [pid, fd] or nil
 *
 * fork(2) for ProcessTestUtil.isolated.  Answers `[0, fd]` in the child, `fd`
 * being where it writes its report, and `[pid, fd]` in the parent, `fd` being
 * where the report is read; nil where no child can be made.
 */
static mrb_value
test_rlimit_isolate(mrb_state *mrb, mrb_value self)
{
  int fds[2];
  pid_t pid;

  if (pipe(fds) != 0) return mrb_nil_value();
  pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return mrb_nil_value();
  }
  if (pid == 0) {
    close(fds[0]);
    return mrb_assoc_new(mrb, mrb_fixnum_value(0), mrb_fixnum_value(fds[1]));
  }
  close(fds[1]);
  return mrb_assoc_new(mrb, mrb_fixnum_value(pid), mrb_fixnum_value(fds[0]));
}

/* ProcessRlimitTest.__report(fd, report) -> does not return
 *
 * Write `report` for the parent and end the child.  _exit(2) rather than
 * exit(3), so that output the parent had buffered is not written a second
 * time and nothing the parent set up to run at exit runs here.  Nothing is
 * raised, whatever the arguments: a child that returned from here would go
 * on to run the rest of the suite.
 */
static mrb_value
test_rlimit_report(mrb_state *mrb, mrb_value self)
{
  mrb_value fd, report;
  const char *p;
  mrb_int len;

  mrb_get_args(mrb, "oo", &fd, &report);
  if (!mrb_integer_p(fd) || !mrb_string_p(report)) _exit(1);
  p = RSTRING_PTR(report);
  len = RSTRING_LEN(report);
  while (len > 0) {
    ssize_t n = write((int)mrb_integer(fd), p, (size_t)len);

    if (n < 0) {
      if (errno == EINTR) continue;
      _exit(1);
    }
    p += n;
    len -= n;
  }
  _exit(0);
  return mrb_nil_value(); /* not reached */
}

/* ProcessRlimitTest.__collect(pid, fd) -> report
 *
 * Read what the child `pid` wrote to `fd` and wait for it.  A child that
 * ended without exiting, or without writing all of its report, raises.
 */
static mrb_value
test_rlimit_collect(mrb_state *mrb, mrb_value self)
{
  mrb_int pid, fd;
  mrb_value report;
  char buf[256];
  ssize_t n;
  int err = 0;

  mrb_get_args(mrb, "ii", &pid, &fd);
  report = mrb_str_new(mrb, NULL, 0);
  for (;;) {
    n = read((int)fd, buf, sizeof(buf));
    if (n > 0) {
      mrb_str_cat(mrb, report, buf, (size_t)n);
    }
    else if (n == 0) {
      break;
    }
    else if (errno != EINTR) {
      err = errno;
      break;
    }
  }
  close((int)fd);
  if (test_rlimit_reap(mrb, (pid_t)pid) != 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "child %i could not write its report", pid);
  }
  if (err != 0) {
    errno = err;
    mrb_sys_fail(mrb, "read");
  }
  return report;
}

/* ProcessRlimitTest.privileged? -> true or false
 *
 * Whether this process runs as root, which may raise a hard limit it lowered.
 * Asked apart from any limit call, so that a call failing to refuse a raise
 * is not taken for privilege.
 */
static mrb_value
test_rlimit_privileged(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(geteuid() == 0);
}

/* ProcessRlimitTest.native_infinity -> decimal
 *
 * The number the POSIX port hands setrlimit(2) for no limit, as a program
 * could write it.  It is the wider calls' where the port takes them: the
 * narrower RLIM_INFINITY is a count to those, and a hard limit set to it
 * could not be raised again.
 */
static mrb_value
test_rlimit_native_infinity(mrb_state *mrb, mrb_value self)
{
  return test_rlimit_decimal(mrb, (uint64_t)TEST_RLIM_INFINITY);
}
#endif

void
mrb_mruby_process_gem_test(mrb_state *mrb)
{
  struct RClass *test = mrb_define_module(mrb, "ProcessStatusTest");
  struct RClass *clock = mrb_define_module(mrb, "ProcessClockTest");
#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) || defined(MRB_HAL_PROCESS_HAS_SETRLIMIT)
  struct RClass *rlimit = mrb_define_module(mrb, "ProcessRlimitTest");
#endif

  mrb_define_module_function(mrb, test, "build", test_status_build, MRB_ARGS_REQ(3));
  mrb_define_module_function(mrb, clock, "convert", test_clock_convert,
                             MRB_ARGS_ARG(3, 1));
  mrb_define_module_function(mrb, clock, "fits?", test_clock_fits, MRB_ARGS_REQ(1));
#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) || defined(MRB_HAL_PROCESS_HAS_SETRLIMIT)
  mrb_define_module_function(mrb, rlimit, "from_platform", test_rlimit_from_platform,
                             MRB_ARGS_REQ(2));
  mrb_define_module_function(mrb, rlimit, "to_platform", test_rlimit_to_platform,
                             MRB_ARGS_REQ(3));
#endif
#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) && defined(HAVE_GETRLIMIT) && defined(HAVE_SETRLIMIT)
  mrb_define_module_function(mrb, rlimit, "native_is?", test_rlimit_native_is,
                             MRB_ARGS_REQ(2));
  mrb_define_module_function(mrb, rlimit, "native_infinity", test_rlimit_native_infinity,
                             MRB_ARGS_NONE());
  mrb_define_module_function(mrb, rlimit, "__isolate", test_rlimit_isolate, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, rlimit, "__report", test_rlimit_report, MRB_ARGS_REQ(2));
  mrb_define_module_function(mrb, rlimit, "__collect", test_rlimit_collect, MRB_ARGS_REQ(2));
  mrb_define_module_function(mrb, rlimit, "privileged?", test_rlimit_privileged,
                             MRB_ARGS_NONE());
#endif
}
