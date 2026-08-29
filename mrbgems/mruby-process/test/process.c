#include <mruby.h>
#include <mruby/class.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <stdlib.h>

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
test_int64(mrb_state *mrb, const char *s, const char *what)
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
  t.sec = test_int64(mrb, sec, "sec");
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

/* ProcessSysTest.fits?(decimal, size, signed) -> true or false
 *
 * Whether the number `decimal` spells is held by an ID type `size` bytes
 * wide with that sign, which is what a port answers mrb_hal_process_id_fits()
 * from.  The host has one width and one sign for each of its two ID types,
 * and the reading has to be right for the others too, so they are asked
 * about here rather than only the host's.  The number is a String because a
 * build whose Integer is 32 bits cannot write the ones at the ends of a
 * 32-bit type, and those are the ones worth asking about.
 */
static mrb_value
test_sys_fits(mrb_state *mrb, mrb_value self)
{
  const char *decimal;
  mrb_int size;
  mrb_bool is_signed;

  mrb_get_args(mrb, "zib", &decimal, &size, &is_signed);
  if (size != 2 && size != 4 && size != 8) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "no ID type is %i bytes wide", size);
  }
  return mrb_bool_value(mrb_process_id_fits_type(test_int64(mrb, decimal, "id"),
                                                 (size_t)size, is_signed));
}

/* ProcessSysTest.port_fits?(decimal) -> true or false
 *
 * What the port answers mrb_hal_process_id_fits() with for a user ID, which
 * is the question a setter asks before it makes its call.  A test that wants
 * to know the host's shape before making a call asks this rather than making
 * the call and reading the refusal, since a number one host refuses is an
 * ID on another and the call would then be made for real.  On a port that
 * takes no ID there is no predicate and the method is not defined.
 */
#ifdef MRB_HAL_PROCESS_TAKES_ID
static mrb_value
test_sys_port_fits(mrb_state *mrb, mrb_value self)
{
  const char *decimal;

  mrb_get_args(mrb, "z", &decimal);
  return mrb_bool_value(mrb_hal_process_id_fits(MRB_PROCESS_ID_USER,
                                                test_int64(mrb, decimal, "id")));
}

/* Whether the port reads a name: which of the two answers a String gets from
   a setter is decided by a macro the tests cannot see. */
static mrb_value
test_sys_port_names(mrb_state *mrb, mrb_value self)
{
#ifdef MRB_HAL_PROCESS_HAS_ID_BY_NAME
  return mrb_true_value();
#else
  return mrb_false_value();
#endif
}
#endif

void
mrb_mruby_process_gem_test(mrb_state *mrb)
{
  struct RClass *test = mrb_define_module(mrb, "ProcessStatusTest");
  struct RClass *clock = mrb_define_module(mrb, "ProcessClockTest");
  struct RClass *sys = mrb_define_module(mrb, "ProcessSysTest");

  mrb_define_module_function(mrb, test, "build", test_status_build, MRB_ARGS_REQ(3));
  mrb_define_module_function(mrb, clock, "convert", test_clock_convert,
                             MRB_ARGS_ARG(3, 1));
  mrb_define_module_function(mrb, clock, "fits?", test_clock_fits, MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, sys, "fits?", test_sys_fits, MRB_ARGS_REQ(3));
#ifdef MRB_HAL_PROCESS_TAKES_ID
  mrb_define_module_function(mrb, sys, "port_fits?", test_sys_port_fits,
                             MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, sys, "port_names?", test_sys_port_names,
                             MRB_ARGS_NONE());
#endif
}
