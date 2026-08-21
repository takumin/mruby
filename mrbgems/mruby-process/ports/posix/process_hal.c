/*
** process_hal.c - POSIX HAL implementation for mruby-process
**
** See Copyright Notice in mruby.h
**
** POSIX implementation of the process HAL using getpid(2), getppid(2),
** fork(2)/exec(3), waitpid(2) and kill(2).
** Supported platforms: Linux, macOS, BSD, Unix
*/

#include <mruby.h>
#include "process_hal.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
# include <crt_externs.h>
# define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

/* An mrb_int is wider than a pid_t where mrb_int is 64-bit, so a pid from
   Ruby is range-checked rather than truncated into one. */
#define PID_FITS(pid) ((pid) >= (mrb_int)INT_MIN && (pid) <= (mrb_int)INT_MAX)

/*
 * Signal table
 *
 * Only the signals this platform actually defines make it in, so a name the
 * host lacks is reported as unknown rather than silently mapped elsewhere.
 */

struct signal_entry {
  const char *name;
  int signo;
};

static const struct signal_entry signal_table[] = {
#define SIGNAL_ENTRY(name) { #name, SIG##name },
#ifdef SIGHUP
  SIGNAL_ENTRY(HUP)
#endif
#ifdef SIGINT
  SIGNAL_ENTRY(INT)
#endif
#ifdef SIGQUIT
  SIGNAL_ENTRY(QUIT)
#endif
#ifdef SIGILL
  SIGNAL_ENTRY(ILL)
#endif
#ifdef SIGTRAP
  SIGNAL_ENTRY(TRAP)
#endif
#ifdef SIGABRT
  SIGNAL_ENTRY(ABRT)
#endif
#ifdef SIGBUS
  SIGNAL_ENTRY(BUS)
#endif
#ifdef SIGFPE
  SIGNAL_ENTRY(FPE)
#endif
#ifdef SIGKILL
  SIGNAL_ENTRY(KILL)
#endif
#ifdef SIGUSR1
  SIGNAL_ENTRY(USR1)
#endif
#ifdef SIGSEGV
  SIGNAL_ENTRY(SEGV)
#endif
#ifdef SIGUSR2
  SIGNAL_ENTRY(USR2)
#endif
#ifdef SIGPIPE
  SIGNAL_ENTRY(PIPE)
#endif
#ifdef SIGALRM
  SIGNAL_ENTRY(ALRM)
#endif
#ifdef SIGTERM
  SIGNAL_ENTRY(TERM)
#endif
#ifdef SIGCHLD
  SIGNAL_ENTRY(CHLD)
#endif
#ifdef SIGCONT
  SIGNAL_ENTRY(CONT)
#endif
#ifdef SIGSTOP
  SIGNAL_ENTRY(STOP)
#endif
#ifdef SIGTSTP
  SIGNAL_ENTRY(TSTP)
#endif
#ifdef SIGTTIN
  SIGNAL_ENTRY(TTIN)
#endif
#ifdef SIGTTOU
  SIGNAL_ENTRY(TTOU)
#endif
#ifdef SIGURG
  SIGNAL_ENTRY(URG)
#endif
#ifdef SIGXCPU
  SIGNAL_ENTRY(XCPU)
#endif
#ifdef SIGXFSZ
  SIGNAL_ENTRY(XFSZ)
#endif
#ifdef SIGVTALRM
  SIGNAL_ENTRY(VTALRM)
#endif
#ifdef SIGPROF
  SIGNAL_ENTRY(PROF)
#endif
#ifdef SIGWINCH
  SIGNAL_ENTRY(WINCH)
#endif
#ifdef SIGIO
  SIGNAL_ENTRY(IO)
#endif
#ifdef SIGSYS
  SIGNAL_ENTRY(SYS)
#endif
#undef SIGNAL_ENTRY
};

#define SIGNAL_TABLE_LEN (sizeof(signal_table) / sizeof(signal_table[0]))

/*
 * Context and children
 *
 * A POSIX child is its pid and nothing more -- the kernel keeps the status
 * slot the pid labels, and keeps it until the child is waited for.  The
 * context still holds the live children, because a wait-any has to report
 * *which* child it drew an event from, and only this list can turn the pid
 * waitpid() returned back into the object the common layer knows.
 */

struct mrb_hal_process_child {
  pid_t pid;
  void *udata;
  struct mrb_hal_process_child *next;
};

struct mrb_hal_process_context {
  struct mrb_hal_process_child *children;
};

int
mrb_hal_process_context_init(mrb_state *mrb, mrb_hal_process_context **out)
{
  mrb_hal_process_context *ctx =
    (mrb_hal_process_context*)mrb_malloc_simple(mrb, sizeof(mrb_hal_process_context));

  if (ctx == NULL) {
    errno = ENOMEM;
    return -1;
  }
  ctx->children = NULL;
  *out = ctx;
  return 0;
}

void
mrb_hal_process_context_free(mrb_state *mrb, mrb_hal_process_context *ctx)
{
  struct mrb_hal_process_child *child, *next;

  if (ctx == NULL) return;
  for (child = ctx->children; child != NULL; child = next) {
    next = child->next;
    mrb_free(mrb, child);
  }
  mrb_free(mrb, ctx);
}

static struct mrb_hal_process_child*
child_find(mrb_hal_process_context *ctx, pid_t pid)
{
  struct mrb_hal_process_child *child;

  for (child = ctx->children; child != NULL; child = child->next) {
    if (child->pid == pid) return child;
  }
  return NULL;
}

mrb_int
mrb_hal_process_child_pid(const mrb_hal_process_child *child)
{
  return (mrb_int)child->pid;
}

void
mrb_hal_process_child_set_udata(mrb_hal_process_child *child, void *udata)
{
  child->udata = udata;
}

void*
mrb_hal_process_child_udata(const mrb_hal_process_child *child)
{
  return child->udata;
}

void
mrb_hal_process_child_release(mrb_state *mrb, mrb_hal_process_context *ctx,
                              mrb_hal_process_child *child)
{
  struct mrb_hal_process_child **link;

  if (child == NULL) return;
  for (link = &ctx->children; *link != NULL; link = &(*link)->next) {
    if (*link == child) {
      *link = child->next;
      break;
    }
  }
  mrb_free(mrb, child);
}

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
 * Spawning
 */

#ifndef MRB_NO_PROCESS_SPAWN

static int
fd_set_cloexec(int fd, int on)
{
#if defined(F_GETFD) && defined(F_SETFD) && defined(FD_CLOEXEC)
  int flags = fcntl(fd, F_GETFD);

  if (flags == -1) return -1;
  flags = on ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
  return fcntl(fd, F_SETFD, flags);
#else
  (void)fd; (void)on;
  return 0;
#endif
}

/* Move `fd` above `least` and keep it out of the exec'd image.  Used for the
   descriptors the child needs but the redirection table is about to write
   over: the error pipe, and any source that is also a target. */
static int
fd_move_above(int fd, int least)
{
  int moved = fcntl(fd, F_DUPFD, least);

  if (moved == -1) return -1;
  if (fd_set_cloexec(moved, 1) == -1) {
    close(moved);
    return -1;
  }
  return moved;
}

/* Everything the child does between fork() and exec().  It reports a failure
   by writing errno down `errfd`, which the parent is reading; the write end
   is close-on-exec, so a successful exec closes it and the parent's read
   ends at EOF with nothing to report.

   Nothing here allocates: the one array it needs was allocated before the
   fork. */
static void
child_exec(const mrb_process_spawn_params *params, int *sources, int errfd)
{
  static char *empty_environ[] = { NULL };
  size_t i;
  int err;
  mrb_int max_target = -1;

  for (i = 0; i < params->nredirects; i++) {
    if (params->redirects[i].child_fd > max_target) {
      max_target = params->redirects[i].child_fd;
    }
  }

  /* Keep the error pipe out of the way of the descriptors about to be
     written, so that a redirection cannot silence the report. */
  if ((mrb_int)errfd <= max_target) {
    int moved = fd_move_above(errfd, (int)max_target + 1);
    if (moved == -1) goto fail;
    close(errfd);
    errfd = moved;
  }

  /* Save the parent-side sources that collide with a target.  dup2() is also
     defined to do nothing at all when its two arguments are equal, including
     nothing to the descriptor flags, and moving the source first is what
     keeps that case from quietly leaving FD_CLOEXEC set. */
  for (i = 0; i < params->nredirects; i++) {
    if (params->redirects[i].kind != MRB_PROCESS_REDIR_PARENT) continue;
    if ((mrb_int)sources[i] > max_target) continue;
    sources[i] = fd_move_above(sources[i], (int)max_target + 1);
    if (sources[i] == -1) goto fail;
  }

  /* Apply the table in order: a CHILD entry names the child's descriptor as
     the entries before it have left it. */
  for (i = 0; i < params->nredirects; i++) {
    const mrb_process_redirect *r = &params->redirects[i];
    int target = (int)r->child_fd;

    switch (r->kind) {
    case MRB_PROCESS_REDIR_CLOSE:
      close(target);
      break;
    case MRB_PROCESS_REDIR_PARENT:
      if (dup2(sources[i], target) == -1) goto fail;
      if (fd_set_cloexec(target, 0) == -1) goto fail;
      break;
    case MRB_PROCESS_REDIR_CHILD:
      if (dup2((int)r->source_fd, target) == -1) goto fail;
      if (fd_set_cloexec(target, 0) == -1) goto fail;
      break;
    }
  }

  if (params->flags & MRB_PROCESS_SPAWN_CLOSE_OTHERS) {
    long maxfd = sysconf(_SC_OPEN_MAX);
    int fd;

    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    for (fd = 3; fd < (int)maxfd; fd++) {
      if (fd != errfd) close(fd);
    }
  }

  if (params->chdir != NULL && chdir(params->chdir) == -1) goto fail;

  if (params->flags & MRB_PROCESS_SPAWN_UNSETENV_OTHERS) {
    environ = empty_environ;
  }
  for (i = 0; i < params->nenv; i++) {
    const mrb_process_env_entry *e = &params->env[i];
    int ok = (e->value != NULL) ? setenv(e->name, e->value, 1) : unsetenv(e->name);
    if (ok == -1) goto fail;
  }

  if (params->kind == MRB_PROCESS_SPAWN_SHELL) {
    execl("/bin/sh", "sh", "-c", params->argv[0], (char*)NULL);
  }
  else {
    execvp(params->argv[0], (char*const*)params->argv);
  }

fail:
  err = errno;
  if (err == 0) err = ENOEXEC;
  {
    /* One short write to a pipe nobody else is writing to; a partial write
       is not a case the parent has to read around. */
    ssize_t ignored = write(errfd, &err, sizeof(err));
    (void)ignored;
  }
  _exit(127);
}

/* Read what the child reported, if anything.  Returns the errno the child
   failed with, or 0 when the pipe reached EOF because exec succeeded. */
static int
read_child_error(int fd)
{
  char buf[sizeof(int)];
  size_t got = 0;

  while (got < sizeof(buf)) {
    ssize_t n = read(fd, buf + got, sizeof(buf) - got);
    if (n == -1) {
      if (errno == EINTR) continue;
      return 0;
    }
    if (n == 0) break;
    got += (size_t)n;
  }
  if (got < sizeof(buf)) return 0;

  {
    int err;
    memcpy(&err, buf, sizeof(err));
    return err;
  }
}

/* The command a shell is asked to run has to be more than blanks: /bin/sh
   would run an empty command happily, and "" is not a command. */
static int
command_is_blank(const char *cmd)
{
  if (cmd == NULL) return 1;
  while (*cmd == ' ' || *cmd == '\t' || *cmd == '\n' || *cmd == '\r') cmd++;
  return *cmd == '\0';
}

int
mrb_hal_process_spawn(mrb_state *mrb, mrb_hal_process_context *ctx,
                      const mrb_process_spawn_params *params,
                      mrb_hal_process_child **out)
{
  struct mrb_hal_process_child *child;
  int errfds[2] = { -1, -1 };
  int *sources = NULL;
  int child_errno, saved_errno;
  pid_t pid;
  size_t i;

  if (params->argv == NULL || params->argv[0] == NULL ||
      command_is_blank(params->argv[0])) {
    errno = ENOENT;
    return -1;
  }

  sources = (int*)mrb_malloc_simple(mrb, sizeof(int) * (params->nredirects + 1));
  child = (struct mrb_hal_process_child*)mrb_malloc_simple(mrb, sizeof(*child));
  if (sources == NULL || child == NULL) {
    errno = ENOMEM;
    goto error;
  }
  for (i = 0; i < params->nredirects; i++) {
    sources[i] = (int)params->redirects[i].source_fd;
  }

  if (pipe(errfds) == -1) goto error;
  if (fd_set_cloexec(errfds[0], 1) == -1 || fd_set_cloexec(errfds[1], 1) == -1) {
    goto error;
  }

  pid = fork();
  if (pid == -1) goto error;
  if (pid == 0) {
    close(errfds[0]);
    child_exec(params, sources, errfds[1]);
    /* not reached */
  }

  close(errfds[1]);
  errfds[1] = -1;
  child_errno = read_child_error(errfds[0]);
  close(errfds[0]);
  errfds[0] = -1;

  if (child_errno != 0) {
    /* The child never became the command, so it is this call's to clean up
       rather than a child the caller now owns. */
    int status;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
    errno = child_errno;
    goto error;
  }

  mrb_free(mrb, sources);
  child->pid = pid;
  child->udata = NULL;
  child->next = ctx->children;
  ctx->children = child;
  *out = child;
  return 0;

error:
  saved_errno = errno;
  if (errfds[0] != -1) close(errfds[0]);
  if (errfds[1] != -1) close(errfds[1]);
  mrb_free(mrb, sources);
  mrb_free(mrb, child);
  errno = saved_errno;
  return -1;
}

#else /* MRB_NO_PROCESS_SPAWN */

int
mrb_hal_process_spawn(mrb_state *mrb, mrb_hal_process_context *ctx,
                      const mrb_process_spawn_params *params,
                      mrb_hal_process_child **out)
{
  (void)mrb; (void)ctx; (void)params; (void)out;
  errno = ENOSYS;
  return -1;
}

#endif /* MRB_NO_PROCESS_SPAWN */

/*
 * Waiting
 */

/* Read a platform wait status into its neutral form.  Decoding stays here,
   in the port that produced the status, at the moment it was produced: no
   WIFEXITED and no raw layout is visible anywhere else. */
static void
status_decode(mrb_int pid, int raw, mrb_process_status *status)
{
  status->pid = pid;
  status->raw_status = (mrb_int)raw;
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
#ifdef WCOREDUMP
    if (WCOREDUMP(raw)) {
      status->flags |= MRB_PROCESS_STATUS_COREDUMP;
    }
#endif
  }
}

static mrb_process_event_kind
event_kind(const mrb_process_status *status)
{
  if (status->flags & MRB_PROCESS_STATUS_STOPPED) return MRB_PROCESS_EVENT_STOPPED;
  if (status->flags & MRB_PROCESS_STATUS_EXITED) return MRB_PROCESS_EVENT_EXITED;
  if (status->flags & MRB_PROCESS_STATUS_SIGNALED) return MRB_PROCESS_EVENT_SIGNALED;
  /* A status this platform does not classify still ended a wait, and the
     child that produced it is gone either way. */
  return MRB_PROCESS_EVENT_EXITED;
}

int
mrb_hal_process_wait(mrb_state *mrb, mrb_hal_process_context *ctx,
                     mrb_hal_process_child *child, unsigned int flags,
                     mrb_process_event *event)
{
  pid_t target = (child != NULL) ? child->pid : (pid_t)-1;
  pid_t result;
  int status = 0;
  int options = 0;
  (void)mrb;

  if (flags & MRB_PROCESS_WAIT_NOHANG) options |= WNOHANG;
  if (flags & MRB_PROCESS_WAIT_UNTRACED) options |= WUNTRACED;

  do {
    result = waitpid(target, &status, options);
  } while (result == -1 && errno == EINTR);

  if (result == -1) return -1;

  memset(event, 0, sizeof(*event));
  if (result == 0) {
    /* MRB_PROCESS_WAIT_NOHANG and nothing had happened */
    event->kind = MRB_PROCESS_EVENT_NONE;
    return 0;
  }

  status_decode((mrb_int)result, status, &event->status);
  event->kind = event_kind(&event->status);
  /* A wait-any can also draw a child this context never spawned -- one the
     host process forked itself.  Reporting it with no child attached says
     so, rather than guessing at an owner. */
  event->child = (child != NULL) ? child : child_find(ctx, result);
  return 0;
}

/*
 * Signalling
 */

int
mrb_hal_process_kill(mrb_state *mrb, mrb_int pid, mrb_int signo)
{
  (void)mrb;

  if (signo < 0 || signo > 255) {
    errno = EINVAL;
    return -1;
  }
  if (!PID_FITS(pid)) {
    errno = ESRCH;
    return -1;
  }
  return kill((pid_t)pid, (int)signo);
}

int
mrb_hal_process_signal_number(mrb_state *mrb, const char *name, mrb_int *signo)
{
  size_t i;
  (void)mrb;

  for (i = 0; i < SIGNAL_TABLE_LEN; i++) {
    if (strcmp(signal_table[i].name, name) == 0) {
      *signo = (mrb_int)signal_table[i].signo;
      return 0;
    }
  }
  return -1;
}

const char*
mrb_hal_process_signal_name(mrb_state *mrb, mrb_int signo)
{
  size_t i;
  (void)mrb;

  for (i = 0; i < SIGNAL_TABLE_LEN; i++) {
    if ((mrb_int)signal_table[i].signo == signo) {
      return signal_table[i].name;
    }
  }
  return NULL;
}
