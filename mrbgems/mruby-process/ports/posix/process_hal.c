/*
** process_hal.c - POSIX HAL implementation for mruby-process
**
** See Copyright Notice in mruby.h
**
** POSIX implementation of the process HAL using getpid(2), getppid(2),
** waitpid(2) and kill(2), and the user and group ID calls.  The clocks are
** clock_hal.c's.
** Supported platforms: Linux, macOS, BSD, Unix
**
** Not every credential call is POSIX.  Which ones this host has is declared
** in include/process_hal_features.h, at compile time and never from an
** errno: a call the kernel refused at run time is a different thing, and
** stays an errno.
*/

/* setresuid(2) and setresgid(2) need _GNU_SOURCE to be declared on glibc.  It
   is set in this gem's mrbgem.rake rather than here: a feature-test macro has
   to precede every header, and this source does not always get to be first,
   since the amalgam concatenates every source into one translation unit. */

#include <mruby.h>
#include "process_hal.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

/*
 * Wait status feature capabilities
 *
 * Each MRB_PROCESS_HAVE_* is always defined, to 0 or 1, so the rest of this
 * file tests it with #if rather than #ifdef; the #ifndef guard around each
 * one lets a build override the detection below where it gets a host wrong.
 */

/* Whether WIFSIGNALED's status can also say the process dumped core; not
   every host's <sys/wait.h> defines this. */
#ifndef MRB_PROCESS_HAVE_WCOREDUMP
# ifdef WCOREDUMP
#  define MRB_PROCESS_HAVE_WCOREDUMP 1
# else
#  define MRB_PROCESS_HAVE_WCOREDUMP 0
# endif
#endif

/* An mrb_int is wider than a pid_t where mrb_int is 64-bit, so a pid from
   Ruby is range-checked rather than truncated into one. */
#define PID_FITS(pid) ((pid) >= (mrb_int)INT_MIN && (pid) <= (mrb_int)INT_MAX)

/*
 * Process Identity
 */

mrb_int
mrb_hal_process_pid(mrb_state *mrb)
{
  (void)mrb;
  return (mrb_int)getpid();
}

mrb_int
mrb_hal_process_ppid(mrb_state *mrb)
{
  (void)mrb;
  return (mrb_int)getppid();
}

/*
 * Waiting
 */

#ifdef MRB_HAL_PROCESS_HAS_WAIT
int
mrb_hal_process_waitpid(mrb_state *mrb, mrb_int pid, unsigned int flags,
                        mrb_int *result_pid, mrb_int *raw_status)
{
  pid_t result;
  int status = 0;
  int options = 0;
  (void)mrb;

  if (!PID_FITS(pid)) {
    errno = ECHILD;
    return -1;
  }
  if (flags & MRB_PROCESS_WAIT_NOHANG) options |= WNOHANG;
  if (flags & MRB_PROCESS_WAIT_UNTRACED) options |= WUNTRACED;

  do {
    result = waitpid((pid_t)pid, &status, options);
  } while (result == -1 && errno == EINTR);

  if (result == -1) return -1;

  /* result is 0 when WNOHANG found nothing ready; status is untouched then */
  *result_pid = (mrb_int)result;
  *raw_status = (result == 0) ? 0 : (mrb_int)status;
  return 0;
}
#endif

/*
 * Signalling
 */

int
mrb_hal_process_kill(mrb_state *mrb, mrb_int pid, mrb_int signo)
{
  (void)mrb;

  /* Which numbers name a signal is kill(2)'s to say, and it answers EINVAL
     for the ones this host does not have, so the range asked for here is only
     the one an int can carry. */
  if (signo < 0 || signo > (mrb_int)INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  if (!PID_FITS(pid)) {
    errno = ESRCH;
    return -1;
  }
  return kill((pid_t)pid, (int)signo);
}

/*
 * Process credentials
 */

/* POSIX fixes neither width nor sign for uid_t and gid_t: 32 bits and
   unsigned on every current host, 16 bits on old ones, signed on a few.  So
   both are read off the type rather than assumed, and the numbers that name
   an ID here are the ones the type holds and, for an unsigned type, the
   negative the same bits read as, which is how Ruby spells the (uid_t)-1 of
   setreuid(2).  A signed type spells -1 as itself, and a number past its top
   is refused rather than folded onto a lower ID by the cast: 4294967295 on
   a signed 32-bit uid_t would land on the -1 that setreuid(2) reads as
   "leave this one alone".  The common layer asks this before it sends a
   number down, so the casts below are the platform reading its own type and
   never a value it would have to rename.

   Whether a type is signed is whether -1 stays below 1 in it, spelled that
   way rather than against 0, which compilers flag as always false for an
   unsigned type. */
#define ID_TYPE_SIGNED(type) ((type)-1 < (type)1)

/* An ID crosses the HAL as an int64_t, so a type has to fit in one for
   every value it holds to cross whole: any type narrower than 64 bits, and
   a signed one exactly that wide.  No host has a uid_t past that; one that
   did would need the transport widened, and is told so here rather than
   left to report the top half of its IDs as negative numbers or the getters
   left to hand back the low 64 bits of one. */
#define ID_TYPE_FITS_INT64(type) \
  (sizeof(type) < sizeof(int64_t) || \
   (sizeof(type) == sizeof(int64_t) && ID_TYPE_SIGNED(type)))

mrb_static_assert(ID_TYPE_FITS_INT64(uid_t),
                  "uid_t does not fit the int64_t an ID crosses the HAL as");
mrb_static_assert(ID_TYPE_FITS_INT64(gid_t),
                  "gid_t does not fit the int64_t an ID crosses the HAL as");

#ifdef MRB_HAL_PROCESS_TAKES_ID
mrb_bool
mrb_hal_process_id_fits(mrb_process_id_kind kind, int64_t id)
{
  return (kind == MRB_PROCESS_ID_USER)
    ? mrb_process_id_fits_type(id, sizeof(uid_t), ID_TYPE_SIGNED(uid_t))
    : mrb_process_id_fits_type(id, sizeof(gid_t), ID_TYPE_SIGNED(gid_t));
}
#endif /* MRB_HAL_PROCESS_TAKES_ID */

/* Each shape once, and inside it one case a call, under the macro that
   declared the call: a case for a call the host does not have would not
   compile, and the macros in process_hal_features.h decide which operations
   reach a function, so the default is a caller that went around them. */

#ifdef MRB_HAL_PROCESS_NEEDS_GETID
int
mrb_hal_process_getid(mrb_state *mrb, mrb_process_op op, int64_t *id)
{
  (void)mrb;

  /* Widened as the type reads it: an unsigned uid_t arrives as the number
     itself and a signed one keeps its sign.  Nothing is reinterpreted. */
  switch (op) {
#ifdef MRB_HAL_PROCESS_HAS_GETUID
  case MRB_PROCESS_OP_GETUID:  *id = (int64_t)getuid();  return 0;
#endif
#ifdef MRB_HAL_PROCESS_HAS_GETEUID
  case MRB_PROCESS_OP_GETEUID: *id = (int64_t)geteuid(); return 0;
#endif
#ifdef MRB_HAL_PROCESS_HAS_GETGID
  case MRB_PROCESS_OP_GETGID:  *id = (int64_t)getgid();  return 0;
#endif
#ifdef MRB_HAL_PROCESS_HAS_GETEGID
  case MRB_PROCESS_OP_GETEGID: *id = (int64_t)getegid(); return 0;
#endif
  default:
    errno = ENOSYS;
    return -1;
  }
}
#endif /* MRB_HAL_PROCESS_NEEDS_GETID */

#ifdef MRB_HAL_PROCESS_NEEDS_SETID
int
mrb_hal_process_setid(mrb_state *mrb, mrb_process_op op, int64_t id)
{
  (void)mrb;

  switch (op) {
#ifdef MRB_HAL_PROCESS_HAS_SETUID
  case MRB_PROCESS_OP_SETUID:  return setuid((uid_t)id);
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETEUID
  case MRB_PROCESS_OP_SETEUID: return seteuid((uid_t)id);
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETRUID
  case MRB_PROCESS_OP_SETRUID: return setruid((uid_t)id);
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETGID
  case MRB_PROCESS_OP_SETGID:  return setgid((gid_t)id);
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETEGID
  case MRB_PROCESS_OP_SETEGID: return setegid((gid_t)id);
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETRGID
  case MRB_PROCESS_OP_SETRGID: return setrgid((gid_t)id);
#endif
  default:
    errno = ENOSYS;
    return -1;
  }
}
#endif /* MRB_HAL_PROCESS_NEEDS_SETID */

#ifdef MRB_HAL_PROCESS_NEEDS_SETREID
int
mrb_hal_process_setreid(mrb_state *mrb, mrb_process_op op, int64_t rid, int64_t eid)
{
  (void)mrb;

  switch (op) {
#ifdef MRB_HAL_PROCESS_HAS_SETREUID
  case MRB_PROCESS_OP_SETREUID: return setreuid((uid_t)rid, (uid_t)eid);
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETREGID
  case MRB_PROCESS_OP_SETREGID: return setregid((gid_t)rid, (gid_t)eid);
#endif
  default:
    errno = ENOSYS;
    return -1;
  }
}
#endif /* MRB_HAL_PROCESS_NEEDS_SETREID */

#ifdef MRB_HAL_PROCESS_NEEDS_SETRESID
int
mrb_hal_process_setresid(mrb_state *mrb, mrb_process_op op,
                         int64_t rid, int64_t eid, int64_t sid)
{
  (void)mrb;

  switch (op) {
#ifdef MRB_HAL_PROCESS_HAS_SETRESUID
  case MRB_PROCESS_OP_SETRESUID: return setresuid((uid_t)rid, (uid_t)eid, (uid_t)sid);
#endif
#ifdef MRB_HAL_PROCESS_HAS_SETRESGID
  case MRB_PROCESS_OP_SETRESGID: return setresgid((gid_t)rid, (gid_t)eid, (gid_t)sid);
#endif
  default:
    errno = ENOSYS;
    return -1;
  }
}
#endif /* MRB_HAL_PROCESS_NEEDS_SETRESID */

#if defined(MRB_HAL_PROCESS_TAKES_ID) && defined(MRB_HAL_PROCESS_NEEDS_ID_BY_NAME)

#ifdef MRB_HAL_PROCESS_HAS_UID_BY_NAME
#include <pwd.h>
#endif
#ifdef MRB_HAL_PROCESS_HAS_GID_BY_NAME
#include <grp.h>
#endif

/*
 * Looking an ID up by name
 *
 * getpwnam_r(3) and getgrnam_r(3) answer from whatever the host keeps its
 * accounts in, which is the same place Ruby's own lookup reads.  The
 * reentrant forms, because the plain ones hand out one static record shared
 * by every thread of the process, and a host program may run an interpreter
 * on each of several threads, or call the plain forms itself.  Each is
 * declared on its own, so a port may have one table and not the other, and
 * what the two share is written once: the buffer the record's strings go in,
 * whose size only the name service knows.  sysconf(3) offers a first guess
 * where the C library has a name to ask it by, ERANGE says it was not
 * enough, and the buffer doubles.  NAME_BUF_LIMIT bounds the doubling and
 * not the buffer: growing stops once the buffer has reached it, so the last
 * doubling may land past it, and a host whose sysconf(3) answer is already
 * larger keeps that answer rather than being cut back to a size it has said
 * is too small.  CRuby's loop has the same shape and stops at the same
 * number.
 *
 * Three answers come back, and the call's own return value tells them apart
 * as CRuby's obj2uid() reads it.  A record is the ID.  Answering 0 with no
 * record is the unknown name POSIX says it is, and the only unknown name:
 * errno is left clear and the common layer raises an ArgumentError.  Every
 * other return is the lookup failing before it could answer and stays in
 * errno, a SystemCallError naming the account it was looking for.
 *
 * Which is not the same set as getpwnam(3)'s ERRORS, where ENOENT, ESRCH,
 * EBADF and EPERM stand beside 0 as ways a backend says not found, nor the
 * set of CRuby's pwd_not_found(), which reads those five where `Dir.home`
 * wants a missing account to be nil rather than an error.  Reading them here
 * would report a name service that really did refuse with EPERM as a name no
 * account has, and refusing is not answering.  The price is the one CRuby
 * pays: glibc's switch answers ENOENT rather than 0 for a name it has no
 * record of, so an unknown account is an Errno::ENOENT there and the
 * ArgumentError on a libc that answers 0, musl among them.
 */

#define NAME_BUF_DEFAULT 4096
#define NAME_BUF_LIMIT   0x10000

/* One call of the reentrant lookup into `buf`: the return value is the
   call's, `*raw` is written when a record came back, and `*found` says
   whether one did.  One of these a table, since the two calls fill different
   records, and the loop that sizes the buffer is written once below.

   The first size to try comes with it.  _SC_GETPW_R_SIZE_MAX and
   _SC_GETGR_R_SIZE_MAX are POSIX's, but mrbgem.rake asks about the two calls
   and not about the two names, and a C library may have the calls without
   them, so each is asked of sysconf(3) only where the header defines it, as
   CRuby does, and the default stands where it is not or sysconf(3) has no
   answer. */
typedef int name_lookup(const char *name, char *buf, size_t len,
                        int *found, int64_t *raw);

#ifdef MRB_HAL_PROCESS_HAS_UID_BY_NAME
static int
passwd_lookup(const char *name, char *buf, size_t len, int *found, int64_t *raw)
{
  struct passwd pw, *result = NULL;
  int err = getpwnam_r(name, &pw, buf, len, &result);

  *found = (err == 0 && result != NULL);
  if (*found) *raw = (int64_t)result->pw_uid;
  return err;
}

static size_t
passwd_buf_size(void)
{
#ifdef _SC_GETPW_R_SIZE_MAX
  long hint = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (hint > 0) return (size_t)hint;
#endif
  return NAME_BUF_DEFAULT;
}
#endif /* MRB_HAL_PROCESS_HAS_UID_BY_NAME */

#ifdef MRB_HAL_PROCESS_HAS_GID_BY_NAME
static int
group_lookup(const char *name, char *buf, size_t len, int *found, int64_t *raw)
{
  struct group gr, *result = NULL;
  int err = getgrnam_r(name, &gr, buf, len, &result);

  *found = (err == 0 && result != NULL);
  if (*found) *raw = (int64_t)result->gr_gid;
  return err;
}

static size_t
group_buf_size(void)
{
#ifdef _SC_GETGR_R_SIZE_MAX
  long hint = sysconf(_SC_GETGR_R_SIZE_MAX);
  if (hint > 0) return (size_t)hint;
#endif
  return NAME_BUF_DEFAULT;
}
#endif /* MRB_HAL_PROCESS_HAS_GID_BY_NAME */

static int
id_by_name(mrb_state *mrb, name_lookup *lookup, size_t len,
           const char *name, int64_t *id)
{
  char *buf = NULL;
  int64_t raw = 0;
  int found = 0;
  int err;

  for (;;) {
    char *grown = (char*)mrb_realloc_simple(mrb, buf, len);
    if (grown == NULL) {
      mrb_free(mrb, buf);
      errno = ENOMEM;
      return -1;
    }
    buf = grown;
    err = lookup(name, buf, len, &found, &raw);
    if (err != ERANGE || len >= NAME_BUF_LIMIT) break;
    len *= 2;
  }
  mrb_free(mrb, buf);

  if (found) {
    *id = raw;
    return 0;
  }
  errno = err;
  return -1;
}

int
mrb_hal_process_id_by_name(mrb_state *mrb, mrb_process_id_kind kind,
                           const char *name, int64_t *id)
{
  switch (kind) {
#ifdef MRB_HAL_PROCESS_HAS_UID_BY_NAME
  case MRB_PROCESS_ID_USER:
    return id_by_name(mrb, passwd_lookup, passwd_buf_size(), name, id);
#endif
#ifdef MRB_HAL_PROCESS_HAS_GID_BY_NAME
  case MRB_PROCESS_ID_GROUP:
    return id_by_name(mrb, group_lookup, group_buf_size(), name, id);
#endif
  default:
    /* The common layer sends a name only for a table this port declared. */
    errno = ENOSYS;
    return -1;
  }
}

#endif /* MRB_HAL_PROCESS_TAKES_ID && MRB_HAL_PROCESS_NEEDS_ID_BY_NAME */

#ifdef MRB_HAL_PROCESS_HAS_ISSETUGID
int
mrb_hal_process_issetugid(mrb_state *mrb, mrb_bool *tainted)
{
  (void)mrb;

  /* issetugid(2) has no failure of its own; it answers or it is not there. */
  *tainted = issetugid() != 0;
  return 0;
}
#endif /* MRB_HAL_PROCESS_HAS_ISSETUGID */

/*
 * Status Decoding
 */

void
mrb_hal_process_status_decode(mrb_state *mrb, mrb_int pid, mrb_int raw_status,
                              mrb_process_status *status)
{
  int raw = (int)raw_status;
  (void)mrb;

  status->pid = pid;
  status->raw_status = raw_status;
  status->exitstatus = 0;
  status->termsig = 0;
  status->stopsig = 0;
  status->flags = 0;

  /* WIFSTOPPED comes first: a stopped status can also satisfy WIFSIGNALED on
     some platforms, and stopping is the more specific answer. */
  if (WIFSTOPPED(raw)) {
    status->flags |= MRB_PROCESS_STATUS_STOPPED;
    status->stopsig = (mrb_int)WSTOPSIG(raw);
  }
  else if (WIFEXITED(raw)) {
    status->flags |= MRB_PROCESS_STATUS_EXITED;
    status->exitstatus = (mrb_int)WEXITSTATUS(raw);
  }
  else if (WIFSIGNALED(raw)) {
    status->flags |= MRB_PROCESS_STATUS_SIGNALED;
    status->termsig = (mrb_int)WTERMSIG(raw);
#if MRB_PROCESS_HAVE_WCOREDUMP
    if (WCOREDUMP(raw)) {
      status->flags |= MRB_PROCESS_STATUS_COREDUMP;
    }
#endif
  }
}

/*
 * HAL Initialization/Finalization
 */

void
mrb_hal_process_init(mrb_state *mrb)
{
  (void)mrb;
}

void
mrb_hal_process_final(mrb_state *mrb)
{
  (void)mrb;
}
