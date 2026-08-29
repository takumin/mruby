/*
** sys.c - Process::Sys
**
** See Copyright Notice in mruby.h
**
** The user and group IDs, as the system calls themselves present them.  Every
** method here is one call: it does what that call does, returns what that call
** returns, and reports what that call reported.  Nothing is retried, softened
** or arranged into a friendlier shape; that is what `Process.uid=` and the
** rest of the `Process` spellings are for, and they are a separate thing.
**
** Which of these calls a machine has is not the same question as whether a
** call it has was allowed to run.  The first is the port's to answer, through
** the MRB_HAL_PROCESS_HAS_* macros of its process_hal_features.h, and a
** method standing for a call the port does not declare is defined as
** unimplemented: `respond_to?` reports it as missing and calling it raises
** NotImplementedError.  The second is `errno`, and reaches Ruby as an
** `Errno::*`, including the ENOSYS a seccomp filter answers with, which is a
** refusal by the running kernel and not a call the port lacks.
*/

#include <mruby.h>
#include <mruby/error.h>
#include <mruby/numeric.h>
#include <mruby/string.h>
#ifdef MRB_USE_BIGINT
#include <mruby/internal.h>
#endif
#include <errno.h>
#include <stdint.h>
#include "process_hal.h"
#include "process_internal.h"

#ifdef MRB_HAL_PROCESS_TAKES_ID

/*
 * Read an ID given as a number.
 *
 * An ID crosses the HAL as an int64_t, and which numbers name one is the
 * port's to say, since the width of a `uid_t` is the platform's and POSIX
 * fixes none.  So the port is asked, and a number it does not know is
 * refused here as a RangeError, where size can be said, rather than left to
 * an errno that has no spelling for it; mrb_process_int_arg() makes the same
 * argument for a pid.  A build whose Integer is narrower than an int64_t may
 * hold such a number as a bigint, and that is read the same way; one past
 * int64_t itself is past every ID the HAL carries (process_hal.h says where
 * that bound comes from), and the bigint conversion refuses it.
 */
static int64_t
id_number(mrb_state *mrb, mrb_value v, mrb_process_id_kind kind)
{
  int64_t id;

  v = mrb_ensure_integer_type(mrb, v);
#ifdef MRB_USE_BIGINT
  if (mrb_bigint_p(v)) {
    id = mrb_bint_as_int64(mrb, v);
  }
  else
#endif
  {
    id = (int64_t)mrb_integer(v);
  }
  if (!mrb_hal_process_id_fits(kind, id)) {
    mrb_raisef(mrb, E_RANGE_ERROR, "%s out of range: %v",
               (kind == MRB_PROCESS_ID_USER) ? "uid" : "gid", v);
  }
  return id;
}

/*
 * Read an ID argument.
 *
 * Ruby takes the number itself, or the name of a user or group as a String.
 * A name the table answered it has no record of is an ArgumentError rather
 * than a failed call: the lookup never reaches the call the method stands
 * for, so there is no errno for it to have failed with.  A lookup that
 * failed before it could answer is a different thing, and is reported as
 * the SystemCallError it was, naming the account it was looking for; which
 * of the two an unknown name is depends on how the port's account table
 * answers, and CRuby's answer splits the same way (the POSIX port says
 * where).  A port declares the user table and the group table one at a time
 * (MRB_HAL_PROCESS_HAS_UID_BY_NAME, MRB_HAL_PROCESS_HAS_GID_BY_NAME), and
 * where it declared neither, or not the one this ID is read from, a String
 * is what anything but an Integer is, a TypeError, as CRuby built without
 * <pwd.h> answers.
 */
#ifdef MRB_HAL_PROCESS_NEEDS_ID_BY_NAME
/* Whether a name stands for this kind of ID on this port.  Written as three
   whole bodies rather than one that tests both macros, so that the usual
   port, the one with both tables, asks nothing at run time: a `switch` over
   the two kinds would be a real comparison, since C does not promise an enum
   holds only its enumerators, and id_arg() carrying it crosses gcc's inlining
   threshold in the setre* methods for 720 bytes of duplicated body. */
static mrb_bool
takes_name(mrb_process_id_kind kind)
{
#if defined(MRB_HAL_PROCESS_HAS_UID_BY_NAME) && defined(MRB_HAL_PROCESS_HAS_GID_BY_NAME)
  (void)kind;
  return TRUE;
#elif defined(MRB_HAL_PROCESS_HAS_UID_BY_NAME)
  return kind == MRB_PROCESS_ID_USER;
#else
  return kind == MRB_PROCESS_ID_GROUP;
#endif
}
#endif

static int64_t
id_arg(mrb_state *mrb, mrb_value v, mrb_process_id_kind kind)
{
#ifdef MRB_HAL_PROCESS_NEEDS_ID_BY_NAME
  int64_t id;

  if (mrb_string_p(v) && takes_name(kind)) {
    /* RSTRING_CSTR() refuses a name holding a NUL, which the HAL would
       otherwise read as the part before it and look up something that was
       never asked for. */
    const char *name = RSTRING_CSTR(mrb, v);

    if (mrb_hal_process_id_by_name(mrb, kind, name, &id) != 0) {
      if (errno != 0) mrb_sys_fail(mrb, name);
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "can't find %s for %v",
                 (kind == MRB_PROCESS_ID_USER) ? "user" : "group", v);
    }
    return id;
  }
#endif
  return id_number(mrb, v, kind);
}

#endif /* MRB_HAL_PROCESS_TAKES_ID */

/* The shapes the calls come in.  A method below is its `op` and its
   `mrb_process_id_kind` and nothing more, so the work is done once here, and
   each shape is compiled when the port declared any call of it, which is
   when process_hal.h declares the function it reaches the port through. */

#ifdef MRB_HAL_PROCESS_NEEDS_GETID
/*
 * Hand an ID up as an Integer.
 *
 * An ID crosses the HAL as an int64_t and mrb_int64_value() is what turns one
 * of those into an Integer: inside mrb_int a Fixnum, outside it a bigint, and
 * a RangeError where the build carries none, which is how a clock reading past
 * an Integer is refused too.
 */
static mrb_value
getid(mrb_state *mrb, mrb_process_op op)
{
  int64_t id;

  /* A credential call names no object it was working on, so the message is
     the error alone, as it is for `Process.kill` and `Process.waitpid`. */
  if (mrb_hal_process_getid(mrb, op, &id) != 0) mrb_sys_fail(mrb, NULL);
  return mrb_int64_value(mrb, id);
}
#endif /* MRB_HAL_PROCESS_NEEDS_GETID */

#ifdef MRB_HAL_PROCESS_NEEDS_SETID
static mrb_value
setid(mrb_state *mrb, mrb_process_op op, mrb_process_id_kind kind)
{
  mrb_value v;

  mrb_get_args(mrb, "o", &v);
  if (mrb_hal_process_setid(mrb, op, id_arg(mrb, v, kind)) != 0) mrb_sys_fail(mrb, NULL);
  return mrb_nil_value();
}
#endif

#ifdef MRB_HAL_PROCESS_NEEDS_SETREID
static mrb_value
setreid(mrb_state *mrb, mrb_process_op op, mrb_process_id_kind kind)
{
  mrb_value r, e;
  int64_t rid, eid;

  mrb_get_args(mrb, "oo", &r, &e);
  /* Both are read before either is sent, so a bad second argument does not
     leave the first one already applied. */
  rid = id_arg(mrb, r, kind);
  eid = id_arg(mrb, e, kind);
  if (mrb_hal_process_setreid(mrb, op, rid, eid) != 0) mrb_sys_fail(mrb, NULL);
  return mrb_nil_value();
}
#endif

#ifdef MRB_HAL_PROCESS_NEEDS_SETRESID
static mrb_value
setresid(mrb_state *mrb, mrb_process_op op, mrb_process_id_kind kind)
{
  mrb_value r, e, s;
  int64_t rid, eid, sid;

  mrb_get_args(mrb, "ooo", &r, &e, &s);
  rid = id_arg(mrb, r, kind);
  eid = id_arg(mrb, e, kind);
  sid = id_arg(mrb, s, kind);
  if (mrb_hal_process_setresid(mrb, op, rid, eid, sid) != 0) mrb_sys_fail(mrb, NULL);
  return mrb_nil_value();
}
#endif

/* The methods.  Each has its body where the port declared the call it stands
   for, and where it did not, its name is spelled NULL, which is what the
   table at the end reads and the init turns into the mark. */

/*
 * call-seq:
 *   Process::Sys.getuid  -> integer
 *   Process::Sys.geteuid -> integer
 *   Process::Sys.getgid  -> integer
 *   Process::Sys.getegid -> integer
 *
 * The real or effective user or group ID of the calling process, as
 * getuid(2), geteuid(2), getgid(2) and getegid(2) report it.
 */
#ifdef MRB_HAL_PROCESS_HAS_GETUID
static mrb_value
sys_getuid(mrb_state *mrb, mrb_value self)
{
  return getid(mrb, MRB_PROCESS_OP_GETUID);
}
#else
# define sys_getuid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_GETEUID
static mrb_value
sys_geteuid(mrb_state *mrb, mrb_value self)
{
  return getid(mrb, MRB_PROCESS_OP_GETEUID);
}
#else
# define sys_geteuid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_GETGID
static mrb_value
sys_getgid(mrb_state *mrb, mrb_value self)
{
  return getid(mrb, MRB_PROCESS_OP_GETGID);
}
#else
# define sys_getgid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_GETEGID
static mrb_value
sys_getegid(mrb_state *mrb, mrb_value self)
{
  return getid(mrb, MRB_PROCESS_OP_GETEGID);
}
#else
# define sys_getegid NULL
#endif

/*
 * call-seq:
 *   Process::Sys.setuid(id)  -> nil
 *   Process::Sys.seteuid(id) -> nil
 *   Process::Sys.setruid(id) -> nil
 *   Process::Sys.setgid(id)  -> nil
 *   Process::Sys.setegid(id) -> nil
 *   Process::Sys.setrgid(id) -> nil
 *
 * Set one user or group ID of the calling process, as setuid(2), seteuid(2),
 * setruid(2), setgid(2), setegid(2) and setrgid(2) do.  `id` is a number or
 * the name of a user or group.
 */
#ifdef MRB_HAL_PROCESS_HAS_SETUID
static mrb_value
sys_setuid(mrb_state *mrb, mrb_value self)
{
  return setid(mrb, MRB_PROCESS_OP_SETUID, MRB_PROCESS_ID_USER);
}
#else
# define sys_setuid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETEUID
static mrb_value
sys_seteuid(mrb_state *mrb, mrb_value self)
{
  return setid(mrb, MRB_PROCESS_OP_SETEUID, MRB_PROCESS_ID_USER);
}
#else
# define sys_seteuid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETRUID
static mrb_value
sys_setruid(mrb_state *mrb, mrb_value self)
{
  return setid(mrb, MRB_PROCESS_OP_SETRUID, MRB_PROCESS_ID_USER);
}
#else
# define sys_setruid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETGID
static mrb_value
sys_setgid(mrb_state *mrb, mrb_value self)
{
  return setid(mrb, MRB_PROCESS_OP_SETGID, MRB_PROCESS_ID_GROUP);
}
#else
# define sys_setgid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETEGID
static mrb_value
sys_setegid(mrb_state *mrb, mrb_value self)
{
  return setid(mrb, MRB_PROCESS_OP_SETEGID, MRB_PROCESS_ID_GROUP);
}
#else
# define sys_setegid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETRGID
static mrb_value
sys_setrgid(mrb_state *mrb, mrb_value self)
{
  return setid(mrb, MRB_PROCESS_OP_SETRGID, MRB_PROCESS_ID_GROUP);
}
#else
# define sys_setrgid NULL
#endif

/*
 * call-seq:
 *   Process::Sys.setreuid(rid, eid) -> nil
 *   Process::Sys.setregid(rid, eid) -> nil
 *
 * Set the real and effective user or group ID together, as setreuid(2) and
 * setregid(2) do.  -1 leaves that ID as it is.
 */
#ifdef MRB_HAL_PROCESS_HAS_SETREUID
static mrb_value
sys_setreuid(mrb_state *mrb, mrb_value self)
{
  return setreid(mrb, MRB_PROCESS_OP_SETREUID, MRB_PROCESS_ID_USER);
}
#else
# define sys_setreuid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETREGID
static mrb_value
sys_setregid(mrb_state *mrb, mrb_value self)
{
  return setreid(mrb, MRB_PROCESS_OP_SETREGID, MRB_PROCESS_ID_GROUP);
}
#else
# define sys_setregid NULL
#endif

/*
 * call-seq:
 *   Process::Sys.setresuid(rid, eid, sid) -> nil
 *   Process::Sys.setresgid(rid, eid, sid) -> nil
 *
 * Set the real, effective and saved user or group ID together, as
 * setresuid(2) and setresgid(2) do.  -1 leaves that ID as it is.
 */
#ifdef MRB_HAL_PROCESS_HAS_SETRESUID
static mrb_value
sys_setresuid(mrb_state *mrb, mrb_value self)
{
  return setresid(mrb, MRB_PROCESS_OP_SETRESUID, MRB_PROCESS_ID_USER);
}
#else
# define sys_setresuid NULL
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETRESGID
static mrb_value
sys_setresgid(mrb_state *mrb, mrb_value self)
{
  return setresid(mrb, MRB_PROCESS_OP_SETRESGID, MRB_PROCESS_ID_GROUP);
}
#else
# define sys_setresgid NULL
#endif

/*
 * call-seq:
 *   Process::Sys.issetugid -> true or false
 *
 * Whether the process was started from a set-user-ID or set-group-ID
 * executable, or otherwise had its credentials changed out from under it, as
 * issetugid(2) reports.
 */
#ifdef MRB_HAL_PROCESS_HAS_ISSETUGID
static mrb_value
sys_issetugid(mrb_state *mrb, mrb_value self)
{
  mrb_bool tainted;

  if (mrb_hal_process_issetugid(mrb, &tainted) != 0) mrb_sys_fail(mrb, NULL);
  return mrb_bool_value(tainted);
}
#else
# define sys_issetugid NULL
#endif

/* Every method of Process::Sys, beside its body on this port: the list of
   fifteen, read the same whatever the port declared. */
static const struct sys_method {
  mrb_sym mid;
  mrb_func_t fn;
  mrb_aspec aspec;
} sys_methods[] = {
  { MRB_SYM(getuid),    sys_getuid,    MRB_ARGS_NONE() },
  { MRB_SYM(geteuid),   sys_geteuid,   MRB_ARGS_NONE() },
  { MRB_SYM(getgid),    sys_getgid,    MRB_ARGS_NONE() },
  { MRB_SYM(getegid),   sys_getegid,   MRB_ARGS_NONE() },
  { MRB_SYM(setuid),    sys_setuid,    MRB_ARGS_REQ(1) },
  { MRB_SYM(seteuid),   sys_seteuid,   MRB_ARGS_REQ(1) },
  { MRB_SYM(setruid),   sys_setruid,   MRB_ARGS_REQ(1) },
  { MRB_SYM(setgid),    sys_setgid,    MRB_ARGS_REQ(1) },
  { MRB_SYM(setegid),   sys_setegid,   MRB_ARGS_REQ(1) },
  { MRB_SYM(setrgid),   sys_setrgid,   MRB_ARGS_REQ(1) },
  { MRB_SYM(setreuid),  sys_setreuid,  MRB_ARGS_REQ(2) },
  { MRB_SYM(setregid),  sys_setregid,  MRB_ARGS_REQ(2) },
  { MRB_SYM(setresuid), sys_setresuid, MRB_ARGS_REQ(3) },
  { MRB_SYM(setresgid), sys_setresgid, MRB_ARGS_REQ(3) },
  { MRB_SYM(issetugid), sys_issetugid, MRB_ARGS_NONE() },
};

void
mrb_process_sys_init(mrb_state *mrb, struct RClass *process)
{
  struct RClass *sys = mrb_define_module_under_id(mrb, process, MRB_SYM(Sys));
  size_t i;

  /* Every method is defined, and the port's process_hal_features.h decides
     which of them has a body.  The rest of the tree leaves a method the port
     did not declare undefined, as mruby-dir's `Dir.chroot` is; these are
     defined with `mrb_notimplement_m` for a body instead, so `respond_to?`
     answers false for them and calling one raises NotImplementedError naming
     the method, which is what CRuby does for `Process::Sys.issetugid` on
     glibc. */
  for (i = 0; i < sizeof(sys_methods) / sizeof(sys_methods[0]); i++) {
    const struct sys_method *m = &sys_methods[i];

    if (m->fn != NULL) {
      mrb_define_module_function_id(mrb, sys, m->mid, m->fn, m->aspec);
    }
    else {
      /* Every argument shape, rather than the one the method would have had.
         What is wrong with the call is that this machine does not have it, and
         that answer cannot depend on how many arguments it was passed: mruby
         checks the shape before the body runs, so keeping the real one would
         answer a call written with the wrong count by complaining about the
         count.  CRuby's unimplemented methods take any number of arguments
         for the same reason. */
      mrb_define_module_function_id(mrb, sys, m->mid, mrb_notimplement_m, MRB_ARGS_ANY());
    }
  }
}
