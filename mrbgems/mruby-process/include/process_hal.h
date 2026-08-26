/*
** process_hal.h - Process Hardware Abstraction Layer (HAL)
**
** See Copyright Notice in mruby.h
**
** This header defines the HAL interface for platform-specific process
** operations.  A port under mruby-process/ports/<port_name>/, or an
** external provider gem named hal-process-<conf>, supplies every function
** declared here.
**
** The HAL answers OS-level facts and performs OS-level operations.  It knows
** nothing about `Process::Status`, `Process::Tms`, `$?`, `$$`, blocks or any
** other Ruby notion: those belong to the common sources under src/.  In the
** other direction, no platform type or macro (`pid_t`, `WIFEXITED`,
** `SIGTERM`, `WNOHANG`, `CLOCK_MONOTONIC`, `clock_t`, ...) crosses into the
** common layer: process and signal numbers travel as `mrb_int`, wait options
** as the MRB_PROCESS_WAIT_* bits below, a wait result as an
** `mrb_process_event` carrying an already decoded `mrb_process_status`, a
** clock as one of the `mrb_process_clock_id` values, a time it reported as
** `mrb_process_clock_time`, whose two fields are `int64_t` rather than
** `mrb_int` for the reason given where it is defined, and the four CPU time
** totals behind `Process.times` as `mrb_process_times`, four more
** `mrb_process_clock_time` readings rather than platform ticks.
**
** What a child *is* stays behind the HAL as well.  A pid labels a child; it
** is not the child, because the OS may hand the same number to another
** process once the first one has been reaped.  So spawn hands back an opaque
** `mrb_hal_process_child`, every wait takes that object rather than a
** number, and the port keeps whatever it really needs inside it: a pid on
** POSIX, a HANDLE on Windows.
**
** What a signal is *called* is not asked here at all.  mruby-signal owns
** that table, and both `Process.kill` and `Process::Status#to_s` reach it
** through signal_hal.h.
*/

#ifndef MRUBY_PROCESS_HAL_H
#define MRUBY_PROCESS_HAL_H

#include <mruby.h>
#include <stdint.h>

#include <stddef.h>

/* A platform that does not let a process create another has no process
   creation whatever the configuration asks for, so it says so here rather
   than leaving every build for it to be configured by hand.  iOS is one.
   Windows is one until its port grows a spawn of its own. */
#if defined(_WIN32) || defined(_WIN64)
# ifndef MRB_NO_PROCESS_SPAWN
#  define MRB_NO_PROCESS_SPAWN 1
# endif
#endif
#if defined(__APPLE__)
# include <TargetConditionals.h>
# if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#  ifndef MRB_NO_PROCESS_SPAWN
#   define MRB_NO_PROCESS_SPAWN 1
#  endif
# endif
#endif

MRB_BEGIN_DECL

/*
 * Decoded wait status
 */

/* What a decoded status says about how the process left the CPU.  A port sets
   the flags it can tell apart; a platform that only reports an exit code sets
   MRB_PROCESS_STATUS_EXITED alone. */
typedef enum mrb_process_status_flags {
  MRB_PROCESS_STATUS_EXITED    = 1 << 0,  /* ran to completion; exitstatus is set */
  MRB_PROCESS_STATUS_SIGNALED  = 1 << 1,  /* killed by a signal; termsig is set */
  MRB_PROCESS_STATUS_STOPPED   = 1 << 2,  /* stopped, not reaped; stopsig is set */
  MRB_PROCESS_STATUS_COREDUMP  = 1 << 3,  /* a core dump accompanied the signal */
} mrb_process_status_flags;

/* A wait status in platform-neutral form.  Fields not selected by `flags`
   hold 0.  `raw_status` is the platform value the status was decoded from,
   kept so that `Process::Status#to_i` can hand it back unchanged. */
typedef struct mrb_process_status {
  mrb_int pid;
  mrb_int raw_status;
  mrb_int exitstatus;
  mrb_int termsig;
  mrb_int stopsig;
  unsigned int flags;
} mrb_process_status;

/*
 * Wait options
 *
 * These are the values `Process::WNOHANG` and `Process::WUNTRACED` carry, so
 * they are mruby's own; a port translates them to whatever its platform
 * spells them as.
 */
#define MRB_PROCESS_WAIT_NOHANG   (1u << 0)  /* return at once if nothing is ready */
#define MRB_PROCESS_WAIT_UNTRACED (1u << 1)  /* report stopped children too */

/* Every bit a wait may carry.  The common layer refuses anything else, so a
   port is handed only bits it knows and does not have to say what it would
   do with the others. */
#define MRB_PROCESS_WAIT_FLAGS (MRB_PROCESS_WAIT_NOHANG | MRB_PROCESS_WAIT_UNTRACED)

/*
 * Wait events
 *
 * A wait draws an event from a set of children.  An event is not a lifecycle
 * transition: a child that merely stopped is still alive and still owes a
 * reap, so the common layer decides from `kind` whether the child is done
 * with, and only it does.
 */
typedef enum mrb_process_event_kind {
  MRB_PROCESS_EVENT_NONE      = 0,  /* NOHANG, nothing available */
  MRB_PROCESS_EVENT_EXITED    = 1,
  MRB_PROCESS_EVENT_SIGNALED  = 2,
  MRB_PROCESS_EVENT_STOPPED   = 3,
  MRB_PROCESS_EVENT_CONTINUED = 4,  /* reserved; no current flag requests it */
} mrb_process_event_kind;

/* The port's context.  It holds whatever the platform needs to know about
   the children this interpreter has: nothing at all on POSIX, where the
   kernel already knows, and the live handles on Windows, where nothing but
   this does. */
typedef struct mrb_hal_process_context mrb_hal_process_context;

/* One child, as the port sees it. */
typedef struct mrb_hal_process_child mrb_hal_process_child;

typedef struct mrb_process_event {
  mrb_process_event_kind kind;
  mrb_hal_process_child *child;   /* which child produced the event, or NULL
                                     when the wait reaped one this context
                                     never spawned */
  mrb_process_status status;      /* valid unless kind is MRB_PROCESS_EVENT_NONE */
} mrb_process_event;

/*
 * Clocks
 *
 * Which clocks there are is mruby's own list rather than the platform's, as
 * the wait options above are: `CLOCK_MONOTONIC` is 1 on Linux, 6 on macOS
 * and 4 on FreeBSD, and Windows has no `clockid_t` to give it a number at
 * all, so a program that names a clock would otherwise be naming a different
 * one on each port.  The common layer refuses an id outside this list before
 * a port sees it, and a port whose platform has no such clock fails that
 * one with EINVAL.
 */
typedef enum mrb_process_clock_id {
  MRB_PROCESS_CLOCK_REALTIME = 0,     /* wall clock, counted from the epoch */
  MRB_PROCESS_CLOCK_MONOTONIC,        /* never steps; unspecified origin */
  MRB_PROCESS_CLOCK_PROCESS_CPUTIME,  /* CPU time this process has spent */
  MRB_PROCESS_CLOCK_THREAD_CPUTIME,   /* CPU time this thread has spent */
  MRB_PROCESS_CLOCK_COUNT             /* how many ids there are; not a clock */
} mrb_process_clock_id;

/*
 * A time a clock reported, kept in two fields so that nothing is lost on the
 * way up.
 *
 * A Float would hold 53 of the 61 bits a wall-clock nanosecond needs, which
 * would leave `:nanosecond` unable to answer honestly however it was asked,
 * and a single count of nanoseconds runs out in 2262.  A port therefore
 * always reports the same two numbers and knows nothing of the unit a caller
 * wanted: arriving at that unit is the common layer's.
 *
 * The fields are `int64_t` rather than `mrb_int`, which is what every other
 * quantity crossing this interface travels as.  A reading is a fact about
 * the platform and its size is the platform's, so a port reports what its
 * clock said; how much of that this build's Integer can carry is a question
 * about mruby, and is answered where RangeError can be said and a bigint
 * can be built.  Were it `mrb_int`, a build with a 32-bit one would have
 * every port refusing the wall clock from 2038 on, through `errno`, which
 * has no way to say that the platform was fine and the Integer was not.
 * `int64_t` is no more a platform type than `mrb_int` is: `time_t`,
 * `clockid_t` and FILETIME still stop at the port.
 *
 * `nsec` is always in [0, 999999999], whatever the platform counts in.
 */
typedef struct mrb_process_clock_time {
  int64_t sec;
  int64_t nsec;
} mrb_process_clock_time;

/* Nanoseconds in one second, the scale `nsec` above is a count in.  Darwin's
   <mach/clock_types.h> spells the same name (as 1000000000ull), so a
   definition already in scope is left standing. */
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000LL
#endif

/*
 * CPU time totals
 *
 * What Process.times reports: how much CPU time this process, and the
 * children it has already reaped, have spent in user and kernel mode.  Each
 * of the four travels the way a clock reading does, as an
 * mrb_process_clock_time, for the reasons given above it, plus one of its
 * own: mrb_float would not compile under MRB_NO_FLOAT.  Turning the four
 * into a Ruby Float, and building the Process::Tms they are answered as, is
 * Process.times's job once every port has answered the same shape; a port
 * is asked for nothing beyond the four readings themselves.
 */
typedef struct mrb_process_times {
  mrb_process_clock_time utime;   /* user CPU time this process has used */
  mrb_process_clock_time stime;   /* system CPU time this process has used */
  mrb_process_clock_time cutime;  /* user CPU time of reaped children */
  mrb_process_clock_time cstime;  /* system CPU time of reaped children */
} mrb_process_times;

/*
 * Spawn parameters
 *
 * Grouped in a struct so that later additions (process groups, umask,
 * resource limits) do not change every port's signature.
 */

typedef enum mrb_process_redirect_kind {
  MRB_PROCESS_REDIR_PARENT = 0,  /* duplicate a descriptor owned by the parent */
  MRB_PROCESS_REDIR_CHILD  = 1,  /* duplicate a child descriptor as set so far by this table */
  MRB_PROCESS_REDIR_CLOSE  = 2,  /* close in the child */
} mrb_process_redirect_kind;

typedef struct mrb_process_redirect {
  mrb_int child_fd;
  mrb_process_redirect_kind kind;
  mrb_int source_fd;             /* -1 for CLOSE */
} mrb_process_redirect;

typedef struct mrb_process_env_entry {
  const char *name;
  const char *value;             /* NULL means unset */
} mrb_process_env_entry;

typedef enum mrb_process_spawn_kind {
  MRB_PROCESS_SPAWN_ARGV  = 0,   /* execute argv directly */
  MRB_PROCESS_SPAWN_SHELL = 1,   /* execute argv[0] through the system shell */
} mrb_process_spawn_kind;

#define MRB_PROCESS_SPAWN_CLOSE_OTHERS    (1u << 0)
#define MRB_PROCESS_SPAWN_UNSETENV_OTHERS (1u << 1)

typedef struct mrb_process_spawn_params {
  mrb_process_spawn_kind kind;
  const char *const *argv;                  /* NULL-terminated; SHELL uses argv[0] only */
  const mrb_process_env_entry *env;         /* deltas against the parent environment */
  size_t nenv;
  const mrb_process_redirect *redirects;
  size_t nredirects;
  const char *chdir;                        /* NULL means inherit */
  unsigned int flags;
} mrb_process_spawn_params;

/*
 * HAL Interface - the interpreter's process context
 */

/*
 * Create the context every child of this interpreter is spawned into.
 *
 * @return 0 on success with *out set, -1 on error (sets errno)
 */
int mrb_hal_process_context_init(mrb_state *mrb, mrb_hal_process_context **out);

/*
 * Release the context.  The common layer has already released every child by
 * the time this is called; a port that finds any left over frees them rather
 * than leaking, and waits for none of them.
 */
void mrb_hal_process_context_free(mrb_state *mrb, mrb_hal_process_context *ctx);

/*
 * HAL Interface - process identity
 */

/* Process ID of the calling process.  Returns -1 with errno set on failure. */
mrb_int mrb_hal_process_pid(mrb_state *mrb);

/* Process ID of the parent.  Returns -1 with errno set where the platform
   cannot name a parent (errno ENOSYS when it has no such notion at all). */
mrb_int mrb_hal_process_ppid(mrb_state *mrb);

/*
 * Send a signal to a process.
 *
 * Signal 0 sends nothing and only reports whether the process may be
 * signalled, as POSIX `kill(2)` does.
 *
 * @return 0 on success, -1 on error (sets errno; ENOSYS where the platform
 *         cannot deliver this signal at all)
 */
int mrb_hal_process_kill(mrb_state *mrb, mrb_int pid, mrb_int signo);

/*
 * HAL Interface - children
 */

/*
 * Create a child process.
 *
 * The port applies `params->redirects` in order, so `{out: w, err: [:child,
 * :out]}` means `2>&1` after 1 has been redirected, and a CHILD entry naming
 * a descriptor the table has not touched names the inherited one.  A source
 * that collides with a target is saved out of the way first, and a
 * descriptor the table does not name is left alone: close-on-exec, not a
 * close-everything loop, is what keeps the child's descriptor table clean,
 * unless MRB_PROCESS_SPAWN_CLOSE_OTHERS says otherwise.
 *
 * A redirection the platform cannot express fails with ENOTSUP, so
 * capability differences stay inside the port.  A failure to execute the
 * command is reported here, through the return value and errno, rather than
 * left for the caller to guess from an exit status.
 *
 * @return 0 on success with *out holding a child registered in `ctx`,
 *         -1 on error (sets errno)
 */
int mrb_hal_process_spawn(mrb_state *mrb, mrb_hal_process_context *ctx,
                          const mrb_process_spawn_params *params,
                          mrb_hal_process_child **out);

/*
 * Which children a wait draws from.
 *
 * CHILD names a child this context spawned and still holds; the other three
 * are the platform's own selectors, and what they draw from is every child of
 * the *process*, which in an embedded interpreter is not only the ones spawned
 * through here.  A host application or a C extension that forked a child of
 * its own has put it in the same set, which is why a port that answers these
 * reports the child it drew from only when it is one of its own.
 */
typedef enum mrb_process_wait_scope {
  MRB_PROCESS_WAIT_SCOPE_CHILD,  /* the one named by `child` */
  MRB_PROCESS_WAIT_SCOPE_PID,    /* the process `pid` names, spawned here or not */
  MRB_PROCESS_WAIT_SCOPE_ANY,    /* any child of this process */
  MRB_PROCESS_WAIT_SCOPE_GROUP,  /* any child in `group`; 0 is the caller's own */
} mrb_process_wait_scope;

typedef struct mrb_process_wait_target {
  mrb_process_wait_scope scope;
  mrb_hal_process_child *child;  /* read when scope is MRB_PROCESS_WAIT_SCOPE_CHILD */
  mrb_int pid;                   /* read when scope is MRB_PROCESS_WAIT_SCOPE_PID */
  mrb_int group;                 /* read when scope is MRB_PROCESS_WAIT_SCOPE_GROUP */
} mrb_process_wait_target;

/*
 * Wait for a child to produce an event.
 *
 * @param target the set of children to draw from
 * @param flags  zero or more MRB_PROCESS_WAIT_* bits
 * @param event  out: what happened; kind is MRB_PROCESS_EVENT_NONE when
 *               MRB_PROCESS_WAIT_NOHANG found nothing ready
 * @return 0 on success, -1 on error (sets errno)
 *
 * The scopes are one primitive because they are one system call with a
 * different argument, and because emulating any of them from the others
 * cannot be done honestly: polling live children with a non-blocking wait in
 * a loop is not a blocking wait, and it burns the CPU while pretending
 * otherwise.  A port that cannot express a scope fails with ENOSYS rather
 * than answering a narrower question, except that a PID scope a port cannot
 * reach fails with ECHILD: what it cannot wait for is not its child.
 *
 * A terminal event releases the platform resource inside the port but does
 * not free the child object: the pointer in the event stays valid, and
 * readable, until the common layer calls mrb_hal_process_child_release().
 */
int mrb_hal_process_wait(mrb_state *mrb, mrb_hal_process_context *ctx,
                         const mrb_process_wait_target *target,
                         unsigned int flags, mrb_process_event *event);

/*
 * Let go of a child.
 *
 * Called once per child, when the common layer stops owning it, because it
 * was reaped or because the interpreter is closing with it still running.  The port frees the child object and whatever the
 * platform was holding for it, and waits for nothing.
 */
void mrb_hal_process_child_release(mrb_state *mrb, mrb_hal_process_context *ctx,
                                   mrb_hal_process_child *child);

/* The pid this child is labelled with.  Valid for mrb_hal_process_kill() and
   for Process::Status#pid while the child has not been released. */
mrb_int mrb_hal_process_child_pid(const mrb_hal_process_child *child);

/* A slot the common layer keeps its own record of the child in, so that a
   wait-any event can be resolved back to it without a table lookup.  The
   port stores and returns the pointer and never reads it. */
void mrb_hal_process_child_set_udata(mrb_hal_process_child *child, void *udata);
void *mrb_hal_process_child_udata(const mrb_hal_process_child *child);

/*
 * HAL Interface - reading a status that did not come from a wait
 */

/*
 * Read a platform wait status into its neutral form.
 *
 * Every field of `status` is written, including `pid` and `raw_status`, which
 * are copied from the arguments.  Decoding cannot fail: a status the platform
 * does not recognize decodes to flags of 0.
 *
 * A wait decodes its own status, so this is for the one caller that has a raw
 * status and no wait behind it: `Process::Status.new(pid, raw_status)`, which
 * is how mruby-io publishes `$?` after an `IO.popen` stream closes.
 */
void mrb_hal_process_status_decode(mrb_state *mrb, mrb_int pid, mrb_int raw_status,
                                   mrb_process_status *status);

/*
 * Read a clock.
 *
 * MRB_PROCESS_CLOCK_REALTIME is counted from the Unix epoch and may step,
 * backwards included, when the host's idea of the time is corrected.  The
 * other three are counted from an origin this interface does not name; a
 * port must keep whichever it uses fixed for the life of the process, since
 * subtracting two readings is the whole of what such a clock is for.
 *
 * @param clock_id  one of the mrb_process_clock_id values
 * @param t         out: the reading, with nsec normalized to [0, 999999999]
 * @return 0 on success, -1 on error, with errno set.  A clock the platform
 *         does not have is EINVAL; a port that could read no clock at all
 *         would answer ENOSYS, as an inexpressible wait pid does.
 */
int mrb_hal_process_clock_gettime(mrb_state *mrb, mrb_int clock_id,
                                  mrb_process_clock_time *t);

/*
 * The granularity a clock is read at: how finely the mechanism a reading
 * comes out of can tell two moments apart.  It describes the way the port
 * reads the clock, not the clock itself: the interval a reading is driven
 * by where the platform states one, and otherwise the unit a reading is
 * written in, which is the finest two of them can differ by.  A caller gets
 * a bound on what it can distinguish, never a period the clock is promised
 * to advance on, and a port must not answer anything finer than the
 * mechanism it used, since that promises a difference no two readings can
 * show.
 *
 * A reading says little without it, since a monotonic clock that moves every
 * 15ms and one that moves every 100ns are read the same way, so a clock a
 * port can read is one it can answer this for: whatever a reading came out
 * of has a granularity, even where the clock behind it does not state one.
 *
 * The looseness is the platforms', not this interface's.  POSIX's own
 * clock_getres(2) is a statement of the same kind, Linux answering 1ns for
 * clocks whose readings move in tens or hundreds of them, and CRuby reports
 * the granularity of what it emulated a clock out of: a microsecond for the
 * gettimeofday(2)-based wall clock, a tick for the times(2)-based CPU one.
 * A port that refused to answer wherever a platform would not commit to a
 * true period would refuse for nearly every clock on every platform.
 *
 * @param clock_id  one of the mrb_process_clock_id values
 * @param t         out: the granularity, never zero (the common layer
 *                  divides by it to answer `:hertz`), nsec in [0, 999999999]
 * @return 0 on success, -1 on error, as for a reading
 */
int mrb_hal_process_clock_getres(mrb_state *mrb, mrb_int clock_id,
                                 mrb_process_clock_time *t);

/*
 * Read the CPU time totals above.
 *
 * cutime and cstime cover only waited-for terminated children, whichever
 * wait(2)/waitpid(2) call reaped them: Process.wait, Process.waitpid, or one
 * this gem did not itself make (mruby-io's own reap on IO.popen(...).close,
 * say). A child still running, or one never waited for, is not in them,
 * which is what POSIX's times(2) reports too. A platform with no notion of
 * a child's CPU time answers 0 for both.
 *
 * @param t  out: the four readings
 * @return 0 on success, -1 on error (sets errno)
 */
int mrb_hal_process_times(mrb_state *mrb, mrb_process_times *t);

MRB_END_DECL

#endif /* MRUBY_PROCESS_HAL_H */
