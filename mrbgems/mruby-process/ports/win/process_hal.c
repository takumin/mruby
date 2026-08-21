/*
** process_hal.c - Windows HAL implementation for mruby-process
**
** See Copyright Notice in mruby.h
**
** Windows implementation of the process HAL.  Windows has no signals between
** processes and no wait status beyond an exit code, so this port covers the
** part of the interface Win32 can answer honestly and reports ENOSYS for the
** rest:
**
**   - a child is a HANDLE, and the context holds the live ones, because
**     nothing else on this platform knows what this interpreter's children
**     are;
**   - no process is ever reported as stopped, so MRB_PROCESS_WAIT_UNTRACED
**     is accepted and changes nothing;
**   - a raw status is the child's exit code, so a decoded status always
**     reads as exited, even for a process this gem terminated;
**   - only `KILL` and `TERM` can be delivered, both as TerminateProcess(),
**     and signal 0 asks whether the process can be opened at all;
**   - only descriptors 0, 1 and 2 can be redirected, because STARTUPINFO has
**     three slots and no more; anything else fails with ENOTSUP.
*/

#include <mruby.h>
#include "process_hal.h"

#include <windows.h>
#include <tlhelp32.h>

#include <errno.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Vista and later; fall back to the access right every Windows has. */
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION PROCESS_QUERY_INFORMATION
#endif

/*
 * Signal table
 *
 * The numbers Windows' <signal.h> uses, plus KILL at its conventional POSIX
 * value so that `Process.kill(:KILL, pid)` names something here.  Being in
 * the table only makes a name resolvable; mrb_hal_process_kill() decides
 * which of them can actually be delivered.
 */

struct signal_entry {
  const char *name;
  int signo;
};

static const struct signal_entry signal_table[] = {
  { "INT",   2 },
  { "ILL",   4 },
  { "FPE",   8 },
  { "KILL",  9 },
  { "SEGV",  11 },
  { "TERM",  15 },
  { "BREAK", 21 },
  { "ABRT",  22 },
};

#define SIGNAL_TABLE_LEN (sizeof(signal_table) / sizeof(signal_table[0]))

#define SIGNAL_KILL 9
#define SIGNAL_TERM 15

/* Exit code TerminateProcess() stamps on the victim.  128+SIGTERM is the
   shell's convention for "died on a signal", which is the closest a Windows
   exit code gets to saying so. */
#define TERMINATED_EXIT_CODE (128 + SIGNAL_TERM)

/*
 * Helper Functions
 */

static void
set_errno_from_win32(DWORD err)
{
  switch (err) {
  case ERROR_INVALID_PARAMETER:
    errno = EINVAL;
    break;
  case ERROR_ACCESS_DENIED:
    errno = EPERM;
    break;
  case ERROR_INVALID_HANDLE:
  case ERROR_NOT_FOUND:
    errno = ESRCH;
    break;
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    errno = ENOENT;
    break;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    errno = ENOMEM;
    break;
  default:
    errno = EINVAL;
    break;
  }
}

/* Open a process by pid.  A pid Windows will not open is "no such process"
   here, whatever it says: OpenProcess reports a pid that names nothing as
   ERROR_INVALID_PARAMETER rather than as a missing process. */
static HANDLE
open_process(mrb_int pid, DWORD access)
{
  HANDLE h;
  DWORD err;

  if (pid <= 0 || (uint64_t)pid > 0xFFFFFFFFu) {
    errno = ESRCH;
    return NULL;
  }
  h = OpenProcess(access, FALSE, (DWORD)pid);
  if (h == NULL) {
    err = GetLastError();
    switch (err) {
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
    case ERROR_NOT_FOUND:
      errno = ESRCH;
      break;
    default:
      set_errno_from_win32(err);
      break;
    }
  }
  return h;
}

/* Narrow a Win32 exit code to mrb_int.  Codes such as 0xC0000005 do not fit a
   32-bit mrb_int unsigned, so they are read as the signed value of the same
   bits and come out negative. */
static mrb_int
exit_code_to_status(DWORD code)
{
  return (mrb_int)(int32_t)code;
}

/*
 * Context and children
 *
 * A Windows child is a HANDLE: it is what keeps the exit status readable
 * after the process is gone, and it is the only thing a wait can wait on.
 * The pid is a label kept beside it, for Process.kill and Process::Status.
 */

struct mrb_hal_process_child {
  HANDLE handle;
  DWORD pid;
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
    if (child->handle != NULL) CloseHandle(child->handle);
    mrb_free(mrb, child);
  }
  mrb_free(mrb, ctx);
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
  if (child->handle != NULL) CloseHandle(child->handle);
  mrb_free(mrb, child);
}

/*
 * Process Identity
 */

mrb_int
mrb_hal_process_pid(mrb_state *mrb)
{
  (void)mrb;
  return (mrb_int)GetCurrentProcessId();
}

mrb_int
mrb_hal_process_ppid(mrb_state *mrb)
{
  HANDLE snapshot;
  PROCESSENTRY32W entry;
  DWORD self = GetCurrentProcessId();
  mrb_int ppid = -1;
  (void)mrb;

  snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    set_errno_from_win32(GetLastError());
    return -1;
  }

  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == self) {
        ppid = (mrb_int)entry.th32ParentProcessID;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  if (ppid < 0) errno = ESRCH;
  return ppid;
}

/*
 * Spawning
 */

#ifndef MRB_NO_PROCESS_SPAWN

/* A growable byte buffer, used for the command line and the environment
   block.  Both are built by appending, and both end up as one allocation
   CreateProcess reads. */
struct buf {
  mrb_state *mrb;
  char *ptr;
  size_t len;
  size_t cap;
  int failed;
};

static void
buf_init(struct buf *b, mrb_state *mrb)
{
  b->mrb = mrb;
  b->ptr = NULL;
  b->len = b->cap = 0;
  b->failed = 0;
}

static void
buf_free(struct buf *b)
{
  mrb_free(b->mrb, b->ptr);
  b->ptr = NULL;
}

static void
buf_add(struct buf *b, const char *data, size_t len)
{
  if (b->failed) return;
  if (b->len + len > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 256;
    char *ptr;
    while (cap < b->len + len) cap *= 2;
    ptr = (char*)mrb_realloc_simple(b->mrb, b->ptr, cap);
    if (ptr == NULL) {
      b->failed = 1;
      return;
    }
    b->ptr = ptr;
    b->cap = cap;
  }
  memcpy(b->ptr + b->len, data, len);
  b->len += len;
}

static void
buf_add_cstr(struct buf *b, const char *s)
{
  buf_add(b, s, strlen(s) + 1);  /* including the NUL */
}

static void
buf_add_char(struct buf *b, char c)
{
  buf_add(b, &c, 1);
}

/* Quote one argument the way the C runtime's command-line parser reads it
   back, so that argv[] in the child is the argv[] that was asked for. */
static void
buf_add_argument(struct buf *b, const char *arg)
{
  const char *p;
  int needs_quotes = (*arg == '\0');

  for (p = arg; *p != '\0'; p++) {
    if (*p == ' ' || *p == '\t' || *p == '"') {
      needs_quotes = 1;
      break;
    }
  }
  if (!needs_quotes) {
    buf_add(b, arg, strlen(arg));
    return;
  }

  buf_add_char(b, '"');
  for (p = arg; *p != '\0'; p++) {
    size_t backslashes = 0;
    while (*p == '\\') {
      backslashes++;
      p++;
    }
    if (*p == '\0') {
      /* Backslashes before the closing quote are doubled. */
      for (; backslashes > 0; backslashes--) buf_add(b, "\\\\", 2);
      break;
    }
    if (*p == '"') {
      for (; backslashes > 0; backslashes--) buf_add(b, "\\\\", 2);
      buf_add(b, "\\\"", 2);
    }
    else {
      for (; backslashes > 0; backslashes--) buf_add_char(b, '\\');
      buf_add_char(b, *p);
    }
  }
  buf_add_char(b, '"');
}

/* The command line CreateProcess is given.  A shell command goes to cmd.exe
   verbatim; an argv is quoted back into one string, because a command line
   is all Windows has. */
static char*
build_command_line(mrb_state *mrb, const mrb_process_spawn_params *params)
{
  struct buf b;
  size_t i;

  buf_init(&b, mrb);
  if (params->kind == MRB_PROCESS_SPAWN_SHELL) {
    const char *shell = getenv("COMSPEC");
    if (shell == NULL) shell = "cmd.exe";
    buf_add(&b, shell, strlen(shell));
    buf_add(&b, " /c ", 4);
    buf_add(&b, params->argv[0], strlen(params->argv[0]));
  }
  else {
    for (i = 0; params->argv[i] != NULL; i++) {
      if (i > 0) buf_add_char(&b, ' ');
      buf_add_argument(&b, params->argv[i]);
    }
  }
  buf_add_char(&b, '\0');

  if (b.failed) {
    buf_free(&b);
    errno = ENOMEM;
    return NULL;
  }
  return b.ptr;
}

/* Compare an environment entry's name with `name`, case-insensitively as
   Windows does. */
static int
env_name_is(const char *entry, const char *name, size_t namelen)
{
  size_t i;

  for (i = 0; i < namelen; i++) {
    char a = entry[i], c = name[i];
    if (a == '\0' || a == '=') return 0;
    if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (a != c) return 0;
  }
  return entry[namelen] == '=';
}

/* The environment block for the child: the parent's, with the deltas applied.
   NULL means "inherit unchanged", which is what CreateProcess reads a NULL
   environment as. */
static int
build_environment(mrb_state *mrb, const mrb_process_spawn_params *params, char **out)
{
  struct buf b;
  char *parent = NULL;
  size_t i;

  *out = NULL;
  if (params->nenv == 0 && !(params->flags & MRB_PROCESS_SPAWN_UNSETENV_OTHERS)) {
    return 0;
  }

  buf_init(&b, mrb);
  if (!(params->flags & MRB_PROCESS_SPAWN_UNSETENV_OTHERS)) {
    const char *entry;
    parent = GetEnvironmentStringsA();
    if (parent == NULL) {
      errno = ENOMEM;
      return -1;
    }
    for (entry = parent; *entry != '\0'; entry += strlen(entry) + 1) {
      int overridden = 0;
      for (i = 0; i < params->nenv; i++) {
        if (env_name_is(entry, params->env[i].name, strlen(params->env[i].name))) {
          overridden = 1;
          break;
        }
      }
      /* An entry the deltas name is written below -- or, when the delta
         unsets it, not written at all. */
      if (!overridden) buf_add_cstr(&b, entry);
    }
    FreeEnvironmentStringsA(parent);
  }

  for (i = 0; i < params->nenv; i++) {
    if (params->env[i].value == NULL) continue;
    buf_add(&b, params->env[i].name, strlen(params->env[i].name));
    buf_add_char(&b, '=');
    buf_add_cstr(&b, params->env[i].value);
  }
  /* The block is a run of NUL-terminated strings followed by one more NUL.
     An empty environment is the two NULs alone, not one. */
  if (b.len == 0) buf_add_char(&b, '\0');
  buf_add_char(&b, '\0');

  if (b.failed) {
    buf_free(&b);
    errno = ENOMEM;
    return -1;
  }
  *out = b.ptr;
  return 0;
}

/* Resolve the ordered redirection table into the three handles STARTUPINFO
   has room for.  Windows cannot apply a table one entry at a time the way a
   forked child can, so the table is played out here instead, over the three
   slots, and anything that does not fit in them is refused rather than
   half-applied. */
static int
resolve_redirects(const mrb_process_spawn_params *params, HANDLE slots[3])
{
  static const DWORD std_ids[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
  size_t i;
  int fd;

  for (fd = 0; fd < 3; fd++) {
    slots[fd] = GetStdHandle(std_ids[fd]);
  }

  for (i = 0; i < params->nredirects; i++) {
    const mrb_process_redirect *r = &params->redirects[i];
    if (r->child_fd < 0 || r->child_fd > 2) {
      errno = ENOTSUP;
      return -1;
    }
    switch (r->kind) {
    case MRB_PROCESS_REDIR_CLOSE:
      /* No handle at all is as close as CreateProcess comes to a closed
         descriptor. */
      slots[r->child_fd] = NULL;
      break;
    case MRB_PROCESS_REDIR_PARENT:
      {
        intptr_t h;
        if (r->source_fd < 0) {
          errno = EBADF;
          return -1;
        }
        h = _get_osfhandle((int)r->source_fd);
        if (h == -1 || (HANDLE)h == INVALID_HANDLE_VALUE) {
          errno = EBADF;
          return -1;
        }
        slots[r->child_fd] = (HANDLE)h;
      }
      break;
    case MRB_PROCESS_REDIR_CHILD:
      if (r->source_fd < 0 || r->source_fd > 2) {
        errno = ENOTSUP;
        return -1;
      }
      slots[r->child_fd] = slots[r->source_fd];
      break;
    }
  }
  return 0;
}

int
mrb_hal_process_spawn(mrb_state *mrb, mrb_hal_process_context *ctx,
                      const mrb_process_spawn_params *params,
                      mrb_hal_process_child **out)
{
  struct mrb_hal_process_child *child = NULL;
  STARTUPINFOEXA si;
  PROCESS_INFORMATION pi;
  LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
  SIZE_T attrs_size = 0;
  HANDLE slots[3], wanted[3];
  HANDLE inherit[3];
  HANDLE dups[3] = { NULL, NULL, NULL };
  DWORD ninherit = 0, i, j;
  char *cmdline = NULL;
  char *envblock = NULL;
  int saved_errno;

  if (params->argv == NULL || params->argv[0] == NULL || params->argv[0][0] == '\0') {
    errno = ENOENT;
    return -1;
  }

  child = (struct mrb_hal_process_child*)mrb_malloc_simple(mrb, sizeof(*child));
  if (child == NULL) {
    errno = ENOMEM;
    return -1;
  }

  if (resolve_redirects(params, slots) != 0) goto error;

  cmdline = build_command_line(mrb, params);
  if (cmdline == NULL) goto error;
  if (build_environment(mrb, params, &envblock) != 0) goto error;

  /* Hand the child duplicates of exactly the handles it is meant to have.
     Marking the originals inheritable and letting CreateProcess take every
     inheritable handle would leak this spawn's pipes into the next one, and
     a pipe whose write end an unrelated child still holds never reaches
     EOF. */
  memcpy(wanted, slots, sizeof(wanted));
  for (i = 0; i < 3; i++) {
    HANDLE dup = NULL;
    if (wanted[i] == NULL || wanted[i] == INVALID_HANDLE_VALUE) {
      slots[i] = NULL;
      continue;
    }
    /* The same handle in two slots -- `2>&1` and its like -- is duplicated
       once, so that the inherit list stays a set. */
    for (j = 0; j < i; j++) {
      if (wanted[j] == wanted[i] && dups[j] != NULL) break;
    }
    if (j < i) {
      slots[i] = dups[j];
      continue;
    }
    if (!DuplicateHandle(GetCurrentProcess(), wanted[i], GetCurrentProcess(),
                         &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
      set_errno_from_win32(GetLastError());
      goto error;
    }
    dups[i] = dup;
    slots[i] = dup;
    inherit[ninherit++] = dup;
  }

  memset(&si, 0, sizeof(si));
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = slots[0];
  si.StartupInfo.hStdOutput = slots[1];
  si.StartupInfo.hStdError = slots[2];

  if (ninherit > 0) {
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrs_size);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)mrb_malloc_simple(mrb, (size_t)attrs_size);
    if (attrs == NULL) {
      errno = ENOMEM;
      goto error;
    }
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrs_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit, ninherit * sizeof(HANDLE), NULL, NULL)) {
      set_errno_from_win32(GetLastError());
      goto error;
    }
    si.lpAttributeList = attrs;
  }

  memset(&pi, 0, sizeof(pi));
  if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT, envblock,
                      params->chdir, &si.StartupInfo, &pi)) {
    set_errno_from_win32(GetLastError());
    goto error;
  }

  CloseHandle(pi.hThread);
  for (i = 0; i < 3; i++) {
    if (dups[i] != NULL) CloseHandle(dups[i]);
  }
  if (attrs != NULL) {
    DeleteProcThreadAttributeList(attrs);
    mrb_free(mrb, attrs);
  }
  mrb_free(mrb, cmdline);
  mrb_free(mrb, envblock);

  /* The pid is what the caller gets and what Process.kill will use; the
     handle stays here, where nothing can mistake it for a number. */
  child->handle = pi.hProcess;
  child->pid = pi.dwProcessId;
  child->udata = NULL;
  child->next = ctx->children;
  ctx->children = child;
  *out = child;
  return 0;

error:
  saved_errno = errno;
  for (i = 0; i < 3; i++) {
    if (dups[i] != NULL) CloseHandle(dups[i]);
  }
  if (attrs != NULL) {
    DeleteProcThreadAttributeList(attrs);
    mrb_free(mrb, attrs);
  }
  mrb_free(mrb, cmdline);
  mrb_free(mrb, envblock);
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

/* A Windows raw status is an exit code and nothing else: a process killed
   with TerminateProcess() is indistinguishable from one that exited with the
   same code, so every status reads as exited. */
static void
status_from_exit_code(mrb_int pid, DWORD code, mrb_process_status *status)
{
  status->pid = pid;
  status->raw_status = exit_code_to_status(code);
  status->exitstatus = status->raw_status;
  status->termsig = 0;
  status->stopsig = 0;
  status->flags = MRB_PROCESS_STATUS_EXITED;
}

static int
finish_wait(mrb_hal_process_child *child, mrb_process_event *event)
{
  DWORD code = 0;

  if (!GetExitCodeProcess(child->handle, &code)) {
    set_errno_from_win32(GetLastError());
    return -1;
  }
  status_from_exit_code((mrb_int)child->pid, code, &event->status);
  event->kind = MRB_PROCESS_EVENT_EXITED;
  event->child = child;
  return 0;
}

/* Wait over every live child.  WaitForMultipleObjects takes at most
   MAXIMUM_WAIT_OBJECTS handles at a time; a poll can be run in chunks, but a
   blocking wait over more than one chunk cannot be expressed, so it is
   refused rather than silently watching part of the set. */
static int
wait_any(mrb_state *mrb, mrb_hal_process_context *ctx, unsigned int flags,
         mrb_process_event *event)
{
  struct mrb_hal_process_child *child, *found = NULL;
  HANDLE handles[MAXIMUM_WAIT_OBJECTS];
  DWORD count = 0, result;
  (void)mrb;

  if (ctx->children == NULL) {
    errno = ECHILD;
    return -1;
  }

  if (flags & MRB_PROCESS_WAIT_NOHANG) {
    for (child = ctx->children; child != NULL; child = child->next) {
      if (WaitForSingleObject(child->handle, 0) == WAIT_OBJECT_0) {
        found = child;
        break;
      }
    }
    if (found == NULL) {
      memset(event, 0, sizeof(*event));
      event->kind = MRB_PROCESS_EVENT_NONE;
      return 0;
    }
    memset(event, 0, sizeof(*event));
    return finish_wait(found, event);
  }

  for (child = ctx->children; child != NULL; child = child->next) {
    if (count == MAXIMUM_WAIT_OBJECTS) {
      errno = EINVAL;
      return -1;
    }
    handles[count++] = child->handle;
  }

  result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
  if (result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + count) {
    set_errno_from_win32(GetLastError());
    return -1;
  }
  result -= WAIT_OBJECT_0;
  for (child = ctx->children; child != NULL; child = child->next) {
    if (child->handle == handles[result]) {
      found = child;
      break;
    }
  }
  if (found == NULL) {
    errno = ECHILD;
    return -1;
  }
  memset(event, 0, sizeof(*event));
  return finish_wait(found, event);
}

int
mrb_hal_process_wait(mrb_state *mrb, mrb_hal_process_context *ctx,
                     mrb_hal_process_child *child, unsigned int flags,
                     mrb_process_event *event)
{
  DWORD result;

  if (child == NULL) return wait_any(mrb, ctx, flags, event);

  /* MRB_PROCESS_WAIT_UNTRACED asks for stopped children, of which Windows
     has none, so it changes nothing here. */
  result = WaitForSingleObject(child->handle,
                               (flags & MRB_PROCESS_WAIT_NOHANG) ? 0 : INFINITE);
  memset(event, 0, sizeof(*event));
  if (result == WAIT_TIMEOUT) {
    event->kind = MRB_PROCESS_EVENT_NONE;
    return 0;
  }
  if (result != WAIT_OBJECT_0) {
    set_errno_from_win32(GetLastError());
    return -1;
  }
  return finish_wait(child, event);
}

/*
 * Signalling
 */

int
mrb_hal_process_kill(mrb_state *mrb, mrb_int pid, mrb_int signo)
{
  HANDLE h;
  (void)mrb;

  if (signo == 0) {
    h = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (h == NULL) return -1;
    CloseHandle(h);
    return 0;
  }

  if (signo != SIGNAL_KILL && signo != SIGNAL_TERM) {
    errno = ENOSYS;
    return -1;
  }

  h = open_process(pid, PROCESS_TERMINATE);
  if (h == NULL) return -1;
  if (!TerminateProcess(h, TERMINATED_EXIT_CODE)) {
    set_errno_from_win32(GetLastError());
    CloseHandle(h);
    return -1;
  }
  CloseHandle(h);
  return 0;
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
