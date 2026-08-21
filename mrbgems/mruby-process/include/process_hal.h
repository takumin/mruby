/*
** process_hal.h - Process Hardware Abstraction Layer (HAL)
**
** See Copyright Notice in mruby.h
**
** This header defines the HAL interface for platform-specific process
** operations.  A port under mruby-process/ports/<port_name>/ -- or an
** external provider gem named hal-process-<conf> -- supplies every function
** declared here.
**
** The HAL answers OS-level facts and performs OS-level operations.  It knows
** nothing about `Process::Status`, `$?`, `$$`, blocks or any other Ruby
** notion: those belong to the common sources under src/.  In the other
** direction, no platform type or macro (`pid_t`, `WIFEXITED`, `SIGTERM`,
** `WNOHANG`, ...) crosses into the common layer -- process and signal
** numbers travel as `mrb_int`, wait options as the MRB_PROCESS_WAIT_* bits
** below, and a wait result as an `mrb_process_event` carrying an already
** decoded `mrb_process_status`.
**
** What a child *is* stays behind the HAL as well.  A pid labels a child; it
** is not the child, because the OS may hand the same number to another
** process once the first one has been reaped.  So spawn hands back an opaque
** `mrb_hal_process_child`, every wait takes that object rather than a
** number, and the port keeps whatever it really needs inside it -- a pid on
** POSIX, a HANDLE on Windows.
*/

#ifndef MRUBY_PROCESS_HAL_H
#define MRUBY_PROCESS_HAL_H

#include <mruby.h>

#include <stddef.h>

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
   kept so that `Process::Status#to_i` can hand it back unchanged; it is
   opaque and platform-defined, and nothing above the port reads it.

   The status is a snapshot rather than a question to be re-asked, because a
   `Process::Status` outlives the child it came from. */
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
 * they are mruby's own -- a port translates them to whatever its platform
 * spells them as.
 */
#define MRB_PROCESS_WAIT_NOHANG   (1u << 0)  /* return at once if nothing is ready */
#define MRB_PROCESS_WAIT_UNTRACED (1u << 1)  /* report stopped children too */

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
                                     when a wait-any reaped one this context
                                     never spawned */
  mrb_process_status status;      /* valid unless kind is MRB_PROCESS_EVENT_NONE */
} mrb_process_event;

/*
 * Spawn parameters
 *
 * Grouped in a struct so that later additions -- process groups, umask,
 * resource limits -- do not change every port's signature.
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

#define MRB_PROCESS_SPAWN_UNSETENV_OTHERS (1u << 0)
#define MRB_PROCESS_SPAWN_CLOSE_OTHERS    (1u << 1)

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
 * Resolve a signal name to its number on this platform.
 *
 * @param name  a bare name such as "TERM", without the "SIG" prefix, which
 *              the common layer has already stripped
 * @return 0 with *signo set, or -1 when the platform has no such signal
 */
int mrb_hal_process_signal_number(mrb_state *mrb, const char *name, mrb_int *signo);

/*
 * Name the signal `signo` stands for on this platform.
 *
 * @return a static bare name such as "TERM", or NULL when the number names
 *         no signal here.  The caller must not free it.
 */
const char *mrb_hal_process_signal_name(mrb_state *mrb, mrb_int signo);

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
 * descriptor the table does not name is left alone -- close-on-exec, not a
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
 * Wait for a child to produce an event.
 *
 * @param child  the child to wait for, or NULL to wait for any child of this
 *               context -- the only two sets a wait can express
 * @param flags  zero or more MRB_PROCESS_WAIT_* bits
 * @param event  out: what happened; kind is MRB_PROCESS_EVENT_NONE when
 *               MRB_PROCESS_WAIT_NOHANG found nothing ready
 * @return 0 on success, -1 on error (sets errno)
 *
 * A terminal event releases the platform resource inside the port but does
 * not free the child object: the pointer in the event stays valid, and
 * readable, until the common layer calls mrb_hal_process_child_release().
 */
int mrb_hal_process_wait(mrb_state *mrb, mrb_hal_process_context *ctx,
                         mrb_hal_process_child *child, unsigned int flags,
                         mrb_process_event *event);

/*
 * Let go of a child.
 *
 * Called once per child, when the common layer stops owning it -- because it
 * was reaped, because it was detached, or because the interpreter is closing
 * with it still running.  The port frees the child object and whatever the
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

#endif /* MRUBY_PROCESS_HAL_H */
