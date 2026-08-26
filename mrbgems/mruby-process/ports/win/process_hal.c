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
**
** Everything that reaches Win32 goes through the wide entry points.  A mruby
** String is bytes holding UTF-8, and the ANSI ones would read it in whatever
** code page the machine is set to, so a command, a path or an environment
** value spelled outside that code page would reach the child as something
** else.  The conversion happens at this boundary and nowhere above it.
**
** The clocks are the one part Win32 answers in full: the wall clock as a
** FILETIME, the monotonic one from the performance counter, and the two CPU
** times from GetProcessTimes() and GetThreadTimes().
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
#include <wchar.h>

/* Vista and later; fall back to the access right every Windows has. */
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION PROCESS_QUERY_INFORMATION
#endif

/* The two signals this port can deliver, by the numbers mruby-signal's
   Windows table gives them.  Naming them here rather than asking the signal
   HAL keeps this port answering about delivery alone: which names resolve to
   which numbers is that gem's question, and a port that could not reach it
   would still have to know these two. */
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

/* A growable UTF-16 buffer, used for the command line and the environment
   block.  Both are built by appending, and both end up as one allocation
   CreateProcessW reads. */
struct wbuf {
  mrb_state *mrb;
  wchar_t *ptr;
  size_t len;
  size_t cap;
  int failed;
};

static void
wbuf_init(struct wbuf *b, mrb_state *mrb)
{
  b->mrb = mrb;
  b->ptr = NULL;
  b->len = b->cap = 0;
  b->failed = 0;
}

static void
wbuf_free(struct wbuf *b)
{
  mrb_free(b->mrb, b->ptr);
  b->ptr = NULL;
}

static void
wbuf_add(struct wbuf *b, const wchar_t *data, size_t len)
{
  if (b->failed) return;
  if (b->len + len > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 256;
    wchar_t *ptr;

    while (cap < b->len + len) cap *= 2;
    ptr = (wchar_t*)mrb_realloc_simple(b->mrb, b->ptr, cap * sizeof(wchar_t));
    if (ptr == NULL) {
      b->failed = 1;
      return;
    }
    b->ptr = ptr;
    b->cap = cap;
  }
  memcpy(b->ptr + b->len, data, len * sizeof(wchar_t));
  b->len += len;
}

static void
wbuf_add_wstr(struct wbuf *b, const wchar_t *s)
{
  wbuf_add(b, s, wcslen(s) + 1);  /* including the NUL */
}

static void
wbuf_add_wchar(struct wbuf *b, wchar_t c)
{
  wbuf_add(b, &c, 1);
}

/*
 * UTF-8 to UTF-16
 *
 * What this gem is handed are mruby Strings, which are bytes, and which hold
 * UTF-8 wherever they came from a Ruby literal.  The ANSI entry points would
 * read them in whatever code page the machine is set to, so a path, an
 * argument or an environment value that is not spelled in that code page
 * would reach the child as something else, or not at all.  The wide entry
 * points take what was written.
 */
static wchar_t*
utf8_to_wide(mrb_state *mrb, const char *s)
{
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w;

  if (n <= 0) {
    errno = EINVAL;
    return NULL;
  }
  w = (wchar_t*)mrb_malloc_simple(mrb, (size_t)n * sizeof(wchar_t));
  if (w == NULL) {
    errno = ENOMEM;
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    mrb_free(mrb, w);
    errno = EINVAL;
    return NULL;
  }
  return w;
}

/* Quote one argument the way the C runtime's command-line parser reads it
   back, so that argv[] in the child is the argv[] that was asked for. */
static void
wbuf_add_argument(struct wbuf *b, const wchar_t *arg)
{
  const wchar_t *p;
  int needs_quotes = (*arg == L'\0');

  for (p = arg; *p != L'\0'; p++) {
    if (*p == L' ' || *p == L'\t' || *p == L'"') {
      needs_quotes = 1;
      break;
    }
  }
  if (!needs_quotes) {
    wbuf_add(b, arg, wcslen(arg));
    return;
  }

  wbuf_add_wchar(b, L'"');
  for (p = arg; *p != L'\0'; p++) {
    size_t backslashes = 0;

    while (*p == L'\\') {
      backslashes++;
      p++;
    }
    if (*p == L'\0') {
      /* Backslashes before the closing quote are doubled. */
      for (; backslashes > 0; backslashes--) wbuf_add(b, L"\\\\", 2);
      break;
    }
    if (*p == L'"') {
      for (; backslashes > 0; backslashes--) wbuf_add(b, L"\\\\", 2);
      wbuf_add(b, L"\\\"", 2);
    }
    else {
      for (; backslashes > 0; backslashes--) wbuf_add_wchar(b, L'\\');
      wbuf_add_wchar(b, *p);
    }
  }
  wbuf_add_wchar(b, L'"');
}

/* The command line CreateProcessW is given.  A shell command goes to cmd.exe
   verbatim; an argv is quoted back into one string, because a command line
   is all Windows has. */
static wchar_t*
build_command_line(mrb_state *mrb, const mrb_process_spawn_params *params)
{
  struct wbuf b;
  size_t i;

  wbuf_init(&b, mrb);
  if (params->kind == MRB_PROCESS_SPAWN_SHELL) {
    wchar_t shell[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"COMSPEC", shell, MAX_PATH);
    wchar_t *command;

    if (n == 0 || n >= MAX_PATH) wcscpy(shell, L"cmd.exe");
    command = utf8_to_wide(mrb, params->argv[0]);
    if (command == NULL) {
      wbuf_free(&b);
      return NULL;
    }
    wbuf_add(&b, shell, wcslen(shell));
    wbuf_add(&b, L" /c ", 4);
    wbuf_add(&b, command, wcslen(command));
    mrb_free(mrb, command);
  }
  else {
    for (i = 0; params->argv[i] != NULL; i++) {
      wchar_t *arg = utf8_to_wide(mrb, params->argv[i]);

      if (arg == NULL) {
        wbuf_free(&b);
        return NULL;
      }
      if (i > 0) wbuf_add_wchar(&b, L' ');
      wbuf_add_argument(&b, arg);
      mrb_free(mrb, arg);
    }
  }
  wbuf_add_wchar(&b, L'\0');

  if (b.failed) {
    wbuf_free(&b);
    errno = ENOMEM;
    return NULL;
  }
  return b.ptr;
}

/* Whether an environment entry, which is "NAME=VALUE", is the one `name`
   names.  Case-insensitively, as Windows names variables. */
static int
env_name_is(const wchar_t *entry, const wchar_t *name)
{
  size_t i;

  for (i = 0; name[i] != L'\0'; i++) {
    wchar_t a = entry[i], c = name[i];

    if (a == L'\0' || a == L'=') return 0;
    if (a >= L'a' && a <= L'z') a = (wchar_t)(a - L'a' + L'A');
    if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    if (a != c) return 0;
  }
  return entry[i] == L'=';
}

/* The order a Windows environment block has to be in: by name, ignoring
   case, and ordinally rather than by the rules of any one locale. */
static int
env_entry_compare(const void *a, const void *b)
{
  const wchar_t *x = *(const wchar_t *const*)a;
  const wchar_t *y = *(const wchar_t *const*)b;
  int r = CompareStringOrdinal(x, -1, y, -1, TRUE);

  /* CSTR_LESS_THAN, CSTR_EQUAL and CSTR_GREATER_THAN are 1, 2 and 3; 0 is
     the call itself failing, which leaves the two in the order they were. */
  return (r == 0) ? 0 : r - CSTR_EQUAL;
}

/* "NAME=VALUE", from a name already in UTF-16 and a value that is not. */
static wchar_t*
env_pair(mrb_state *mrb, const wchar_t *name, const char *value)
{
  wchar_t *wide = utf8_to_wide(mrb, value);
  wchar_t *pair;
  size_t nlen, vlen;

  if (wide == NULL) return NULL;
  nlen = wcslen(name);
  vlen = wcslen(wide);
  pair = (wchar_t*)mrb_malloc_simple(mrb, (nlen + vlen + 2) * sizeof(wchar_t));
  if (pair == NULL) {
    mrb_free(mrb, wide);
    errno = ENOMEM;
    return NULL;
  }
  memcpy(pair, name, nlen * sizeof(wchar_t));
  pair[nlen] = L'=';
  memcpy(pair + nlen + 1, wide, (vlen + 1) * sizeof(wchar_t));
  mrb_free(mrb, wide);
  return pair;
}

/* The environment block for the child: the parent's, with the deltas applied,
   sorted as the API asks for.  NULL means "inherit unchanged", which is what
   CreateProcessW reads a NULL environment as. */
static int
build_environment(mrb_state *mrb, const mrb_process_spawn_params *params, wchar_t **out)
{
  struct wbuf b;
  wchar_t *parent = NULL;
  const wchar_t **entries = NULL;
  wchar_t **owned = NULL;
  size_t nentries = 0, nowned = 0, nparent = 0, i;
  int keep_parent = !(params->flags & MRB_PROCESS_SPAWN_UNSETENV_OTHERS);
  int result = -1;

  *out = NULL;
  if (params->nenv == 0 && keep_parent) return 0;

  wbuf_init(&b, mrb);
  if (keep_parent) {
    const wchar_t *entry;

    parent = GetEnvironmentStringsW();
    if (parent == NULL) {
      errno = ENOMEM;
      return -1;
    }
    for (entry = parent; *entry != L'\0'; entry += wcslen(entry) + 1) nparent++;
  }

  entries = (const wchar_t**)mrb_malloc_simple(mrb, sizeof(wchar_t*) * (nparent + params->nenv + 1));
  /* Each delta needs its name, which the parent's entries are matched
     against, and the pair that goes into the block. */
  owned = (wchar_t**)mrb_malloc_simple(mrb, sizeof(wchar_t*) * (params->nenv * 2 + 1));
  if (entries == NULL || owned == NULL) {
    errno = ENOMEM;
    goto done;
  }

  for (i = 0; i < params->nenv; i++) {
    wchar_t *name = utf8_to_wide(mrb, params->env[i].name);

    if (name == NULL) goto done;
    owned[nowned++] = name;
  }

  if (keep_parent) {
    const wchar_t *entry;

    for (entry = parent; *entry != L'\0'; entry += wcslen(entry) + 1) {
      int overridden = 0;

      for (i = 0; i < params->nenv; i++) {
        if (env_name_is(entry, owned[i])) {
          overridden = 1;
          break;
        }
      }
      /* An entry the deltas name is written below, or, where the delta
         unsets it, not written at all. */
      if (!overridden) entries[nentries++] = entry;
    }
  }

  for (i = 0; i < params->nenv; i++) {
    wchar_t *pair;

    if (params->env[i].value == NULL) continue;
    pair = env_pair(mrb, owned[i], params->env[i].value);
    if (pair == NULL) goto done;
    owned[nowned++] = pair;
    entries[nentries++] = pair;
  }

  /* The block the API documents is a sorted one, and what is assembled here
     is the parent's order followed by the caller's, which is not one. */
  if (nentries > 1) {
    qsort((void*)entries, nentries, sizeof(entries[0]), env_entry_compare);
  }
  for (i = 0; i < nentries; i++) wbuf_add_wstr(&b, entries[i]);

  /* The block is a run of NUL-terminated strings followed by one more NUL.
     An empty environment is the two NULs alone, not one. */
  if (b.len == 0) wbuf_add_wchar(&b, L'\0');
  wbuf_add_wchar(&b, L'\0');

  if (b.failed) {
    errno = ENOMEM;
    goto done;
  }
  *out = b.ptr;
  b.ptr = NULL;
  result = 0;

done:
  wbuf_free(&b);
  for (i = 0; i < nowned; i++) mrb_free(mrb, owned[i]);
  mrb_free(mrb, owned);
  mrb_free(mrb, (void*)entries);
  if (parent != NULL) FreeEnvironmentStringsW(parent);
  return result;
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

/* A command line that is nothing but blanks names no command, and handing it
   to the shell would start one that does nothing and reports success.  What
   the caller asked to run does not exist, which is what ENOENT says. */
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
  struct mrb_hal_process_child *child = NULL;
  STARTUPINFOEXW si;
  PROCESS_INFORMATION pi;
  LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
  SIZE_T attrs_size = 0;
  BOOL attrs_ready = FALSE;
  DWORD flags = 0;
  HANDLE slots[3], wanted[3];
  HANDLE inherit[3];
  HANDLE dups[3] = { NULL, NULL, NULL };
  DWORD ninherit = 0, i, j;
  wchar_t *cmdline = NULL;
  wchar_t *envblock = NULL;
  wchar_t *workdir = NULL;
  int saved_errno;

  if (params->argv == NULL || params->argv[0] == NULL ||
      command_is_blank(params->argv[0])) {
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
  if (params->chdir != NULL) {
    workdir = utf8_to_wide(mrb, params->chdir);
    if (workdir == NULL) goto error;
  }
  /* The block above is UTF-16, and saying so is what keeps CreateProcessW
     from reading it as bytes in the active code page. */
  if (envblock != NULL) flags |= CREATE_UNICODE_ENVIRONMENT;

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
    /* The same handle in two slots, which is `2>&1` and its like, is duplicated
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
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrs_size)) {
      /* Nothing was initialised, so there is nothing to delete: what the
         cleanup below has to let go of is the memory, and only that. */
      set_errno_from_win32(GetLastError());
      goto error;
    }
    attrs_ready = TRUE;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit, ninherit * sizeof(HANDLE), NULL, NULL)) {
      set_errno_from_win32(GetLastError());
      goto error;
    }
    si.lpAttributeList = attrs;
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }

  /* Handles cross only where a list says which.  Asking for inheritance
     without one hands the child every inheritable handle this process has,
     which is the opposite of what the list is for: one spawn's pipes would
     leak into the next, and a read end nobody meant to give away keeps a
     pipe from ever reaching EOF.  With no handle to pass, nothing is passed. */
  memset(&pi, 0, sizeof(pi));
  if (!CreateProcessW(NULL, cmdline, NULL, NULL, (ninherit > 0) ? TRUE : FALSE,
                      flags, envblock,
                      workdir, &si.StartupInfo, &pi)) {
    set_errno_from_win32(GetLastError());
    goto error;
  }

  CloseHandle(pi.hThread);
  for (i = 0; i < 3; i++) {
    if (dups[i] != NULL) CloseHandle(dups[i]);
  }
  if (attrs != NULL) {
    if (attrs_ready) DeleteProcThreadAttributeList(attrs);
    mrb_free(mrb, attrs);
  }
  mrb_free(mrb, cmdline);
  mrb_free(mrb, envblock);
  mrb_free(mrb, workdir);

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
    if (attrs_ready) DeleteProcThreadAttributeList(attrs);
    mrb_free(mrb, attrs);
  }
  mrb_free(mrb, cmdline);
  mrb_free(mrb, envblock);
  mrb_free(mrb, workdir);
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
      DWORD ready = WaitForSingleObject(child->handle, 0);

      if (ready == WAIT_OBJECT_0) {
        found = child;
        break;
      }
      /* A wait that could not be performed is not a child that has not
         finished yet.  Passing over it would answer "nothing ready" for a
         handle that can no longer be waited on at all, and a poll that keeps
         saying that is a wait that never ends. */
      if (ready == WAIT_FAILED) {
        set_errno_from_win32(GetLastError());
        return -1;
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
                     const mrb_process_wait_target *target, unsigned int flags,
                     mrb_process_event *event)
{
  DWORD result;

  if (target->scope == MRB_PROCESS_WAIT_SCOPE_ANY) {
    return wait_any(mrb, ctx, flags, event);
  }
  if (target->scope == MRB_PROCESS_WAIT_SCOPE_PID) {
    /* A wait here is a wait on a handle, and the only handles this port has
       are the ones it opened by spawning.  A pid it never spawned is a pid it
       cannot wait for, which is what ECHILD says; opening the process anew
       would be waiting on whoever holds the number now. */
    errno = ECHILD;
    return -1;
  }
  if (target->scope == MRB_PROCESS_WAIT_SCOPE_GROUP) {
    /* A Win32 process group is what GenerateConsoleCtrlEvent() addresses, and
       nothing a wait can be narrowed by: a wait here takes handles, and a
       group is not one.  Saying so beats waiting on every child instead. */
    errno = ENOSYS;
    return -1;
  }

  /* MRB_PROCESS_WAIT_UNTRACED asks for stopped children, of which Windows
     has none, so it changes nothing here. */
  result = WaitForSingleObject(target->child->handle,
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
  return finish_wait(target->child, event);
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

/*
 * Clocks
 *
 * Four clocks, each read through the Win32 call that suits it, and for each
 * way of reading one the granularity that way reads at.  The two travel
 * together as one `win_clock`, so neither is chosen without the other.
 */

/* A FILETIME counts 100ns intervals.  The wall-clock one is counted from
   1601-01-01 UTC, which is FILETIME_EPOCH_DELTA of them before the Unix
   epoch.

   That 100ns is what this port answers as the granularity of every reading
   written as a FILETIME: two moments closer together than a tick have no
   room to be written down apart, and Windows states no rate at which the
   clocks behind those readings move. */
#define FILETIME_TICKS_PER_SEC 10000000LL
#define FILETIME_EPOCH_DELTA   116444736000000000LL
#define NSEC_PER_FILETIME_TICK 100

static uint64_t
filetime_to_u64(const FILETIME *ft)
{
  return ((uint64_t)ft->dwHighDateTime << 32) | (uint64_t)ft->dwLowDateTime;
}

/* Split a count of FILETIME ticks into seconds and nanoseconds, normalizing
   a negative count downwards so that the nanoseconds land in the
   [0, 999999999] the HAL promises. */
static void
clock_time_from_ticks(mrb_process_clock_time *t, int64_t ticks)
{
  t->sec = ticks / FILETIME_TICKS_PER_SEC;
  t->nsec = (ticks % FILETIME_TICKS_PER_SEC) * NSEC_PER_FILETIME_TICK;

  if (t->nsec < 0) {
    t->sec -= 1;
    t->nsec += NSEC_PER_SEC;
  }
}

/*
 * A Win32 entry point this port resolves at run time.
 *
 * Looked up rather than linked, so that one binary runs both on the Windows
 * that has the call and on the one that does not, and remembered rather than
 * looked up per reading, since a clock is read in loops.
 *
 * `slot` holds NULL before anything has looked, PROC_ABSENT once a lookup
 * has come back empty, and the entry point itself otherwise.  PROC_ABSENT is
 * any value a lookup could not return; NULL cannot serve, being what the
 * slot already says before anyone has looked.
 *
 * The slot is published and read back through interlocked operations.  Two
 * threads reading a clock at once would otherwise be writing and reading a
 * plain static without synchronization, which is a data race whether or not
 * they arrive at the same answer, and a race is undefined rather than merely
 * unlikely to matter.  InitOnceExecuteOnce() would say this more directly,
 * but it is Vista and later, and binding to it would cost this port the
 * older Windows that resolving at run time is here to keep working on; the
 * interlocked calls have been there since XP.
 */
typedef struct win_proc {
  PVOID volatile slot;
  const wchar_t *dll;
  const char *name;
} win_proc;

#define PROC_ABSENT ((PVOID)(INT_PTR)-1)

static PVOID
resolve_proc(win_proc *p)
{
  PVOID resolved = InterlockedCompareExchangePointer(&p->slot, NULL, NULL);

  if (resolved == NULL) {
    HMODULE dll = GetModuleHandleW(p->dll);
    /* Through void*: a FARPROC is not the function's own type, and a direct
       cast between the two is what -Wcast-function-type warns about. */
    resolved = (dll == NULL) ? NULL
             : (PVOID)(void*)GetProcAddress(dll, p->name);
    if (resolved == NULL) resolved = PROC_ABSENT;
    /* Every thread that looked found the same entry point, so a later
       write only repeats what is already in the slot. */
    InterlockedExchangePointer(&p->slot, resolved);
  }
  return (resolved == PROC_ABSENT) ? NULL : resolved;
}

/* GetSystemTimePreciseAsFileTime(), or NULL where this Windows is older than
   8 and has only the coarse reading. */
typedef VOID (WINAPI *precise_system_time_fn)(LPFILETIME);

static win_proc precise_time_proc = {
  NULL, L"kernel32.dll", "GetSystemTimePreciseAsFileTime"
};

static precise_system_time_fn
precise_system_time(void)
{
  return (precise_system_time_fn)resolve_proc(&precise_time_proc);
}

/* NtQueryTimerResolution() answers a live "CurrentResolution": the interval
   the clock interrupt is firing at right now, in the FILETIME's unit.  That
   is what the coarse wall clock moves by, and it is the one number Windows
   will state that follows a timeBeginPeriod() call in either direction, so a
   caller reading a clock on a system that has raised the interrupt rate is
   told the rate it is being read at rather than the one the system booted
   with.

   Undocumented, but read rather than guessed at: it is ntdll's half of the
   same pair NtSetTimerResolution() is, has answered this since XP, and is
   how the runtimes already shipping on every Windows this port supports
   answer the same question.  Where it cannot be reached,
   GetSystemTimeAdjustment()'s TimeIncrement stands in below, naming the
   interrupt the system booted with rather than the one it is keeping now. */
typedef LONG (NTAPI *nt_query_timer_resolution_fn)(PULONG, PULONG, PULONG);

static win_proc timer_resolution_proc = {
  NULL, L"ntdll.dll", "NtQueryTimerResolution"
};

static nt_query_timer_resolution_fn
nt_query_timer_resolution(void)
{
  return (nt_query_timer_resolution_fn)resolve_proc(&timer_resolution_proc);
}

/* The performance counter and its frequency, which is what the monotonic
   clock is read from.  Windows documents both calls as succeeding on every
   version this port supports, so the failure arm below is only there to keep
   an undocumented failure from being read as a time.  A frequency above
   INT64_MAX / NSEC_PER_SEC, about 9.2GHz, is refused rather than wrapped:
   `(counter % freq) * NSEC_PER_SEC` would overflow past it. */
static int
performance_counter(int64_t *counter, int64_t *freq)
{
  LARGE_INTEGER c, f;

  if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c)) {
    set_errno_from_win32(GetLastError());
    return -1;
  }
  if (f.QuadPart <= 0 || f.QuadPart > INT64_MAX / NSEC_PER_SEC) {
    errno = EINVAL;
    return -1;
  }
  *counter = (int64_t)c.QuadPart;
  *freq = (int64_t)f.QuadPart;
  return 0;
}

/*
 * Readings
 */

static void
wall_clock_from_filetime(mrb_process_clock_time *t, const FILETIME *ft)
{
  clock_time_from_ticks(t, (int64_t)filetime_to_u64(ft) - FILETIME_EPOCH_DELTA);
}

/* The wall clock as GetSystemTimePreciseAsFileTime() interpolates it between
   two clock interrupts.  win_clock_for() selects this pair only after the
   lookup has answered, so the entry point here is never NULL. */
static int
precise_wall_read(mrb_process_clock_time *t)
{
  FILETIME ft;

  precise_system_time()(&ft);
  wall_clock_from_filetime(t, &ft);
  return 0;
}

/* The wall clock as the clock interrupt leaves it, which is the only reading
   of it a Windows older than 8 has. */
static int
coarse_wall_read(mrb_process_clock_time *t)
{
  FILETIME ft;

  GetSystemTimeAsFileTime(&ft);
  wall_clock_from_filetime(t, &ft);
  return 0;
}

static int
monotonic_read(mrb_process_clock_time *t)
{
  int64_t counter, freq;

  /* The performance counter never steps, and Windows names no origin for it
     beyond a point it fixes itself and holds while the system runs.  That is
     all this clock is asked for: the origin stands for the life of the
     process, so two readings can be subtracted, which is what a caller has
     one of these for. */
  if (performance_counter(&counter, &freq) != 0) return -1;
  t->sec = counter / freq;
  t->nsec = (counter % freq) * NSEC_PER_SEC / freq;
  return 0;
}

/* The CPU time a process or a thread has spent, as the sum of the kernel and
   user halves Win32 reports it in. */
static int
cpu_time(mrb_bool thread, mrb_process_clock_time *t)
{
  FILETIME creation, exit, kernel, user;
  BOOL ok;

  if (thread) {
    ok = GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
  }
  else {
    ok = GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
  }
  if (!ok) {
    set_errno_from_win32(GetLastError());
    return -1;
  }
  clock_time_from_ticks(t, (int64_t)(filetime_to_u64(&kernel) +
                                     filetime_to_u64(&user)));
  return 0;
}

static int
process_cpu_read(mrb_process_clock_time *t)
{
  return cpu_time(FALSE, t);
}

static int
thread_cpu_read(mrb_process_clock_time *t)
{
  return cpu_time(TRUE, t);
}

/*
 * Granularities
 */

/* One tick of a FILETIME: the precise wall clock and both CPU clocks are all
   read as FILETIMEs, so this is the granularity of all three.  The FILETIME
   macros above say why a tick is what this port can state about them. */
static int
filetime_tick_resolution(mrb_process_clock_time *t)
{
  t->sec = 0;
  t->nsec = NSEC_PER_FILETIME_TICK;
  return 0;
}

/* The interval between two clock interrupts as it stands now, which is what
   the coarse wall clock moves by: that reading does not interpolate, so it
   is no finer than the interrupt that updates it. */
static int
clock_interrupt_resolution(mrb_process_clock_time *t)
{
  nt_query_timer_resolution_fn query = nt_query_timer_resolution();
  ULONG minimum, maximum, current;
  DWORD adjustment, increment;
  BOOL disabled;

  t->sec = 0;
  if (query != NULL && query(&minimum, &maximum, &current) == 0) {
    t->nsec = (int64_t)current * NSEC_PER_FILETIME_TICK;
    return 0;
  }
  if (!GetSystemTimeAdjustment(&adjustment, &increment, &disabled)) {
    set_errno_from_win32(GetLastError());
    return -1;
  }
  t->nsec = (int64_t)increment * NSEC_PER_FILETIME_TICK;
  return 0;
}

/* A tick of the performance counter, rounded up.  A frequency that does not
   divide a second into whole nanoseconds would otherwise be reported as
   finer than it is, saying that two readings a tick apart can differ by less
   than a tick; rounding up also keeps a frequency above 1GHz from flooring
   to nothing, and a granularity of nothing is not one. */
static int
performance_counter_resolution(mrb_process_clock_time *t)
{
  int64_t counter, freq;

  /* The counter is read and dropped: performance_counter() answers both, and
     a granularity is the frequency alone. */
  if (performance_counter(&counter, &freq) != 0) return -1;
  t->sec = 0;
  t->nsec = (NSEC_PER_SEC + freq - 1) / freq;
  return 0;
}

/*
 * A way of reading one clock, paired with the granularity that way of
 * reading has; mrb_hal_process_clock_getres() in process_hal.h says what
 * such a granularity is.  Keeping the two in one struct is what stops them
 * drifting apart, and three pairs share filetime_tick_resolution() because
 * three of the readings are FILETIMEs.
 */
typedef struct win_clock {
  int (*read)(mrb_process_clock_time *t);
  int (*resolution)(mrb_process_clock_time *t);
} win_clock;

static const win_clock precise_wall_clock =
  { precise_wall_read, filetime_tick_resolution };
static const win_clock coarse_wall_clock =
  { coarse_wall_read, clock_interrupt_resolution };
static const win_clock monotonic_clock =
  { monotonic_read, performance_counter_resolution };
static const win_clock process_cpu_clock =
  { process_cpu_read, filetime_tick_resolution };
static const win_clock thread_cpu_clock =
  { thread_cpu_read, filetime_tick_resolution };

static const win_clock *
win_clock_for(mrb_int clock_id)
{
  switch (clock_id) {
  case MRB_PROCESS_CLOCK_REALTIME:
    /* Which wall clock this Windows has decides both how it is read and how
       finely it is read, so it is asked once and answered as a pair. */
    if (precise_system_time() != NULL) return &precise_wall_clock;
    return &coarse_wall_clock;
  case MRB_PROCESS_CLOCK_MONOTONIC:       return &monotonic_clock;
  case MRB_PROCESS_CLOCK_PROCESS_CPUTIME: return &process_cpu_clock;
  case MRB_PROCESS_CLOCK_THREAD_CPUTIME:  return &thread_cpu_clock;
  default:                                return NULL;
  }
}

int
mrb_hal_process_clock_gettime(mrb_state *mrb, mrb_int clock_id,
                              mrb_process_clock_time *t)
{
  const win_clock *c = win_clock_for(clock_id);
  (void)mrb;

  if (c == NULL) {
    errno = EINVAL;
    return -1;
  }
  return c->read(t);
}

int
mrb_hal_process_clock_getres(mrb_state *mrb, mrb_int clock_id,
                             mrb_process_clock_time *t)
{
  const win_clock *c = win_clock_for(clock_id);
  (void)mrb;

  if (c == NULL) {
    errno = EINVAL;
    return -1;
  }
  return c->resolution(t);
}
