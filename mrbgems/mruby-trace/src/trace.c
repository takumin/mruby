/*
** trace.c - a call tracer that aggregates folded stacks
**
** The tracer rides on the call frame hooks of `mrb_state`
** (`MRB_USE_CALL_HOOK`).  The enter hook only takes a timestamp, because
** the method is not looked up yet when a frame is pushed; the leave hook
** does the real work, reading the frame that is about to be popped and
** the callers still alive below it.
**
** Every leave folds one sample into a table keyed by the whole stack
** ("<main>;Foo#outer;Foo#inner"), holding the time spent in that frame
** minus the time its children took.  That is the FlameGraph "folded
** stacks" format, so the output pipes straight into flamegraph.pl.
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/presym.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
# include <windows.h>
#endif

#ifndef MRB_USE_CALL_HOOK
# error mruby-trace needs MRB_USE_CALL_HOOK; the gem defines it via mrbgem.rake
#endif

/* ------------------------------------------------------------------ */
/* monotonic clock                                                     */
/* ------------------------------------------------------------------ */

static uint64_t
trace_now(void)
{
#if defined(_WIN32)
  LARGE_INTEGER freq, cnt;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&cnt);
  if (freq.QuadPart == 0) return 0;
  return (uint64_t)((double)cnt.QuadPart * 1e9 / (double)freq.QuadPart);
#elif defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
  return (uint64_t)clock() * (1000000000ull / CLOCKS_PER_SEC);
#endif
}

/* ------------------------------------------------------------------ */
/* data                                                                */
/* ------------------------------------------------------------------ */

typedef struct trace_frame {
  uint64_t enter;               /* timestamp taken by the enter hook */
  uint64_t child;               /* time spent in frames called from here */
  uint32_t path_end;            /* end of this frame's name in ctx->path */
} trace_frame;

/* Shadow stack of one `mrb_context`.  Frames are indexed by their depth
   in the context's callinfo stack, so a lost leave (a frame thrown away
   by exception unwinding, say) costs one sample instead of desyncing the
   whole stack. */
typedef struct trace_ctx {
  const struct mrb_context *c;
  const mrb_callinfo *cibase;   /* to notice a context reusing an address */
  size_t cicap;
  trace_frame *frames;
  size_t len, cap;
  char *path;                   /* "<main>;Foo#outer" for frames < resolved */
  size_t path_len, path_cap;
  size_t resolved;              /* frames whose name is already in path */
} trace_ctx;

typedef struct trace_stack {
  char *key;                    /* NULL when the slot is free */
  uint32_t klen;
  uint32_t hash;
  uint64_t self_ns;
  uint64_t calls;
} trace_stack;

typedef struct trace_state {
  mrb_bool running;
  mrb_bool broken;              /* out of memory: the data is incomplete */
  trace_ctx *ctxs;
  size_t nctx, ctxcap;
  trace_stack *tab;             /* open addressing, tabsize is a power of 2 */
  size_t tabsize, tabused;
  uint64_t started, elapsed;
} trace_state;

#define TRACE_TAB_INIT 64

static void*
trace_zalloc(mrb_state *mrb, size_t size)
{
  void *p = mrb_malloc_simple(mrb, size);
  if (p) memset(p, 0, size);
  return p;
}

static trace_state*
trace_state_get(mrb_state *mrb)
{
  return (trace_state*)mrb->call_hook_ud;
}

/* ------------------------------------------------------------------ */
/* folded stack table                                                  */
/* ------------------------------------------------------------------ */

static uint32_t
trace_hash(const char *p, size_t len)
{
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)p[i];
    h *= 16777619u;
  }
  return h;
}

static void
trace_tab_put(trace_stack *tab, size_t size, trace_stack *e)
{
  size_t i = e->hash & (size - 1);
  while (tab[i].key) i = (i + 1) & (size - 1);
  tab[i] = *e;
}

static mrb_bool
trace_tab_grow(mrb_state *mrb, trace_state *st)
{
  size_t size = st->tabsize ? st->tabsize * 2 : TRACE_TAB_INIT;
  trace_stack *tab = (trace_stack*)trace_zalloc(mrb, size * sizeof(trace_stack));
  if (!tab) return FALSE;
  for (size_t i = 0; i < st->tabsize; i++) {
    if (st->tab[i].key) trace_tab_put(tab, size, &st->tab[i]);
  }
  mrb_free(mrb, st->tab);
  st->tab = tab;
  st->tabsize = size;
  return TRUE;
}

static void
trace_fold(mrb_state *mrb, trace_state *st, const char *key, size_t klen, uint64_t self_ns)
{
  if (st->tabsize == 0 || (st->tabused + 1) * 4 >= st->tabsize * 3) {
    if (!trace_tab_grow(mrb, st)) {
      st->broken = TRUE;
      return;
    }
  }

  uint32_t h = trace_hash(key, klen);
  size_t i = h & (st->tabsize - 1);
  while (st->tab[i].key) {
    trace_stack *e = &st->tab[i];
    if (e->hash == h && e->klen == klen && memcmp(e->key, key, klen) == 0) {
      e->self_ns += self_ns;
      e->calls++;
      return;
    }
    i = (i + 1) & (st->tabsize - 1);
  }

  char *copy = (char*)mrb_malloc_simple(mrb, klen + 1);
  if (!copy) {
    st->broken = TRUE;
    return;
  }
  memcpy(copy, key, klen);
  copy[klen] = '\0';
  st->tab[i].key = copy;
  st->tab[i].klen = (uint32_t)klen;
  st->tab[i].hash = h;
  st->tab[i].self_ns = self_ns;
  st->tab[i].calls = 1;
  st->tabused++;
}

static void
trace_tab_clear(mrb_state *mrb, trace_state *st)
{
  for (size_t i = 0; i < st->tabsize; i++) {
    mrb_free(mrb, st->tab[i].key);
  }
  mrb_free(mrb, st->tab);
  st->tab = NULL;
  st->tabsize = st->tabused = 0;
}

/* ------------------------------------------------------------------ */
/* shadow stacks                                                       */
/* ------------------------------------------------------------------ */

static void
trace_ctx_reset(trace_ctx *tc)
{
  tc->len = 0;
  tc->resolved = 0;
  tc->path_len = 0;
}

static trace_ctx*
trace_ctx_for(mrb_state *mrb, trace_state *st, const struct mrb_context *c)
{
  trace_ctx *tc = NULL;

  for (size_t i = 0; i < st->nctx; i++) {
    if (st->ctxs[i].c == c) {
      tc = &st->ctxs[i];
      break;
    }
  }

  if (!tc) {
    if (st->nctx == st->ctxcap) {
      size_t cap = st->ctxcap ? st->ctxcap * 2 : 4;
      trace_ctx *p = (trace_ctx*)mrb_realloc_simple(mrb, st->ctxs, cap * sizeof(trace_ctx));
      if (!p) {
        st->broken = TRUE;
        return NULL;
      }
      st->ctxs = p;
      st->ctxcap = cap;
    }
    tc = &st->ctxs[st->nctx++];
    memset(tc, 0, sizeof(*tc));
    tc->c = c;
  }

  /* A grown callinfo stack keeps its frames, a recycled context does not.
     Growth always widens the stack, so a base that moved without gaining
     room means this address now belongs to somebody else. */
  size_t cicap = (size_t)(c->ciend - c->cibase);
  if (tc->cibase != c->cibase) {
    if (cicap <= tc->cicap) trace_ctx_reset(tc);
    tc->cibase = c->cibase;
    tc->cicap = cicap;
  }

  return tc;
}

static void
trace_ctx_free(mrb_state *mrb, trace_state *st)
{
  for (size_t i = 0; i < st->nctx; i++) {
    mrb_free(mrb, st->ctxs[i].frames);
    mrb_free(mrb, st->ctxs[i].path);
  }
  mrb_free(mrb, st->ctxs);
  st->ctxs = NULL;
  st->nctx = st->ctxcap = 0;
}

static mrb_bool
trace_frames_reserve(mrb_state *mrb, trace_ctx *tc, size_t need)
{
  if (need <= tc->cap) return TRUE;
  size_t cap = tc->cap ? tc->cap : 32;
  while (cap < need) cap *= 2;
  trace_frame *p = (trace_frame*)mrb_realloc_simple(mrb, tc->frames, cap * sizeof(trace_frame));
  if (!p) return FALSE;
  tc->frames = p;
  tc->cap = cap;
  return TRUE;
}

static mrb_bool
trace_path_cat(mrb_state *mrb, trace_ctx *tc, const char *s, size_t len)
{
  if (tc->path_len + len + 1 > tc->path_cap) {
    size_t cap = tc->path_cap ? tc->path_cap : 256;
    while (cap < tc->path_len + len + 1) cap *= 2;
    char *p = (char*)mrb_realloc_simple(mrb, tc->path, cap);
    if (!p) return FALSE;
    tc->path = p;
    tc->path_cap = cap;
  }
  memcpy(tc->path + tc->path_len, s, len);
  tc->path_len += len;
  tc->path[tc->path_len] = '\0';
  return TRUE;
}

/* A name a program picked can hold the very characters that hold the
   folded format together, so it gets in with those spelled away. */
static mrb_bool
trace_path_cat_name(mrb_state *mrb, trace_ctx *tc, const char *s, size_t len)
{
  size_t begin = tc->path_len;
  if (!trace_path_cat(mrb, tc, s, len)) return FALSE;
  for (size_t i = begin; i < tc->path_len; i++) {
    char ch = tc->path[i];
    if (ch == ';' || ch == ' ' || ch == '\n' || ch == '\r') tc->path[i] = '_';
  }
  return TRUE;
}

#define TRACE_LIT(mrb, tc, s) trace_path_cat((mrb), (tc), (s), sizeof(s) - 1)

/* ------------------------------------------------------------------ */
/* frame names                                                         */
/* ------------------------------------------------------------------ */

/* Reads the name a class already carries.  Deliberately avoids
   `mrb_class_name()`, which builds a string for anonymous classes and so
   can allocate in the middle of a frame pop. */
static const char*
trace_class_name(mrb_state *mrb, struct RClass *c, mrb_int *len)
{
  mrb_value name = mrb_obj_iv_get(mrb, (struct RObject*)c, MRB_SYM(__classname__));
  if (mrb_symbol_p(name)) return mrb_sym_name_len(mrb, mrb_symbol(name), len);
  if (mrb_string_p(name)) {
    *len = RSTRING_LEN(name);
    return RSTRING_PTR(name);
  }
  return NULL;
}

static mrb_bool
trace_cat_class(mrb_state *mrb, trace_ctx *tc, struct RClass *c)
{
  mrb_int len;
  const char *name = trace_class_name(mrb, c, &len);
  if (!name) return TRACE_LIT(mrb, tc, "#<Class>");
  return trace_path_cat_name(mrb, tc, name, (size_t)len);
}

/* "Foo#" for instance methods, "Foo." for singleton ones. */
static mrb_bool
trace_cat_owner(mrb_state *mrb, trace_ctx *tc, struct RClass *c)
{
  if (!c) return TRUE;

  if (c->tt == MRB_TT_SCLASS) {
    mrb_value at = mrb_obj_iv_get(mrb, (struct RObject*)c, MRB_SYM(__attached__));
    if (mrb_type(at) == MRB_TT_CLASS || mrb_type(at) == MRB_TT_MODULE ||
        mrb_type(at) == MRB_TT_SCLASS) {
      if (!trace_cat_class(mrb, tc, mrb_class_ptr(at))) return FALSE;
    }
    else if (!TRACE_LIT(mrb, tc, "#<singleton>")) return FALSE;
    return TRACE_LIT(mrb, tc, ".");
  }

  if (!trace_cat_class(mrb, tc, mrb_class_real(c))) return FALSE;
  return TRACE_LIT(mrb, tc, "#");
}

static mrb_bool
trace_cat_frame(mrb_state *mrb, trace_ctx *tc, const mrb_callinfo *ci, size_t depth)
{
  if (depth == 0) {
    return (tc->c == mrb->root_c) ? TRACE_LIT(mrb, tc, "<main>")
                                  : TRACE_LIT(mrb, tc, "<fiber>");
  }

  struct RClass *owner = mrb_vm_ci_target_class(ci);
  const struct RProc *proc = ci->proc;

  if (ci->mid == 0) {
    /* a class or module body carries the scope flag; a block that was
       written where no method was does not */
    if (owner && proc && MRB_PROC_SCOPE_P(proc)) {
      if (!TRACE_LIT(mrb, tc, "<class:")) return FALSE;
      if (!trace_cat_class(mrb, tc, owner)) return FALSE;
      return TRACE_LIT(mrb, tc, ">");
    }
    return TRACE_LIT(mrb, tc, "<block>");
  }

  /* A block runs under the method it was written in and borrows its name,
     so say which of the two this frame is. */
  if (proc && !MRB_PROC_CFUNC_P(proc) &&
      !MRB_PROC_SCOPE_P(proc) && !MRB_PROC_STRICT_P(proc)) {
    if (!TRACE_LIT(mrb, tc, "block in ")) return FALSE;
  }

  if (!trace_cat_owner(mrb, tc, owner)) return FALSE;

  mrb_int len;
  const char *name = mrb_sym_name_len(mrb, ci->mid, &len);
  if (!name) return TRACE_LIT(mrb, tc, "?");
  return trace_path_cat_name(mrb, tc, name, (size_t)len);
}

/* Fills in the names of frames `tc->resolved`..`depth`.  Callers are named
   the first time one of their callees returns, and keep that name for as
   long as they stay on the stack. */
static mrb_bool
trace_resolve(mrb_state *mrb, trace_ctx *tc, size_t depth)
{
  for (size_t i = tc->resolved; i <= depth; i++) {
    if (tc->path_len > 0 && !TRACE_LIT(mrb, tc, ";")) return FALSE;
    if (!trace_cat_frame(mrb, tc, &tc->cibase[i], i)) return FALSE;
    tc->frames[i].path_end = (uint32_t)tc->path_len;
  }
  tc->resolved = depth + 1;
  return TRUE;
}

static void
trace_unresolve(trace_ctx *tc, size_t depth)
{
  if (tc->resolved <= depth) return;
  tc->resolved = depth;
  tc->path_len = depth ? tc->frames[depth - 1].path_end : 0;
}

/* ------------------------------------------------------------------ */
/* hooks                                                               */
/* ------------------------------------------------------------------ */

static void
trace_enter(mrb_state *mrb, const mrb_callinfo *ci)
{
  trace_state *st = trace_state_get(mrb);
  if (!st || st->broken) return;

  const struct mrb_context *c = mrb->c;
  trace_ctx *tc = trace_ctx_for(mrb, st, c);
  if (!tc) return;

  size_t depth = (size_t)(ci - c->cibase);
  if (!trace_frames_reserve(mrb, tc, depth + 1)) {
    st->broken = TRUE;
    return;
  }

  uint64_t now = trace_now();
  /* Frames below this one that we never saw pushed (a fiber's own root,
     or whatever ran before tracing started) count from here on. */
  for (size_t i = tc->len; i < depth; i++) {
    tc->frames[i].enter = now;
    tc->frames[i].child = 0;
  }
  trace_unresolve(tc, depth);
  tc->frames[depth].enter = now;
  tc->frames[depth].child = 0;
  tc->len = depth + 1;
}

static void
trace_leave(mrb_state *mrb, const mrb_callinfo *ci)
{
  trace_state *st = trace_state_get(mrb);
  if (!st || st->broken) return;

  const struct mrb_context *c = mrb->c;
  trace_ctx *tc = trace_ctx_for(mrb, st, c);
  if (!tc) return;

  size_t depth = (size_t)(ci - c->cibase);
  if (depth >= tc->len) return;         /* a frame we never saw entered */

  uint64_t now = trace_now();
  trace_frame *f = &tc->frames[depth];
  uint64_t elapsed = now > f->enter ? now - f->enter : 0;
  uint64_t self = elapsed > f->child ? elapsed - f->child : 0;

  if (!trace_resolve(mrb, tc, depth)) {
    st->broken = TRUE;
    return;
  }
  trace_fold(mrb, st, tc->path, f->path_end, self);

  if (depth > 0) tc->frames[depth - 1].child += elapsed;
  tc->len = depth;
  trace_unresolve(tc, depth);
}

/* ------------------------------------------------------------------ */
/* Ruby interface                                                      */
/* ------------------------------------------------------------------ */

/* Takes back only the hooks this gem installed, in case something else
   holds the other one. */
static void
trace_uninstall(mrb_state *mrb)
{
  if (mrb->call_enter_hook == trace_enter) mrb->call_enter_hook = NULL;
  if (mrb->call_leave_hook == trace_leave) mrb->call_leave_hook = NULL;
}

static trace_state*
trace_state_prepare(mrb_state *mrb)
{
  trace_state *st = trace_state_get(mrb);
  if (!st) {
    st = (trace_state*)trace_zalloc(mrb, sizeof(trace_state));
    if (!st) mrb_raise(mrb, E_RUNTIME_ERROR, "cannot allocate trace buffer");
    mrb->call_hook_ud = st;
  }
  return st;
}

/*
 * call-seq:
 *   Trace.start -> true or false
 *
 * Starts recording.  Returns false if recording was already on.
 */
static mrb_value
trace_m_start(mrb_state *mrb, mrb_value self)
{
  trace_state *st = trace_state_prepare(mrb);
  if (st->running) return mrb_false_value();

  trace_ctx_free(mrb, st);
  st->broken = FALSE;
  st->started = trace_now();

  /* Seed the shadow stack with the frames already running, but stop below
     this very call: `Trace.start` itself must not be sampled, since only
     its leave would be seen. */
  trace_ctx *tc = trace_ctx_for(mrb, st, mrb->c);
  if (tc) {
    size_t depth = (size_t)(mrb->c->ci - mrb->c->cibase);
    if (trace_frames_reserve(mrb, tc, depth + 1)) {
      uint64_t now = trace_now();
      for (size_t i = 0; i < depth; i++) {
        tc->frames[i].enter = now;
        tc->frames[i].child = 0;
      }
      tc->len = depth;
    }
  }

  mrb->call_enter_hook = trace_enter;
  mrb->call_leave_hook = trace_leave;
  st->running = TRUE;
  return mrb_true_value();
}

/*
 * call-seq:
 *   Trace.stop -> true or false
 *
 * Stops recording, keeping what was collected.  Returns false if
 * recording was not on.
 */
static mrb_value
trace_m_stop(mrb_state *mrb, mrb_value self)
{
  trace_state *st = trace_state_get(mrb);

  trace_uninstall(mrb);
  if (!st || !st->running) return mrb_false_value();

  uint64_t now = trace_now();
  st->elapsed += now > st->started ? now - st->started : 0;
  st->running = FALSE;
  return mrb_true_value();
}

/*
 * call-seq:
 *   Trace.running? -> true or false
 */
static mrb_value
trace_m_running_p(mrb_state *mrb, mrb_value self)
{
  trace_state *st = trace_state_get(mrb);
  return mrb_bool_value(st && st->running);
}

/*
 * call-seq:
 *   Trace.clear -> nil
 *
 * Throws away everything recorded so far.  Recording, if on, keeps going.
 */
static mrb_value
trace_m_clear(mrb_state *mrb, mrb_value self)
{
  trace_state *st = trace_state_get(mrb);
  if (!st) return mrb_nil_value();

  trace_tab_clear(mrb, st);
  for (size_t i = 0; i < st->nctx; i++) {
    trace_ctx_reset(&st->ctxs[i]);
  }
  st->elapsed = 0;
  st->started = trace_now();
  st->broken = FALSE;
  return mrb_nil_value();
}

static size_t
trace_utoa(uint64_t v, char *buf)
{
  char tmp[24];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  for (size_t i = 0; i < n; i++) {
    buf[i] = tmp[n - 1 - i];
  }
  return n;
}

static int
trace_cmp_key(const void *a, const void *b)
{
  const trace_stack *x = *(const trace_stack* const*)a;
  const trace_stack *y = *(const trace_stack* const*)b;
  uint32_t len = x->klen < y->klen ? x->klen : y->klen;
  int cmp = memcmp(x->key, y->key, len);
  if (cmp != 0) return cmp;
  return x->klen < y->klen ? -1 : (x->klen > y->klen ? 1 : 0);
}

/*
 * call-seq:
 *   Trace.folded(unit = :ns) -> string
 *
 * The recorded stacks in FlameGraph's folded format: one line per stack,
 * frames separated by `;`, then a space and the value.  The value is the
 * time spent in the innermost frame itself, in nanoseconds (`:ns`) or
 * microseconds (`:us`), or the number of calls (`:calls`).
 *
 * Lines come out sorted, so two runs of the same program can be diffed.
 */
static mrb_value
trace_m_folded(mrb_state *mrb, mrb_value self)
{
  mrb_sym unit = MRB_SYM(ns);
  mrb_get_args(mrb, "|n", &unit);

  int div;
  mrb_bool calls = FALSE;
  if (unit == MRB_SYM(ns)) div = 1;
  else if (unit == MRB_SYM(us)) div = 1000;
  else if (unit == MRB_SYM(calls)) { div = 1; calls = TRUE; }
  else mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown unit: %n", unit);

  trace_state *st = trace_state_get(mrb);
  if (!st) return mrb_str_new_lit(mrb, "");
  if (st->broken) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "trace data is incomplete (out of memory)");
  }
  if (st->tabused == 0) return mrb_str_new_lit(mrb, "");

  trace_stack **list = (trace_stack**)mrb_malloc(mrb, st->tabused * sizeof(trace_stack*));
  size_t n = 0;
  for (size_t i = 0; i < st->tabsize; i++) {
    if (st->tab[i].key) list[n++] = &st->tab[i];
  }
  qsort(list, n, sizeof(trace_stack*), trace_cmp_key);

  mrb_value str = mrb_str_new_capa(mrb, n * 64);
  for (size_t i = 0; i < n; i++) {
    char num[24];
    uint64_t value = calls ? list[i]->calls : list[i]->self_ns / div;
    size_t len = trace_utoa(value, num);
    mrb_str_cat(mrb, str, list[i]->key, list[i]->klen);
    mrb_str_cat_lit(mrb, str, " ");
    mrb_str_cat(mrb, str, num, len);
    mrb_str_cat_lit(mrb, str, "\n");
  }
  mrb_free(mrb, list);
  return str;
}

/*
 * call-seq:
 *   Trace.size -> integer
 *
 * How many distinct stacks were recorded.
 */
static mrb_value
trace_m_size(mrb_state *mrb, mrb_value self)
{
  trace_state *st = trace_state_get(mrb);
  return mrb_int_value(mrb, st ? (mrb_int)st->tabused : 0);
}

/*
 * call-seq:
 *   Trace.elapsed -> integer
 *
 * Nanoseconds spent with recording on.
 */
static mrb_value
trace_m_elapsed(mrb_state *mrb, mrb_value self)
{
  trace_state *st = trace_state_get(mrb);
  if (!st) return mrb_int_value(mrb, 0);
  uint64_t ns = st->elapsed;
  if (st->running) {
    uint64_t now = trace_now();
    ns += now > st->started ? now - st->started : 0;
  }
  return mrb_int_value(mrb, (mrb_int)ns);
}

static void
trace_state_free(mrb_state *mrb)
{
  trace_state *st = trace_state_get(mrb);
  if (!st) return;

  trace_uninstall(mrb);
  mrb->call_hook_ud = NULL;
  trace_tab_clear(mrb, st);
  trace_ctx_free(mrb, st);
  mrb_free(mrb, st);
}

void
mrb_mruby_trace_gem_init(mrb_state *mrb)
{
  struct RClass *m = mrb_define_module_id(mrb, MRB_SYM(Trace));

  mrb_define_class_method_id(mrb, m, MRB_SYM(start), trace_m_start, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, m, MRB_SYM(stop), trace_m_stop, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, m, MRB_SYM_Q(running), trace_m_running_p, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, m, MRB_SYM(clear), trace_m_clear, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, m, MRB_SYM(folded), trace_m_folded, MRB_ARGS_OPT(1));
  mrb_define_class_method_id(mrb, m, MRB_SYM(size), trace_m_size, MRB_ARGS_NONE());
  mrb_define_class_method_id(mrb, m, MRB_SYM(elapsed), trace_m_elapsed, MRB_ARGS_NONE());

  mrb_state_atexit(mrb, trace_state_free);
}

void
mrb_mruby_trace_gem_final(mrb_state *mrb)
{
}
