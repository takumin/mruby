/*
** rlimit.c - Process.getrlimit and Process.setrlimit
**
** See Copyright Notice in mruby.h
**
** The resource limits half of mruby-process.  A limit crosses the HAL as a
** pair of int64_t and a resource as one of mruby's own numbers, and
** everything Ruby says about them is said here: which resources can be
** named, how a name is read, which numbers name no limit, and which of the
** `Process::RLIMIT_*` constants this build defines.  A port answers what its
** platform has and is asked nothing about any of that.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/error.h>
#include <mruby/numeric.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <stdint.h>

/* The answers a limit can be instead of a number, which is what the kinds the
   HAL carries stand for: no limit, and the two POSIX saved limits a platform
   answers where its own type is too narrow to report the limit it holds.

   The numbers are mruby's own, as the resource ids are.  A limit is a count
   of something and is never negative, which leaves the negative numbers free
   to say what kind of answer this is, and there is no one platform number to
   borrow in any case: no limit is `((rlim_t)-1)` on Linux, 2**63 - 1 on the
   BSDs and macOS and `((rlim_t)-3)` on illumos, and the first of those is past
   `mrb_int` in every build.  CRuby answers with the platform's numbers
   instead, and so defines `RLIM_SAVED_MAX` as `RLIM_INFINITY` where a host
   spells the two the same; here the three are distinct whatever the host does,
   and a host that cannot tell them apart simply never answers with a saved
   one. */
static const struct rlimit_answer {
  mrb_int number;
  mrb_sym name;
  mrb_sym cname;
  mrb_process_rlimit_kind kind;
} rlimit_answers[] = {
  { -1, MRB_SYM(INFINITY),  MRB_SYM(RLIM_INFINITY),  MRB_PROCESS_RLIMIT_INFINITY  },
  { -2, MRB_SYM(SAVED_CUR), MRB_SYM(RLIM_SAVED_CUR), MRB_PROCESS_RLIMIT_SAVED_CUR },
  { -3, MRB_SYM(SAVED_MAX), MRB_SYM(RLIM_SAVED_MAX), MRB_PROCESS_RLIMIT_SAVED_MAX },
};

#define RLIMIT_ANSWER_COUNT (sizeof(rlimit_answers) / sizeof(rlimit_answers[0]))

#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) || defined(MRB_HAL_PROCESS_HAS_SETRLIMIT)

/* Every resource, in the order the ids are declared in: what a name argument
   spells it, and what the constant standing for it is called.  The names are
   CRuby's, the POSIX ones without the `RLIMIT_` prefix, so that
   `Process.getrlimit(:NOFILE)` reads what `Process::RLIMIT_NOFILE` numbers
   and a program that names its resource rather than numbering it reads the
   same on both. */
static const struct rlimit_resource {
  mrb_sym name;
  mrb_sym cname;
} rlimit_resources[] = {
  { MRB_SYM(AS),         MRB_SYM(RLIMIT_AS)         },
  { MRB_SYM(CORE),       MRB_SYM(RLIMIT_CORE)       },
  { MRB_SYM(CPU),        MRB_SYM(RLIMIT_CPU)        },
  { MRB_SYM(DATA),       MRB_SYM(RLIMIT_DATA)       },
  { MRB_SYM(FSIZE),      MRB_SYM(RLIMIT_FSIZE)      },
  { MRB_SYM(MEMLOCK),    MRB_SYM(RLIMIT_MEMLOCK)    },
  { MRB_SYM(MSGQUEUE),   MRB_SYM(RLIMIT_MSGQUEUE)   },
  { MRB_SYM(NICE),       MRB_SYM(RLIMIT_NICE)       },
  { MRB_SYM(NOFILE),     MRB_SYM(RLIMIT_NOFILE)     },
  { MRB_SYM(NPROC),      MRB_SYM(RLIMIT_NPROC)      },
  { MRB_SYM(NPTS),       MRB_SYM(RLIMIT_NPTS)       },
  { MRB_SYM(RSS),        MRB_SYM(RLIMIT_RSS)        },
  { MRB_SYM(RTPRIO),     MRB_SYM(RLIMIT_RTPRIO)     },
  { MRB_SYM(RTTIME),     MRB_SYM(RLIMIT_RTTIME)     },
  { MRB_SYM(SBSIZE),     MRB_SYM(RLIMIT_SBSIZE)     },
  { MRB_SYM(SIGPENDING), MRB_SYM(RLIMIT_SIGPENDING) },
  { MRB_SYM(STACK),      MRB_SYM(RLIMIT_STACK)      },
};

mrb_static_assert(sizeof(rlimit_resources) / sizeof(rlimit_resources[0]) ==
                  (size_t)MRB_PROCESS_RLIMIT_COUNT,
                  "every resource id needs a name and a constant");

/*
 * The Symbol a name argument spells, or 0 for an argument that is not a name.
 *
 * A String names what the Symbol of the same spelling names, as it does in
 * CRuby.  It is looked up rather than interned: a name no resource has would
 * otherwise leave a Symbol in the table for the rest of the program, put
 * there by the call that was turned down for it.  Only a plain type check
 * either way, since mruby dispatches nothing to convert an argument.
 */
static mrb_sym
rlimit_name(mrb_state *mrb, mrb_value v)
{
  if (mrb_symbol_p(v)) return mrb_symbol(v);
  if (mrb_string_p(v)) return mrb_intern_check_str(mrb, v);
  return 0;
}

/*
 * Read a resource argument into one of mruby's own resource numbers.
 *
 * A name that picks out none of the seventeen is an ArgumentError, as it is
 * in CRuby: nothing was asked of the system, so there is no errno for it to
 * have failed with.  A number outside the list is refused before a port sees
 * it, as an unknown clock id is and for the same reason, with the EINVAL a
 * platform answers for a resource it does not have.  A resource that is on
 * the list and not on this host arrives at that errno too, from the port.
 */
static mrb_int
rlimit_resource_arg(mrb_state *mrb, mrb_value resource, const char *what)
{
  mrb_int id;

  if (mrb_symbol_p(resource) || mrb_string_p(resource)) {
    mrb_sym name = rlimit_name(mrb, resource);

    /* A String no Symbol was ever made of answers 0, which names nothing. */
    if (name != 0) {
      for (id = 0; id < (mrb_int)MRB_PROCESS_RLIMIT_COUNT; id++) {
        if (rlimit_resources[id].name == name) return id;
      }
    }
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid resource name: %!v", resource);
  }
  id = mrb_as_int(mrb, resource);
  if (id < 0 || id >= (mrb_int)MRB_PROCESS_RLIMIT_COUNT) {
    errno = EINVAL;
    mrb_sys_fail(mrb, what);
  }
  return id;
}

#ifdef MRB_HAL_PROCESS_HAS_SETRLIMIT
/*
 * Read a limit argument.
 *
 * A limit is a number, one of the three constants above, or the name of one
 * as a Symbol or a String, which is how CRuby takes `:INFINITY`, `:SAVED_CUR`
 * and `:SAVED_MAX` too.
 *
 * A negative number that names none of them is refused with RangeError, where
 * the size of a number can be said, rather than passed to a port that would
 * have to narrow it into an unsigned `rlim_t` and could only report what came
 * of it through an errno; mrb_process_int_arg() refuses an oversized pid the
 * same way.  CRuby lets one through as the unsigned number of the same bits,
 * so `Process.setrlimit(:CORE, -4)` sets a limit of 18446744073709551612
 * where `rlim_t` is 64 bits wide.
 */
static void
rlimit_limit_arg(mrb_state *mrb, mrb_value v, mrb_process_rlimit_value *out)
{
  size_t i;

  out->value = 0;
  if (mrb_symbol_p(v) || mrb_string_p(v)) {
    mrb_sym name = rlimit_name(mrb, v);

    if (name != 0) {
      for (i = 0; i < RLIMIT_ANSWER_COUNT; i++) {
        if (rlimit_answers[i].name == name) {
          out->kind = rlimit_answers[i].kind;
          return;
        }
      }
    }
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid resource value: %!v", v);
  }

  v = mrb_ensure_integer_type(mrb, v);
  /* Only a number small enough to be a Fixnum can be one of the three, so a
     bigint is a limit or nothing, and negative it is nothing either way. */
  if (mrb_integer_p(v) && mrb_integer(v) < 0) {
    for (i = 0; i < RLIMIT_ANSWER_COUNT; i++) {
      if (rlimit_answers[i].number == mrb_integer(v)) {
        out->kind = rlimit_answers[i].kind;
        return;
      }
    }
    mrb_raisef(mrb, E_RANGE_ERROR, "resource limit out of range: %v", v);
  }
  out->kind = MRB_PROCESS_RLIMIT_VALUE;
  out->value = mrb_as_uint64(mrb, v);
}
#endif

#ifdef MRB_HAL_PROCESS_HAS_GETRLIMIT
/*
 * The Integer a limit is answered as.
 *
 * A number is one, which is a bigint where this build's Integer is narrower
 * than the platform counted in and a RangeError where it has no bigints; the
 * other kinds are the constants above, which every build can hold.
 */
static mrb_value
rlimit_result(mrb_state *mrb, const mrb_process_rlimit_value *v)
{
  size_t i;

  for (i = 0; i < RLIMIT_ANSWER_COUNT; i++) {
    if (rlimit_answers[i].kind == v->kind) return mrb_fixnum_value(rlimit_answers[i].number);
  }
  return mrb_uint64_value(mrb, v->value);
}

/*
 * call-seq:
 *   Process.getrlimit(resource) -> [cur_limit, max_limit]
 *
 * The soft and the hard limit on +resource+: what the system enforces now,
 * and the ceiling the soft limit may be raised to.  Either is
 * Process::RLIM_INFINITY for a resource that is not limited, and
 * Process::RLIM_SAVED_CUR or Process::RLIM_SAVED_MAX where the platform
 * holds a limit it has no way to report.
 *
 *   Process.getrlimit(Process::RLIMIT_NOFILE)  #=> [1024, 524288]
 *   Process.getrlimit(:CORE)                   #=> [0, Process::RLIM_INFINITY]
 *
 * The resource is one of the Process::RLIMIT_* constants, or its name
 * without the prefix as a Symbol or a String.  Which resources there are is
 * the platform's: a constant is defined for each one this build has, so a
 * program asks <code>defined?(Process::RLIMIT_NPTS)</code> rather than
 * calling to find out.
 *
 * Raises ArgumentError for a name no resource has, Errno::EINVAL for a
 * number that names none on this platform, and RangeError where a limit is
 * larger than this build's Integer can hold.
 */
static mrb_value
process_getrlimit(mrb_state *mrb, mrb_value self)
{
  mrb_value resource;
  mrb_process_rlimit r;
  mrb_int id;

  mrb_get_args(mrb, "o", &resource);
  id = rlimit_resource_arg(mrb, resource, "getrlimit");

  if (mrb_hal_process_getrlimit(mrb, id, &r) != 0) {
    mrb_sys_fail(mrb, "getrlimit");
  }
  return mrb_assoc_new(mrb, rlimit_result(mrb, &r.cur), rlimit_result(mrb, &r.max));
}
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETRLIMIT
/*
 * call-seq:
 *   Process.setrlimit(resource, cur_limit, max_limit = cur_limit) -> nil
 *
 * Sets the soft and the hard limit on +resource+.  Both are written, as
 * setrlimit(2) writes both, so a call that means to leave one of them alone
 * reads it first; with +max_limit+ left out, or given as nil, the hard limit
 * is set to +cur_limit+ as well.
 *
 *   Process.setrlimit(:CORE, 0)                              # no core files
 *   Process.setrlimit(:CORE, Process.getrlimit(:CORE)[1])    # as large as allowed
 *   Process.setrlimit(:CPU, 10, Process::RLIM_INFINITY)
 *
 * A limit is a number of whatever the resource is counted in, or one of
 * Process::RLIM_INFINITY, Process::RLIM_SAVED_CUR and
 * Process::RLIM_SAVED_MAX, which +:INFINITY+, +:SAVED_CUR+ and +:SAVED_MAX+
 * also name.  The resource is named as Process.getrlimit names it.
 *
 * Raises Errno::EPERM where the process may not set the limit it asked for,
 * a hard limit above the one it has being the usual reason, and
 * Errno::EINVAL for a soft limit above the hard one.  A negative limit other
 * than Process::RLIM_INFINITY raises RangeError.
 */
static mrb_value
process_setrlimit(mrb_state *mrb, mrb_value self)
{
  mrb_value resource, cur, max = mrb_nil_value();
  mrb_process_rlimit r;
  mrb_int id;

  mrb_get_args(mrb, "oo|o", &resource, &cur, &max);
  id = rlimit_resource_arg(mrb, resource, "setrlimit");
  rlimit_limit_arg(mrb, cur, &r.cur);
  if (mrb_nil_p(max)) {
    r.max = r.cur;
  }
  else {
    rlimit_limit_arg(mrb, max, &r.max);
  }

  if (mrb_hal_process_setrlimit(mrb, id, &r) != 0) {
    mrb_sys_fail(mrb, "setrlimit");
  }
  return mrb_nil_value();
}
#endif

#endif /* a port with either call */

void
mrb_process_rlimit_init(mrb_state *mrb, struct RClass *process)
{
  {
    /* The answers that are not numbers are the shape of the call, as the wait
       flags are, and are defined whether or not this port limits anything.
       mruby's own values rather than the platform's, for the reason given
       where they are listed. */
    size_t i;

    for (i = 0; i < RLIMIT_ANSWER_COUNT; i++) {
      mrb_define_const_id(mrb, process, rlimit_answers[i].cname,
                          mrb_fixnum_value(rlimit_answers[i].number));
    }
  }

#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) || defined(MRB_HAL_PROCESS_HAS_SETRLIMIT)
  {
    /* A constant for each resource this port has and none for the rest, as
       CRuby defines them, so that `defined?(Process::RLIMIT_RTTIME)` is the
       question a portable program already asks.  The clocks are defined
       everywhere instead, since every port answers for all four; half of
       these are one operating system's alone. */
    uint32_t ids = mrb_hal_process_rlimit_ids(mrb);
    mrb_int id;

    for (id = 0; id < (mrb_int)MRB_PROCESS_RLIMIT_COUNT; id++) {
      if (ids & ((uint32_t)1 << id)) {
        mrb_define_const_id(mrb, process, rlimit_resources[id].cname,
                            mrb_fixnum_value(id));
      }
    }
  }
#endif

#ifdef MRB_HAL_PROCESS_HAS_GETRLIMIT
  mrb_define_module_function_id(mrb, process, MRB_SYM(getrlimit), process_getrlimit,
                                MRB_ARGS_REQ(1));
#else
  /* Every argument shape, rather than the one the method would have had: what
     is wrong with the call is that this build does not have it, and mruby
     checks the shape before the body runs, so keeping the real one would
     answer a call written with the wrong count by complaining about the
     count.  CRuby's unimplemented methods take any number of arguments for
     the same reason. */
  mrb_define_module_function_id(mrb, process, MRB_SYM(getrlimit), mrb_notimplement_m,
                                MRB_ARGS_ANY());
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETRLIMIT
  mrb_define_module_function_id(mrb, process, MRB_SYM(setrlimit), process_setrlimit,
                                MRB_ARGS_ARG(2, 1));
#else
  mrb_define_module_function_id(mrb, process, MRB_SYM(setrlimit), mrb_notimplement_m,
                                MRB_ARGS_ANY());
#endif
}
