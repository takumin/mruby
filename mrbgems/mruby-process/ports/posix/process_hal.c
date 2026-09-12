/*
** process_hal.c - POSIX HAL implementation for mruby-process
**
** See Copyright Notice in mruby.h
**
** POSIX implementation of the process HAL using getpid(2), getppid(2),
** waitpid(2), kill(2), getrlimit(2) and setrlimit(2).  The clocks are
** clock_hal.c's.
** Supported platforms: Linux, macOS, BSD, Unix
*/

#include <mruby.h>
#include "process_hal.h"

#include <sys/types.h>
#include <sys/wait.h>
#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) || defined(MRB_HAL_PROCESS_HAS_SETRLIMIT)
#include <sys/resource.h>
#endif

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>

/*
 * Feature Capabilities
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
 * Resource Limits
 */

#if defined(MRB_HAL_PROCESS_HAS_GETRLIMIT) || defined(MRB_HAL_PROCESS_HAS_SETRLIMIT)

/* No number of this host's stands for the resource.  Spelled outside the
   platform's own RLIMIT_ namespace, since it is this port's and not one of
   theirs. */
#define MRB_RLIMIT_ABSENT (-1)

/* The number this host calls each of the resources by, in the order the ids
   are declared in.  Read with the preprocessor rather than asked of the
   build, as CRuby's process.c reads them: each is a macro or an enumerator
   with a macro beside it that <sys/resource.h> either has or has not, and
   nothing about a host's name settles which. */
static const int rlimit_numbers[] = {
#ifdef RLIMIT_AS
  RLIMIT_AS,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_CORE
  RLIMIT_CORE,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_CPU
  RLIMIT_CPU,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_DATA
  RLIMIT_DATA,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_FSIZE
  RLIMIT_FSIZE,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_MEMLOCK
  RLIMIT_MEMLOCK,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_MSGQUEUE
  RLIMIT_MSGQUEUE,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_NICE
  RLIMIT_NICE,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_NOFILE
  RLIMIT_NOFILE,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_NPROC
  RLIMIT_NPROC,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_NPTS
  RLIMIT_NPTS,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_RSS
  RLIMIT_RSS,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_RTPRIO
  RLIMIT_RTPRIO,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_RTTIME
  RLIMIT_RTTIME,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_SBSIZE
  RLIMIT_SBSIZE,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_SIGPENDING
  RLIMIT_SIGPENDING,
#else
  MRB_RLIMIT_ABSENT,
#endif
#ifdef RLIMIT_STACK
  RLIMIT_STACK,
#else
  MRB_RLIMIT_ABSENT,
#endif
};

mrb_static_assert(sizeof(rlimit_numbers) / sizeof(rlimit_numbers[0]) ==
                  (size_t)MRB_PROCESS_RLIMIT_COUNT,
                  "every resource id needs a number, or MRB_RLIMIT_ABSENT");
mrb_static_assert(MRB_PROCESS_RLIMIT_COUNT <= 32,
                  "the ids are answered as bits of a uint32_t");

/* The number to call getrlimit(2) with, or MRB_RLIMIT_ABSENT for a resource this
   host has not.  An id outside the list arrives here only from a caller that
   is not the common layer, which refuses one before the HAL is reached. */
static int
rlimit_number(mrb_int id)
{
  if (id < 0 || id >= (mrb_int)MRB_PROCESS_RLIMIT_COUNT) return MRB_RLIMIT_ABSENT;
  return rlimit_numbers[id];
}

uint32_t
mrb_hal_process_rlimit_ids(mrb_state *mrb)
{
  uint32_t ids = 0;
  int i;
  (void)mrb;

  for (i = 0; i < (int)MRB_PROCESS_RLIMIT_COUNT; i++) {
    if (rlimit_numbers[i] != MRB_RLIMIT_ABSENT) ids |= (uint32_t)1 << i;
  }
  return ids;
}

/*
 * Which of the two interfaces the limits are read and written through.
 *
 * The saved limits below are what a platform answers where its own `rlim_t`
 * is too narrow for the limit the kernel holds, which is a property of the
 * compilation environment rather than of the kernel: illumos answers
 * `RLIM_SAVED_CUR` where its 32-bit environment is asked for a limit its
 * 64-bit one reports as the number it is, and offers getrlimit64(2) beside
 * getrlimit(2) for exactly that.  Whether a host does is asked rather than
 * assumed, as the width of its `rlim_t` is; mrbgem.rake asks both, so a build
 * with no need of the wider calls keeps the ones POSIX names, a build that
 * needs them and has them asks the question it can answer, and a build that
 * needs them and has not is left to report a saved limit as a saved limit.
 */
#if !defined(HAVE_WIDE_RLIM_T) && defined(HAVE_GETRLIMIT64) && defined(HAVE_SETRLIMIT64)
typedef rlim64_t port_rlim_t;
# define port_rlimit           rlimit64
# define port_getrlimit        getrlimit64
# define port_setrlimit        setrlimit64
# define PORT_RLIM_INFINITY    RLIM64_INFINITY
# ifdef RLIM64_SAVED_CUR
#  define PORT_RLIM_SAVED_CUR  RLIM64_SAVED_CUR
# endif
# ifdef RLIM64_SAVED_MAX
#  define PORT_RLIM_SAVED_MAX  RLIM64_SAVED_MAX
# endif
#else
typedef rlim_t port_rlim_t;
# define port_rlimit           rlimit
# define port_getrlimit        getrlimit
# define port_setrlimit        setrlimit
# define PORT_RLIM_INFINITY    RLIM_INFINITY
# ifdef RLIM_SAVED_CUR
#  define PORT_RLIM_SAVED_CUR  RLIM_SAVED_CUR
# endif
# ifdef RLIM_SAVED_MAX
#  define PORT_RLIM_SAVED_MAX  RLIM_SAVED_MAX
# endif
#endif

#ifdef MRB_HAL_PROCESS_HAS_GETRLIMIT
/*
 * A limit the platform reported, as the kind of answer it is.
 *
 * No limit is tested for first: Linux, the BSDs and macOS spell their saved
 * limits the way they spell no limit, and mean no limit by all three.
 */
static void
rlimit_from_platform(port_rlim_t v, mrb_process_rlimit_value *out)
{
  out->value = 0;
  if (v == PORT_RLIM_INFINITY) {
    out->kind = MRB_PROCESS_RLIMIT_INFINITY;
  }
#ifdef PORT_RLIM_SAVED_CUR
  else if (v == PORT_RLIM_SAVED_CUR) {
    out->kind = MRB_PROCESS_RLIMIT_SAVED_CUR;
  }
#endif
#ifdef PORT_RLIM_SAVED_MAX
  else if (v == PORT_RLIM_SAVED_MAX) {
    out->kind = MRB_PROCESS_RLIMIT_SAVED_MAX;
  }
#endif
  else {
    out->kind = MRB_PROCESS_RLIMIT_VALUE;
    out->value = (uint64_t)v;
  }
}

int
mrb_hal_process_getrlimit(mrb_state *mrb, mrb_int id, mrb_process_rlimit *r)
{
  struct port_rlimit rlim;
  int resource = rlimit_number(id);
  (void)mrb;

  if (resource == MRB_RLIMIT_ABSENT) {
    errno = EINVAL;
    return -1;
  }
  if (port_getrlimit(resource, &rlim) != 0) return -1;
  rlimit_from_platform(rlim.rlim_cur, &r->cur);
  rlimit_from_platform(rlim.rlim_max, &r->max);
  return 0;
}
#endif

#ifdef MRB_HAL_PROCESS_HAS_SETRLIMIT
/*
 * The platform's spelling of a limit, or -1 for one this host has no value
 * for: a number its `rlim_t` cannot carry, or a saved limit it does not name.
 * The width test is what a `rlim_t` narrower than the interface is for; on
 * the wider of the two above, no number that crossed the HAL can fail it.
 */
static int
rlimit_to_platform(const mrb_process_rlimit_value *v, port_rlim_t *out)
{
  const port_rlim_t widest = (port_rlim_t)-1;  /* every bit set, whatever the width */

  switch (v->kind) {
    case MRB_PROCESS_RLIMIT_VALUE:
      if (v->value > (uint64_t)widest) return -1;
      *out = (port_rlim_t)v->value;
      return 0;
    case MRB_PROCESS_RLIMIT_INFINITY:
      *out = PORT_RLIM_INFINITY;
      return 0;
#ifdef PORT_RLIM_SAVED_CUR
    case MRB_PROCESS_RLIMIT_SAVED_CUR:
      *out = PORT_RLIM_SAVED_CUR;
      return 0;
#endif
#ifdef PORT_RLIM_SAVED_MAX
    case MRB_PROCESS_RLIMIT_SAVED_MAX:
      *out = PORT_RLIM_SAVED_MAX;
      return 0;
#endif
    default:
      /* A kind this host has no value for, which is a saved limit on a host
         that names none: the limits it reports are the limits it has. */
      return -1;
  }
}

int
mrb_hal_process_setrlimit(mrb_state *mrb, mrb_int id, const mrb_process_rlimit *r)
{
  struct port_rlimit rlim;
  int resource = rlimit_number(id);
  (void)mrb;

  if (resource == MRB_RLIMIT_ABSENT ||
      rlimit_to_platform(&r->cur, &rlim.rlim_cur) != 0 ||
      rlimit_to_platform(&r->max, &rlim.rlim_max) != 0) {
    errno = EINVAL;
    return -1;
  }
  return port_setrlimit(resource, &rlim);
}
#endif

#endif /* a port with either limit call */

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
