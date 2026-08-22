/*
** process_hal.c - POSIX HAL implementation for mruby-process
**
** See Copyright Notice in mruby.h
**
** POSIX implementation of the process HAL using getpid(2), getppid(2),
** fork(2)/exec(3), waitpid(2), kill(2), clock_gettime(2) and
** clock_getres(2), falling back to gettimeofday(2) where the host has no
** POSIX clocks.
** Supported platforms: Linux, macOS, BSD, Unix
*/

/* pipe2() is not in POSIX, and where a host has it the C library may keep it
   behind a feature-test macro, which has to be asked for before any header
   is read.  A host without it creates the pipe in two calls instead. */
#if defined(__linux__)
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
# endif
# define MRB_PROCESS_HAS_PIPE2 1
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
      defined(__DragonFly__)
# define MRB_PROCESS_HAS_PIPE2 1
#endif

#include <mruby.h>
#include "process_hal.h"

#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Feature Capabilities
 *
 * Each MRB_PROCESS_HAVE_* is always defined, to 0 or 1, so the rest of this
 * file tests it with #if rather than #ifdef; the #ifndef guard around each
 * one lets a build override the detection below where it gets a host wrong.
 */

/* Whether this host has POSIX clocks.  _POSIX_TIMERS is the feature-test
   macro for POSIX's interval-timer option (timer_create(2) and friends),
   not for clock_gettime(2) itself, and Apple's libc leaves it undefined
   because it has never implemented that option, even though clock_gettime(2)
   and CLOCK_REALTIME/CLOCK_MONOTONIC have been there since macOS 10.12.
   Relying on _POSIX_TIMERS alone therefore misses a host that plainly has
   the call, so Apple is also asked for directly. Where the answer is no,
   the wall clock is still reachable through gettimeofday(2) and nothing
   else is. */
#ifndef MRB_PROCESS_HAVE_CLOCK_GETTIME
# if (defined(_POSIX_TIMERS) && (_POSIX_TIMERS + 0) > 0 && defined(CLOCK_REALTIME)) || \
     (defined(__APPLE__) && defined(CLOCK_REALTIME))
#  define MRB_PROCESS_HAVE_CLOCK_GETTIME 1
# else
#  define MRB_PROCESS_HAVE_CLOCK_GETTIME 0
# endif
#endif

/* CLOCK_MONOTONIC is absent on a few old hosts that still have
   CLOCK_REALTIME. */
#ifndef MRB_PROCESS_HAVE_CLOCK_MONOTONIC
# ifdef CLOCK_MONOTONIC
#  define MRB_PROCESS_HAVE_CLOCK_MONOTONIC 1
# else
#  define MRB_PROCESS_HAVE_CLOCK_MONOTONIC 0
# endif
#endif

/* The CPU-time clocks are both optional POSIX extensions. */
#ifndef MRB_PROCESS_HAVE_CLOCK_PROCESS_CPUTIME
# ifdef CLOCK_PROCESS_CPUTIME_ID
#  define MRB_PROCESS_HAVE_CLOCK_PROCESS_CPUTIME 1
# else
#  define MRB_PROCESS_HAVE_CLOCK_PROCESS_CPUTIME 0
# endif
#endif

#ifndef MRB_PROCESS_HAVE_CLOCK_THREAD_CPUTIME
# ifdef CLOCK_THREAD_CPUTIME_ID
#  define MRB_PROCESS_HAVE_CLOCK_THREAD_CPUTIME 1
# else
#  define MRB_PROCESS_HAVE_CLOCK_THREAD_CPUTIME 0
# endif
#endif

/* Whether WIFSIGNALED's status can also say the process dumped core; not
   every host's <sys/wait.h> defines this. */
#ifndef MRB_PROCESS_HAVE_WCOREDUMP
# ifdef WCOREDUMP
#  define MRB_PROCESS_HAVE_WCOREDUMP 1
# else
#  define MRB_PROCESS_HAVE_WCOREDUMP 0
# endif
#endif

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
 * Context and children
 *
 * A POSIX child is its pid and nothing more, since the kernel keeps the status
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

/*
 * The pipe a failed exec is reported down.
 *
 * It has to be close-on-exec, and creating it that way is one call where the
 * host has pipe2(): with pipe() and fcntl() there is a window in which
 * another thread of the embedding process can fork and take a copy of a
 * descriptor that was never meant to leave this one, and a copy of the write
 * end left open in an unrelated child is a read that never reaches EOF.
 */
static int
errpipe(int fds[2])
{
#if defined(MRB_PROCESS_HAS_PIPE2)
  if (pipe2(fds, O_CLOEXEC) == 0) return 0;
  if (errno != ENOSYS) return -1;
  /* A kernel older than the C library it was built against says ENOSYS, and
     the two calls below are what it has instead. */
#endif
  if (pipe(fds) == -1) return -1;
  if (fd_set_cloexec(fds[0], 1) == -1 || fd_set_cloexec(fds[1], 1) == -1) {
    int err = errno;
    close(fds[0]);
    close(fds[1]);
    fds[0] = fds[1] = -1;
    errno = err;
    return -1;
  }
  return 0;
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

/* Add `fd` to the ascending `keep` list, which holds no duplicates and
   nothing below 3.  Called after fork(), so it does no more than compare and
   move `int`s. */
static void
keep_add(int *keep, size_t *nkeep, int fd)
{
  size_t i, j;

  if (fd < 3) return;
  for (i = 0; i < *nkeep; i++) {
    if (keep[i] == fd) return;
    if (keep[i] > fd) break;
  }
  for (j = *nkeep; j > i; j--) keep[j] = keep[j - 1];
  keep[i] = fd;
  (*nkeep)++;
}

/* Close every descriptor from `low` up to and including `high`, or to the top
   of the table when `high` is -1.  Where the top is is what the parent read
   _SC_OPEN_MAX for: a sweep that stopped at a number picked here would leave
   open exactly what the caller asked to have closed. */
static void
close_fds(int low, int high, long maxfd)
{
  int fd;

  if (high < 0) {
    high = (maxfd - 1 > (long)INT_MAX) ? INT_MAX : (int)(maxfd - 1);
  }
  for (fd = low; fd <= high; fd++) close(fd);
}

/*
 * What the child will execute, worked out before the fork
 *
 * A child between fork() and exec() may call only what is async-signal-safe.
 * mruby needs no threads of its own for that to matter: it is an embeddable
 * VM, so the process it runs in may already have others, and a fork made
 * while one of them holds a lock leaves the child holding a lock nobody will
 * ever release.  So the environment is assembled here rather than through
 * setenv() and unsetenv() over there, and the PATH is walked here rather
 * than by execvp().  What is left for the child is dup2(), close(), chdir()
 * and execve().
 */
struct exec_plan {
  const char **argv;     /* what the image is run with */
  const char **path;     /* images to try, in order; NULL-terminated */
  const char **sh_argv;  /* what ENOEXEC falls back to, or NULL; the child
                            writes the image it found into slot 1 */
  char **envp;           /* the child's whole environment */
  char **owned;          /* every string allocated for the above */
};

/* "<a><sep><b>", with no separator when `sep` is 0. */
static char*
str_join(mrb_state *mrb, const char *a, size_t alen, char sep, const char *b, size_t blen)
{
  char *s = (char*)mrb_malloc_simple(mrb, alen + (sep ? 1 : 0) + blen + 1);
  size_t n = alen;

  if (s == NULL) return NULL;
  memcpy(s, a, alen);
  if (sep) s[n++] = sep;
  memcpy(s + n, b, blen);
  s[n + blen] = '\0';
  return s;
}

/* Whether `entry`, which is "NAME=VALUE", is the one for `name`. */
static int
env_entry_is(const char *entry, const char *name, size_t namelen)
{
  return strncmp(entry, name, namelen) == 0 && entry[namelen] == '=';
}

static void
plan_free(mrb_state *mrb, struct exec_plan *plan)
{
  size_t i;

  if (plan->owned != NULL) {
    for (i = 0; plan->owned[i] != NULL; i++) mrb_free(mrb, plan->owned[i]);
    mrb_free(mrb, plan->owned);
  }
  if (plan->envp != environ) mrb_free(mrb, plan->envp);
  mrb_free(mrb, (void*)plan->argv);
  mrb_free(mrb, (void*)plan->path);
  mrb_free(mrb, (void*)plan->sh_argv);
  memset(plan, 0, sizeof(*plan));
}

/* The PATH a bare command name is looked up on is the child's, not this
   process's, so the deltas are read before the parent's own is. */
static const char*
plan_path_env(const mrb_process_spawn_params *params)
{
  size_t i;

  for (i = 0; i < params->nenv; i++) {
    if (strcmp(params->env[i].name, "PATH") == 0) return params->env[i].value;
  }
  if (params->flags & MRB_PROCESS_SPAWN_UNSETENV_OTHERS) return NULL;
  return getenv("PATH");
}

static size_t
count_elements(const char *s)
{
  size_t n = 1;

  for (; *s != '\0'; s++) {
    if (*s == ':') n++;
  }
  return n;
}

/* Build the plan.  Returns 0, or -1 with errno set. */
static int
plan_build(mrb_state *mrb, const mrb_process_spawn_params *params, struct exec_plan *plan)
{
  const char *path_env = plan_path_env(params);
  const char *name;
  size_t argc = 0, nparent = 0, nowned = 0, nenvp = 0, npath = 1, namelen, i, j;
  int keep_parent = !(params->flags & MRB_PROCESS_SPAWN_UNSETENV_OTHERS);

  memset(plan, 0, sizeof(*plan));
  while (params->argv[argc] != NULL) argc++;
  if (keep_parent) {
    while (environ[nparent] != NULL) nparent++;
  }

  name = params->argv[0];
  namelen = strlen(name);
  if (params->kind == MRB_PROCESS_SPAWN_ARGV && strchr(name, '/') == NULL) {
    npath = (path_env != NULL) ? count_elements(path_env) : 1;
  }

  /* One allocation for each array the child reads, and one for the strings
     inside them.  The strings are the environment entries the deltas add,
     the candidate images, and the default PATH where one has to be asked
     for, which is the extra slot. */
  plan->owned = (char**)mrb_malloc_simple(mrb, sizeof(char*) * (params->nenv + npath + 2));
  plan->path = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (npath + 1));
  plan->argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (argc + 4));
  if (plan->owned == NULL || plan->path == NULL || plan->argv == NULL) goto nomem;
  plan->owned[0] = NULL;

  if (keep_parent && params->nenv == 0) {
    plan->envp = environ;
  }
  else {
    plan->envp = (char**)mrb_malloc_simple(mrb, sizeof(char*) * (nparent + params->nenv + 1));
    if (plan->envp == NULL) goto nomem;
    for (i = 0; i < nparent; i++) {
      /* An entry the deltas name is written below, or, where the delta unsets
         it, not written at all. */
      int overridden = 0;
      for (j = 0; j < params->nenv; j++) {
        if (env_entry_is(environ[i], params->env[j].name, strlen(params->env[j].name))) {
          overridden = 1;
          break;
        }
      }
      if (!overridden) plan->envp[nenvp++] = environ[i];
    }
    for (i = 0; i < params->nenv; i++) {
      const mrb_process_env_entry *e = &params->env[i];
      char *entry;

      if (e->value == NULL) continue;
      entry = str_join(mrb, e->name, strlen(e->name), '=', e->value, strlen(e->value));
      if (entry == NULL) goto nomem;
      plan->owned[nowned++] = entry;
      plan->owned[nowned] = NULL;
      plan->envp[nenvp++] = entry;
    }
    plan->envp[nenvp] = NULL;
  }

  if (params->kind == MRB_PROCESS_SPAWN_SHELL) {
    /* One string, taken apart by the shell, which is whose work that is. */
    plan->argv[0] = "sh";
    plan->argv[1] = "-c";
    plan->argv[2] = params->argv[0];
    plan->argv[3] = NULL;
    plan->path[0] = "/bin/sh";
    plan->path[1] = NULL;
    return 0;
  }

  for (i = 0; i <= argc; i++) plan->argv[i] = params->argv[i];

  if (strchr(name, '/') != NULL) {
    plan->path[0] = name;
    plan->path[1] = NULL;
    return 0;
  }

  {
    size_t n = 0;
    const char *p;

    if (path_env == NULL) {
      /* No PATH to look on is not no places to look: execvp() falls back to
         the one the host names, and a host that names none leaves the two
         directories every system has. */
      size_t len = confstr(_CS_PATH, NULL, 0);

      if (len > 0) {
        char *def = (char*)mrb_malloc_simple(mrb, len);

        if (def == NULL) goto nomem;
        confstr(_CS_PATH, def, len);
        plan->owned[nowned++] = def;
        plan->owned[nowned] = NULL;
        path_env = def;
      }
      else {
        path_env = "/bin:/usr/bin";
      }
    }
    for (p = path_env; ; ) {
      const char *sep = strchr(p, ':');
      size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
      char *cand;

      /* An empty element is the current directory, as it is for execvp(). */
      cand = (len == 0) ? str_join(mrb, ".", 1, '/', name, namelen)
                        : str_join(mrb, p, len, '/', name, namelen);
      if (cand == NULL) goto nomem;
      plan->owned[nowned++] = cand;
      plan->owned[nowned] = NULL;
      plan->path[n++] = cand;
      if (sep == NULL) break;
      p = sep + 1;
    }
    plan->path[n] = NULL;
  }

  /* What execvp() does with a file that is not an executable image: hand it
     to the shell, arguments and all.  Which image that was is known only
     once one has been tried, so the child fills that slot in. */
  plan->sh_argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (argc + 3));
  if (plan->sh_argv == NULL) goto nomem;
  plan->sh_argv[0] = "sh";
  plan->sh_argv[1] = NULL;
  for (i = 1; i < argc; i++) plan->sh_argv[i + 1] = params->argv[i];
  plan->sh_argv[argc + 1] = NULL;
  return 0;

nomem:
  plan_free(mrb, plan);
  errno = ENOMEM;
  return -1;
}

/* Everything the child does between fork() and exec().  It reports a failure
   by writing errno down `errfd`, which the parent is reading; the write end
   is close-on-exec, so a successful exec closes it and the parent's read
   ends at EOF with nothing to report.

   Nothing here allocates: the one array it needs was allocated before the
   fork. */
static void
child_exec(const mrb_process_spawn_params *params, const struct exec_plan *plan,
           int *sources, int errfd, int *keep, long maxfd)
{
  size_t i;
  int err;
  int low = 3;
  int denied = 0;
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
    /* "Everything else" is everything the caller did not ask for.  What it
       did ask for is each descriptor this table wrote, and the error pipe,
       which closing would silence the report this child owes its parent.
       Sweeping over those would undo the redirection just performed: an
       explicit `3 => io` would create descriptor 3 and then close it. */
    size_t nkeep = 0;
    size_t k;

    for (i = 0; i < params->nredirects; i++) {
      if (params->redirects[i].kind == MRB_PROCESS_REDIR_CLOSE) continue;
      keep_add(keep, &nkeep, (int)params->redirects[i].child_fd);
    }
    keep_add(keep, &nkeep, errfd);

    for (k = 0; k < nkeep; k++) {
      if (low < keep[k]) close_fds(low, keep[k] - 1, maxfd);
      low = keep[k] + 1;
    }
    close_fds(low, -1, maxfd);
  }

  if (params->chdir != NULL && chdir(params->chdir) == -1) goto fail;

  /* The command, tried at each name the parent worked out for it.  What
     execvp() does with the PATH is done there, before the fork, because none
     of it is safe to do here. */
  for (i = 0; plan->path[i] != NULL; i++) {
    execve(plan->path[i], (char*const*)plan->argv, (char*const*)plan->envp);
    switch (errno) {
    case ENOEXEC:
      /* A file that is not an executable image is handed to the shell, as
         execvp() hands it to one.  Nothing is tried after that: the shell
         either runs it or it does not. */
      plan->sh_argv[1] = plan->path[i];
      execve("/bin/sh", (char*const*)plan->sh_argv, (char*const*)plan->envp);
      goto fail;
    case EACCES:
      /* Something of that name is there and cannot be run.  Later names may
         still work, and this is what is reported if none of them does. */
      denied = 1;
      break;
    case ENOENT:
    case ENOTDIR:
      break;
    default:
      goto fail;
    }
  }
  if (denied) errno = EACCES;

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
  struct exec_plan plan;
  int errfds[2] = { -1, -1 };
  int *sources = NULL;
  int *keep = NULL;
  long maxfd;
  int child_errno, saved_errno;
  pid_t pid;
  size_t i;

  memset(&plan, 0, sizeof(plan));
  if (params->argv == NULL || params->argv[0] == NULL ||
      command_is_blank(params->argv[0])) {
    errno = ENOENT;
    return -1;
  }

  /* Both arrays belong to the child as much as to this call: it may not
     allocate between fork() and exec(), so what it will need is allocated
     here.  `sources` is one per redirection, `keep` one per redirection plus
     the error pipe.  How high descriptors go is read here for the same
     reason, since sysconf() is not a call the child may make. */
  sources = (int*)mrb_malloc_simple(mrb, sizeof(int) * (params->nredirects + 1));
  keep = (int*)mrb_malloc_simple(mrb, sizeof(int) * (params->nredirects + 1));
  child = (struct mrb_hal_process_child*)mrb_malloc_simple(mrb, sizeof(*child));
  if (sources == NULL || keep == NULL || child == NULL) {
    errno = ENOMEM;
    goto error;
  }
  for (i = 0; i < params->nredirects; i++) {
    sources[i] = (int)params->redirects[i].source_fd;
  }
  maxfd = sysconf(_SC_OPEN_MAX);
  /* A host that will not say has to be guessed at, and the guess is only
     ever a floor: nothing above it can be closed, so it is made high enough
     to cover what a process is likely to have open. */
  if (maxfd < 0) maxfd = 65536;

  if (plan_build(mrb, params, &plan) != 0) goto error;

  if (errpipe(errfds) == -1) goto error;

  pid = fork();
  if (pid == -1) goto error;
  if (pid == 0) {
    close(errfds[0]);
    child_exec(params, &plan, sources, errfds[1], keep, maxfd);
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

  plan_free(mrb, &plan);
  mrb_free(mrb, sources);
  mrb_free(mrb, keep);
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
  plan_free(mrb, &plan);
  mrb_free(mrb, sources);
  mrb_free(mrb, keep);
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
#if MRB_PROCESS_HAVE_WCOREDUMP
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
                     const mrb_process_wait_target *target, unsigned int flags,
                     mrb_process_event *event)
{
  pid_t want;
  pid_t result;
  int status = 0;
  int options = 0;
  (void)mrb;

  /* The scopes are the things waitpid(2) reads from its first argument, so
     they are the same call with a different number. */
  switch (target->scope) {
  case MRB_PROCESS_WAIT_SCOPE_CHILD:
    want = target->child->pid;
    break;
  case MRB_PROCESS_WAIT_SCOPE_PID:
    /* A number no pid_t can hold labels no process, and so no child of this
       one: the answer waitpid(2) gives for such a pid is the one given
       here. */
    if (!PID_FITS(target->pid)) {
      errno = ECHILD;
      return -1;
    }
    want = (pid_t)target->pid;
    break;
  case MRB_PROCESS_WAIT_SCOPE_ANY:
    want = (pid_t)-1;
    break;
  default:
    /* A group no pid_t can hold names no group this process is in, and so no
       child of it: the answer waitpid(2) gives for such a group is the one
       given here. */
    if (!PID_FITS(target->group)) {
      errno = ECHILD;
      return -1;
    }
    want = (target->group == 0) ? 0 : -(pid_t)target->group;
    break;
  }

  if (flags & MRB_PROCESS_WAIT_NOHANG) options |= WNOHANG;
  if (flags & MRB_PROCESS_WAIT_UNTRACED) options |= WUNTRACED;

  do {
    result = waitpid(want, &status, options);
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
  /* Every scope but CHILD draws from this process's children, which is a
     wider set than the ones spawned through here: the host application may
     have forked its own.  Reporting such a child with none attached says so,
     rather than guessing at an owner. */
  event->child = (target->scope == MRB_PROCESS_WAIT_SCOPE_CHILD)
                   ? target->child
                   : child_find(ctx, result);
  return 0;
}

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
 * Clocks
 */

#if MRB_PROCESS_HAVE_CLOCK_GETTIME
/* Which clockid_t stands for one of mruby's clocks here.  A clock this host
   does not have is left out, and the caller answers EINVAL for it. */
static int
posix_clock_id(mrb_int clock_id, clockid_t *out)
{
  switch (clock_id) {
  case MRB_PROCESS_CLOCK_REALTIME:
    *out = CLOCK_REALTIME;
    return 0;
#if MRB_PROCESS_HAVE_CLOCK_MONOTONIC
  case MRB_PROCESS_CLOCK_MONOTONIC:
    *out = CLOCK_MONOTONIC;
    return 0;
#endif
#if MRB_PROCESS_HAVE_CLOCK_PROCESS_CPUTIME
  case MRB_PROCESS_CLOCK_PROCESS_CPUTIME:
    *out = CLOCK_PROCESS_CPUTIME_ID;
    return 0;
#endif
#if MRB_PROCESS_HAVE_CLOCK_THREAD_CPUTIME
  case MRB_PROCESS_CLOCK_THREAD_CPUTIME:
    *out = CLOCK_THREAD_CPUTIME_ID;
    return 0;
#endif
  default:
    break;
  }
  return -1;
}
#endif /* MRB_PROCESS_HAVE_CLOCK_GETTIME */

int
mrb_hal_process_clock_gettime(mrb_state *mrb, mrb_int clock_id,
                              mrb_process_clock_time *t)
{
#if MRB_PROCESS_HAVE_CLOCK_GETTIME
  clockid_t c;
  struct timespec ts;
  (void)mrb;

  if (posix_clock_id(clock_id, &c) != 0) {
    errno = EINVAL;
    return -1;
  }
  if (clock_gettime(c, &ts) != 0) return -1;
  t->sec = (int64_t)ts.tv_sec;
  t->nsec = (int64_t)ts.tv_nsec;
  return 0;
#else
  /* Without POSIX clocks the wall clock is the only one there is, and
     gettimeofday(2) reads it to the microsecond. */
  struct timeval tv;
  (void)mrb;

  if (clock_id != MRB_PROCESS_CLOCK_REALTIME) {
    errno = EINVAL;
    return -1;
  }
  if (gettimeofday(&tv, NULL) != 0) return -1;
  t->sec = (int64_t)tv.tv_sec;
  t->nsec = (int64_t)tv.tv_usec * 1000;
  return 0;
#endif
}

int
mrb_hal_process_clock_getres(mrb_state *mrb, mrb_int clock_id,
                             mrb_process_clock_time *t)
{
#if MRB_PROCESS_HAVE_CLOCK_GETTIME
  clockid_t c;
  struct timespec ts;
  (void)mrb;

  if (posix_clock_id(clock_id, &c) != 0) {
    errno = EINVAL;
    return -1;
  }
  if (clock_getres(c, &ts) != 0) return -1;
  t->sec = (int64_t)ts.tv_sec;
  t->nsec = (int64_t)ts.tv_nsec;
  return 0;
#else
  /* A microsecond, which is what gettimeofday(2) writes its answer in and so
     how finely this port reads the wall clock where that is the only way to
     read it.  CRuby reports the same number for its own gettimeofday(2)-based
     clock. */
  (void)mrb;

  if (clock_id != MRB_PROCESS_CLOCK_REALTIME) {
    errno = EINVAL;
    return -1;
  }
  t->sec = 0;
  t->nsec = 1000;
  return 0;
#endif
}
