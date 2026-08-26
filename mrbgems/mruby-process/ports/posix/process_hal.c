/*
** process_hal.c - POSIX HAL implementation for mruby-process
**
** See Copyright Notice in mruby.h
**
** POSIX implementation of the process HAL using getpid(2), getppid(2),
** fork(2)/exec(3), waitpid(2), kill(2), clock_gettime(2) and clock_getres(2),
** falling back to gettimeofday(2) where the host has no POSIX clocks, and
** getrusage(2) for the CPU time totals behind Process.times, falling back to
** times(2) scaled by sysconf(_SC_CLK_TCK) where the host has no getrusage(2).
** Supported platforms: Linux, macOS, BSD, Unix
*/

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

/* The environment this process runs in, which is what a child inherits: the
   deltas a caller asks for are applied to a copy of it, and what comes out is
   what the child is handed, since neither exec nor posix_spawn() leaves a
   child to work one out.  Apple keeps it behind a call in a library whose
   image may be shared, where a variable could not be. */
#if defined(__APPLE__)
# include <crt_externs.h>
# define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

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

/* Whether this host has a posix_spawn() that answers for the exec.
   posix_spawn() creates the child and executes the image in the one call, so
   a spawn made with it needs neither a fork() in a process whose other
   threads this VM knows nothing about, nor a pipe to carry a failed exec
   back.  What it does need is that the call itself report that failure:
   POSIX allows an implementation to create the child, let it exit 127 and say
   nothing, which would leave a command that could not be run looking exactly
   like a command that exited 127 of its own accord.  Whether a host does that
   is not something a compile can ask, so it is named here rather than
   probed.  Darwin reports it because posix_spawn() is a system call there;
   glibc reports it from 2.24 on, through the page it shares with the child it
   vforks.  A build whose host does the same can define this itself.
   HAVE_POSIX_SPAWN, from mrbgem.rake, is whether the call is there at all.

   Defining it to 0 is how a build says the other thing, and what the host is
   is not all that decides it: under valgrind 3.27 a posix_spawn() whose exec
   failed returns 0, so a run under that tool wants the fork path, which
   reports through a pipe of its own and does not depend on the host at
   all. */
#ifndef MRB_PROCESS_HAVE_POSIX_SPAWN
# ifdef HAVE_POSIX_SPAWN
#  if defined(__APPLE__)
#   define MRB_PROCESS_HAVE_POSIX_SPAWN 1
#  elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#   if __GLIBC_PREREQ(2, 24)
#    define MRB_PROCESS_HAVE_POSIX_SPAWN 1
#   endif
#  endif
# endif
# ifndef MRB_PROCESS_HAVE_POSIX_SPAWN
#  define MRB_PROCESS_HAVE_POSIX_SPAWN 0
# endif
#endif
#if MRB_PROCESS_HAVE_POSIX_SPAWN
# include <spawn.h>
#endif

/* Whether this host has getrusage(2), which is how Process.times reads CPU
   time here: RUSAGE_SELF and RUSAGE_CHILDREN are both XSI extensions
   (SUSv2), present on Linux, macOS and the BSDs but not guaranteed by base
   POSIX.1, so a host missing either falls back to times(2).

   <sys/resource.h> is part of that same XSI extension; whether it is there
   is asked of the compiler by the gem's mrbgem.rake, for the reason given
   there, and answered as HAVE_SYS_RESOURCE_H.
   Overriding MRB_PROCESS_HAVE_GETRUSAGE to 1 asserts the call is there,
   which it can only be where the header is, so HAVE_SYS_RESOURCE_H goes
   with such an override; overriding it to 0 stands on its own. */
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifndef MRB_PROCESS_HAVE_GETRUSAGE
# if defined(RUSAGE_SELF) && defined(RUSAGE_CHILDREN)
#  define MRB_PROCESS_HAVE_GETRUSAGE 1
# else
#  define MRB_PROCESS_HAVE_GETRUSAGE 0
# endif
#endif

/* times(2) is only reached where getrusage(2) is not, so <sys/times.h> is
   only asked for there: a host that has the XSI call needs nothing from
   this header, and is not made to have it. */
#if !MRB_PROCESS_HAVE_GETRUSAGE
# include <sys/times.h>
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
  char *path_default = NULL;
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
    if (path_env == NULL) {
      /* No PATH to look on is not no places to look: execvp() falls back to
         the one the host names, and a host that names none leaves the two
         directories every system has.  Resolved before the arrays are sized,
         so that what is counted is what is walked. */
      size_t len = confstr(_CS_PATH, NULL, 0);

      if (len > 0) {
        path_default = (char*)mrb_malloc_simple(mrb, len);
        if (path_default == NULL) goto nomem;
        confstr(_CS_PATH, path_default, len);
        path_env = path_default;
      }
      else {
        path_env = "/bin:/usr/bin";
      }
    }
    npath = count_elements(path_env);
  }

  /* One allocation for each array the child reads, and one for the strings
     inside them.  The strings are the environment entries the deltas add, the
     candidate images, and the default PATH where one had to be asked for,
     which is the extra slot.

     Taken one at a time and terminated as it goes: what plan_free() walks to
     find the strings is `owned` itself, so it has to be a list before
     anything that can fail comes after it. */
  plan->owned = (char**)mrb_malloc_simple(mrb, sizeof(char*) * (params->nenv + npath + 2));
  if (plan->owned == NULL) goto nomem;
  plan->owned[0] = NULL;
  plan->path = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (npath + 1));
  if (plan->path == NULL) goto nomem;
  plan->argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (argc + 4));
  if (plan->argv == NULL) goto nomem;
  if (path_default != NULL) {
    plan->owned[nowned++] = path_default;
    plan->owned[nowned] = NULL;
    path_default = NULL;   /* the plan frees it from here on */
  }

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

  /* What execvp() does with a file that is not an executable image: hand it
     to the shell, arguments and all.  Which image that was is known only
     once one has been tried, so the child fills that slot in.  Built before
     the images are chosen, because any of them can turn out to be one,
     including the single image a name that is a path names. */
  plan->sh_argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (argc + 3));
  if (plan->sh_argv == NULL) goto nomem;
  plan->sh_argv[0] = "sh";
  plan->sh_argv[1] = NULL;
  for (i = 1; i < argc; i++) plan->sh_argv[i + 1] = params->argv[i];
  plan->sh_argv[argc + 1] = NULL;

  if (strchr(name, '/') != NULL) {
    plan->path[0] = name;
    plan->path[1] = NULL;
    return 0;
  }

  {
    size_t n = 0;
    const char *p;

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
  return 0;

nomem:
  mrb_free(mrb, path_default);
  plan_free(mrb, plan);
  errno = ENOMEM;
  return -1;
}

/*
 * The fork path
 *
 * A request posix_spawn() cannot carry is carried by a child of this
 * process's own; spawn_is_expressible() below is which requests those are.
 * Where the host has no posix_spawn() at all, this is the only path there is.
 */

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
 * It has to be close-on-exec, and it has to become that in the call that
 * creates it.  Between pipe() and the fcntl() that would set the flag,
 * another thread of the embedding process can fork and exec, and the copy of
 * the write end that child keeps is one this parent cannot close: the read
 * below then reaches EOF when that unrelated child exits rather than when
 * this one execs.  pipe2() sets the flag as it creates the descriptors and
 * leaves no such window, and HAVE_PIPE2 is whether this host has it, asked of
 * the compiler by mrbgem.rake since a `#if` here cannot read a header.
 *
 * The two calls remain for the hosts that have no atomic form to offer.
 * macOS is one of them, so this is not only a path for old systems.  It
 * closes the window no further than fcntl() can, which is to say that a fork
 * and exec landing in it still carries the write end away.
 */
static int
errpipe(int fds[2])
{
#ifdef HAVE_PIPE2
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

/* Everything the child does between fork() and exec().  It reports a failure
   by writing errno down `errfd`, which the parent is reading; the write end
   is close-on-exec, so a successful exec closes it and the parent's read
   ends at EOF with nothing to report.

   Nothing here allocates: the arrays it needs were allocated before the
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
         either runs it or it does not.  A plan that is the shell already has
         nothing to fall back to and reports what it was told. */
      if (plan->sh_argv == NULL) goto fail;
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
    /* The parent reads exactly this many bytes or nothing at all, so a write
       cut short by a signal is finished rather than left half said.  Nobody
       else writes to this pipe, so what is left to write is what has not
       been written yet. */
    const char *p = (const char*)&err;
    size_t left = sizeof(err);

    while (left > 0) {
      ssize_t n = write(errfd, p, left);
      if (n == -1) {
        if (errno == EINTR) continue;
        break;
      }
      p += n;
      left -= (size_t)n;
    }
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

/* Spawn the plan with fork() and exec(), a failed exec reported down a pipe.
   Returns 0 with *out_pid set, or -1 with errno set. */
static int
spawn_fork(mrb_state *mrb, const mrb_process_spawn_params *params,
           const struct exec_plan *plan, pid_t *out_pid)
{
  int errfds[2] = { -1, -1 };
  int *sources = NULL;
  int *keep = NULL;
  long maxfd;
  int child_errno, saved_errno;
  pid_t pid;
  size_t i;

  /* Both arrays belong to the child as much as to this call: it may not
     allocate between fork() and exec(), so what it will need is allocated
     here.  `sources` is one per redirection, `keep` one per redirection plus
     the error pipe.  How high descriptors go is read here for the same
     reason, since sysconf() is not a call the child may make. */
  sources = (int*)mrb_malloc_simple(mrb, sizeof(int) * (params->nredirects + 1));
  keep = (int*)mrb_malloc_simple(mrb, sizeof(int) * (params->nredirects + 1));
  if (sources == NULL || keep == NULL) {
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

  if (errpipe(errfds) == -1) goto error;

  pid = fork();
  if (pid == -1) goto error;
  if (pid == 0) {
    close(errfds[0]);
    child_exec(params, plan, sources, errfds[1], keep, maxfd);
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
  mrb_free(mrb, keep);
  *out_pid = pid;
  return 0;

error:
  saved_errno = errno;
  if (errfds[0] != -1) close(errfds[0]);
  if (errfds[1] != -1) close(errfds[1]);
  mrb_free(mrb, sources);
  mrb_free(mrb, keep);
  errno = saved_errno;
  return -1;
}

#if MRB_PROCESS_HAVE_POSIX_SPAWN

/* Whether posix_spawn() can carry this request.
 *
 * What it carries, it carries as a list of file actions written before there
 * is a child to apply them to.  Two of the things a redirection table can ask
 * for are not that shape and go to the fork path, which decides from inside
 * the child.
 */
static int
spawn_is_expressible(const mrb_process_spawn_params *params)
{
  size_t i;

#ifndef HAVE_POSIX_SPAWN_ADDCHDIR
  /* Where the child starts is a file action this host has not got. */
  if (params->chdir != NULL) return 0;
#endif

  /* :close_others is a sweep over the descriptors the caller did not name,
     and what is open is what only the child can be asked. */
  if (params->flags & MRB_PROCESS_SPAWN_CLOSE_OTHERS) return 0;

  for (i = 0; i < params->nredirects; i++) {
    const mrb_process_redirect *r = &params->redirects[i];

    /* `1 => 1` leaves a descriptor open rather than moving it, which as a
       file action is a dup2() of a descriptor onto itself.  That is defined
       to clear FD_CLOEXEC, but only since POSIX.1-2008 TC2, and a host is
       free to have posix_spawn() and not that.  The fork path clears the
       flag with fcntl() and asks nothing of the host. */
    if (r->kind == MRB_PROCESS_REDIR_CHILD && r->source_fd == r->child_fd) return 0;
  }
  return 1;
}

/* Duplicate `fd` above `least` and keep it out of the exec'd image, as
   fd_move_above() does for the fork path, in the one call where the host has
   it: there is no child on this side, but the descriptor is still one
   another thread could carry away between the two calls. */
static int
spawn_dup_high(int fd, int least)
{
#ifdef F_DUPFD_CLOEXEC
  return fcntl(fd, F_DUPFD_CLOEXEC, least);
#else
  return fd_move_above(fd, least);
#endif
}

/* Spawn the plan with posix_spawn(), which creates the child and executes
   the image in the one call and answers with the errno an exec failed with.
   So there is no fork() here for another thread's lock to be caught in, and
   no pipe for a failure to be carried back down.
   Returns 0 with *out_pid set, or -1 with errno set. */
static int
spawn_posix(mrb_state *mrb, const mrb_process_spawn_params *params,
            const struct exec_plan *plan, pid_t *out_pid)
{
  posix_spawn_file_actions_t actions;
  int actions_ready = 0;
  int *dups = NULL;
  mrb_int max_target = -1;
  pid_t pid = -1;
  size_t i;
  int denied = 0;
  int err = 0;

  for (i = 0; i < params->nredirects; i++) {
    if (params->redirects[i].child_fd > max_target) {
      max_target = params->redirects[i].child_fd;
    }
  }

  if (params->nredirects > 0) {
    dups = (int*)mrb_malloc_simple(mrb, sizeof(int) * params->nredirects);
    if (dups == NULL) {
      err = ENOMEM;
      goto done;
    }
    for (i = 0; i < params->nredirects; i++) dups[i] = -1;
  }

  err = posix_spawn_file_actions_init(&actions);
  if (err != 0) goto done;
  actions_ready = 1;

  /* A source that is also one of the targets is duplicated out of the way
     first: the actions are applied in order, and an earlier one would write
     over a descriptor a later one still has to read. */
  for (i = 0; i < params->nredirects; i++) {
    const mrb_process_redirect *r = &params->redirects[i];

    if (r->kind != MRB_PROCESS_REDIR_PARENT) continue;
    if (r->source_fd > max_target) continue;
    dups[i] = spawn_dup_high((int)r->source_fd, (int)max_target + 1);
    if (dups[i] == -1) {
      err = errno;
      goto done;
    }
  }

  /* The table, in order, as the child would have applied it.  What a dup2()
     answers with is a descriptor that is not close-on-exec, so nothing here
     has to clear the flag the way the fork path does. */
  for (i = 0; i < params->nredirects; i++) {
    const mrb_process_redirect *r = &params->redirects[i];
    int target = (int)r->child_fd;

    switch (r->kind) {
    case MRB_PROCESS_REDIR_CLOSE:
      /* Closing what is not open is what the caller asked for and is already
         so, but as a file action it is a close() that fails, and one failed
         action takes the whole spawn with it.  The child's table is this
         process's until the exec, so the question is asked here. */
      if (fcntl(target, F_GETFD) == -1 && errno == EBADF) continue;
      err = posix_spawn_file_actions_addclose(&actions, target);
      break;
    case MRB_PROCESS_REDIR_PARENT:
      err = posix_spawn_file_actions_adddup2(&actions,
                                             dups[i] != -1 ? dups[i] : (int)r->source_fd,
                                             target);
      break;
    case MRB_PROCESS_REDIR_CHILD:
      err = posix_spawn_file_actions_adddup2(&actions, (int)r->source_fd, target);
      break;
    }
    if (err != 0) goto done;
  }

#ifdef HAVE_POSIX_SPAWN_ADDCHDIR
  /* Last, as it is last in the child: what a redirection names is a
     descriptor rather than a path, so none of the above is read against the
     directory the child moves to. */
  if (params->chdir != NULL) {
    err = posix_spawn_file_actions_addchdir_np(&actions, params->chdir);
    if (err != 0) goto done;
  }
#endif

  /* The command, tried at each name the plan worked out for it.  What the
     child made of execve()'s errno is made here of posix_spawn()'s answer,
     which is that same number arrived at the same way.  A plan always names
     at least one image, so the walk always reaches an answer; the value set
     here is what a host that handed one over empty would report. */
  err = ENOENT;
  for (i = 0; plan->path[i] != NULL; i++) {
    err = posix_spawn(&pid, plan->path[i], &actions, NULL,
                      (char*const*)plan->argv, plan->envp);
    if (err == 0) goto done;
    switch (err) {
    case ENOEXEC:
      /* A file that is not an executable image is handed to the shell, as
         execvp() hands it to one.  Nothing is tried after that: the shell
         either runs it or it does not.  A plan that is the shell already has
         nothing to fall back to and reports what it was told. */
      if (plan->sh_argv == NULL) goto done;
      plan->sh_argv[1] = plan->path[i];
      err = posix_spawn(&pid, "/bin/sh", &actions, NULL,
                        (char*const*)plan->sh_argv, plan->envp);
      goto done;
    case EACCES:
      /* Something of that name is there and cannot be run.  Later names may
         still work, and this is what is reported if none of them does. */
      denied = 1;
      break;
    case ENOENT:
    case ENOTDIR:
      break;
    default:
      goto done;
    }
  }
  if (denied) err = EACCES;

done:
  if (dups != NULL) {
    for (i = 0; i < params->nredirects; i++) {
      if (dups[i] != -1) close(dups[i]);
    }
    mrb_free(mrb, dups);
  }
  if (actions_ready) posix_spawn_file_actions_destroy(&actions);
  if (err != 0) {
    errno = err;
    return -1;
  }
  *out_pid = pid;
  return 0;
}

#endif /* MRB_PROCESS_HAVE_POSIX_SPAWN */

int
mrb_hal_process_spawn(mrb_state *mrb, mrb_hal_process_context *ctx,
                      const mrb_process_spawn_params *params,
                      mrb_hal_process_child **out)
{
  struct mrb_hal_process_child *child;
  struct exec_plan plan;
  int saved_errno;
  pid_t pid;

  /* The empty name names no file, and looking for it on the PATH would find
     the directories themselves.  execve("") answers ENOENT and so does this,
     which is also what a command line of nothing but blanks arrives as. */
  memset(&plan, 0, sizeof(plan));
  if (params->argv == NULL || params->argv[0] == NULL ||
      params->argv[0][0] == '\0') {
    errno = ENOENT;
    return -1;
  }

  child = (struct mrb_hal_process_child*)mrb_malloc_simple(mrb, sizeof(*child));
  if (child == NULL) {
    errno = ENOMEM;
    return -1;
  }

  if (plan_build(mrb, params, &plan) != 0) goto error;

#if MRB_PROCESS_HAVE_POSIX_SPAWN
  if (spawn_is_expressible(params)) {
    if (spawn_posix(mrb, params, &plan, &pid) != 0) goto error;
  }
  else if (spawn_fork(mrb, params, &plan, &pid) != 0) {
    goto error;
  }
#else
  if (spawn_fork(mrb, params, &plan, &pid) != 0) goto error;
#endif

  plan_free(mrb, &plan);
  child->pid = pid;
  child->udata = NULL;
  child->next = ctx->children;
  ctx->children = child;
  *out = child;
  return 0;

error:
  saved_errno = errno;
  plan_free(mrb, &plan);
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
     gettimeofday(2) answers it in microsecond units. */
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

/*
 * CPU Time Totals
 */

#if MRB_PROCESS_HAVE_GETRUSAGE

/* Convert a struct timeval, as getrusage(2) reports CPU time in, into an
   mrb_process_clock_time.  tv_usec is always in [0, 1000000), so unlike the
   FILETIME split on Windows there is nothing here to carry downwards. */
static void
timeval_to_clock_time(mrb_process_clock_time *t, const struct timeval *tv)
{
  t->sec = (int64_t)tv->tv_sec;
  t->nsec = (int64_t)tv->tv_usec * 1000;
}

int
mrb_hal_process_times(mrb_state *mrb, mrb_process_times *t)
{
  struct rusage self, children;
  (void)mrb;

  /* RUSAGE_SELF is this process, summed across every thread that has ever
     run in it; RUSAGE_CHILDREN is every child this process has reaped via
     wait(2)/waitpid(2), summed the same way, which is exactly the
     utime/stime and cutime/cstime split Process.times reports. Both report
     CPU time as a struct timeval with microsecond units, with no
     sysconf(_SC_CLK_TCK) scale factor to look up. */
  if (getrusage(RUSAGE_SELF, &self) != 0) return -1;
  if (getrusage(RUSAGE_CHILDREN, &children) != 0) return -1;

  timeval_to_clock_time(&t->utime,  &self.ru_utime);
  timeval_to_clock_time(&t->stime,  &self.ru_stime);
  timeval_to_clock_time(&t->cutime, &children.ru_utime);
  timeval_to_clock_time(&t->cstime, &children.ru_stime);
  return 0;
}

#else /* !MRB_PROCESS_HAVE_GETRUSAGE */

/* Split a count of times(2) ticks into seconds and nanoseconds. */
static void
ticks_to_clock_time(mrb_process_clock_time *t, clock_t ticks, long clk_tck)
{
  int64_t v = (int64_t)ticks;

  t->sec = v / (int64_t)clk_tck;
  t->nsec = (v % (int64_t)clk_tck) * NSEC_PER_SEC / (int64_t)clk_tck;
}

int
mrb_hal_process_times(mrb_state *mrb, mrb_process_times *t)
{
  struct tms tm;
  long clk_tck;
  (void)mrb;

  /* Where getrusage(2) is unavailable, times(2) is the POSIX.1 baseline:
     the same four totals, as clock_t ticks to be scaled by _SC_CLK_TCK.

     Its return value is elapsed real time, not an error indicator: it
     passes through (clock_t)-1 legitimately as that counter wraps, and
     POSIX leaves errno after a successful call unspecified, so a failure
     cannot be told from one portably. The tms fields are all this reading
     needs, so the return value is ignored, as CRuby's fallback ignores it. */
  (void)times(&tm);

  /* Ticks per second. POSIX guarantees _SC_CLK_TCK an answer; a
     non-positive one is a host declining to say. CLOCKS_PER_SEC is
     clock(3)'s unit, not always times(2)'s, so declining is reported
     rather than guessed past. */
  clk_tck = sysconf(_SC_CLK_TCK);
  if (clk_tck <= 0) {
    errno = EINVAL;
    return -1;
  }

  ticks_to_clock_time(&t->utime,  tm.tms_utime,  clk_tck);
  ticks_to_clock_time(&t->stime,  tm.tms_stime,  clk_tck);
  ticks_to_clock_time(&t->cutime, tm.tms_cutime, clk_tck);
  ticks_to_clock_time(&t->cstime, tm.tms_cstime, clk_tck);
  return 0;
}

#endif /* MRB_PROCESS_HAVE_GETRUSAGE */
