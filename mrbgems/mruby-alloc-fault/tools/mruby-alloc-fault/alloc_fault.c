/*
** alloc_fault.c - run a scenario with an injected allocation failure
**
** Every allocation mruby makes goes through mrb_basic_alloc_func(), and an
** application replaces the allocator by defining that function itself
** (doc/guides/memory.md).  The definition below is linked into this
** executable, so the linker resolves the reference gc.c makes here and never
** pulls src/allocf.o out of libmruby: no duplicate symbol, and no other
** program in the build is affected.
**
** The scenario is either one of the C scenarios named below, which drive
** mrb_open()/mrb_close() themselves, or a piece of Ruby given with -e or -f.
** Ruby is compiled with the injection disarmed and only then run with it
** armed, so that the allocation numbering follows the execution of the
** scenario rather than the parse of its source; --compile-in-scope puts the
** compile in the armed region instead.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/throw.h>
#include <mruby/variable.h>

extern mrb_state *global_mrb; /* defined in mruby-compiler (ccontext.c) */

/* ------------------------------------------------------------------ */
/* the injecting allocator                                            */
/* ------------------------------------------------------------------ */

enum fault_mode {
  FAULT_NONE,    /* count the allocations, refuse none */
  FAULT_STICKY,  /* refuse the at-th allocation and every one after it */
  FAULT_ONCE     /* refuse the at-th allocation alone */
};

struct fault_state {
  enum fault_mode mode;
  mrb_bool armed;
  unsigned long at;      /* 1-based index of the allocation to refuse */
  unsigned long count;   /* allocation requests seen while armed */
  unsigned long refused; /* requests refused */
};

static struct fault_state fault;

/* Arming nests: a scenario that drives the faults itself does so inside the
   region the run already opened, and the two have to agree afterwards.  The
   inner region counts from zero, and what it counted is added to the outer
   count when it closes, because an allocation the inner region saw is one
   the outer region saw too. */
static void
fault_push(struct fault_state *saved, enum fault_mode mode, unsigned long at)
{
  *saved = fault;
  fault.mode = mode;
  fault.at = at;
  fault.count = 0;
  fault.refused = 0;
  fault.armed = TRUE;
}

static void
fault_pop(struct fault_state *saved, unsigned long *counted)
{
  unsigned long count = fault.count, refused = fault.refused;

  if (counted) *counted = count;
  fault = *saved;
  fault.count += count;
  fault.refused += refused;
}

/* Open the outermost region, discarding whatever a previous run left. */
static void
fault_arm(enum fault_mode mode, unsigned long at)
{
  struct fault_state discarded;

  memset(&fault, 0, sizeof(fault));
  fault_push(&discarded, mode, at);
}

static void
fault_disarm(void)
{
  fault.armed = FALSE;
}

/* Whether a refusal is being injected right now, as opposed to allocations
   merely being counted. */
static mrb_bool
fault_injecting(void)
{
  return fault.armed && fault.mode != FAULT_NONE;
}

/* Counts every request for memory, and answers whether this one is refused.
   A free (size == 0) is not a request and is not counted.

   The count covers the retry gc.c makes after a refusal, since that retry is
   itself a call here: under FAULT_STICKY the retry is refused as well, which
   is the "memory is gone" case, while under FAULT_ONCE it succeeds, which is
   the "the emergency collection found room" case. */
static mrb_bool
fault_refuses(void)
{
  if (!fault.armed) return FALSE;
  fault.count++;
  switch (fault.mode) {
  case FAULT_STICKY:
    if (fault.count < fault.at) return FALSE;
    break;
  case FAULT_ONCE:
    if (fault.count != fault.at) return FALSE;
    break;
  default:
    return FALSE;
  }
  fault.refused++;
  return TRUE;
}

void*
mrb_basic_alloc_func(void *p, size_t size)
{
  if (size == 0) {
    free(p);
    return NULL;
  }
  if (fault_refuses()) return NULL;
  return realloc(p, size);
}

/* ------------------------------------------------------------------ */
/* the AllocFault module, for a scenario that drives the faults itself */
/* ------------------------------------------------------------------ */

static mrb_value
scoped_block(mrb_state *mrb, mrb_value blk, enum fault_mode mode, unsigned long at,
             unsigned long *counted)
{
  struct mrb_jmpbuf *prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;
  struct fault_state saved;
  mrb_value ret = mrb_nil_value();

  if (fault_injecting()) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "a refusal is already being injected");
  }

  /* The arming sits inside the TRY so that nothing between here and the
     yield allocates while armed, and the region is closed on both paths so
     that an exception leaving the block does not leave the process
     injecting faults into whatever runs next. */
  MRB_TRY(&c_jmp) {
    mrb->jmp = &c_jmp;
    fault_push(&saved, mode, at);
    ret = mrb_yield_argv(mrb, blk, 0, NULL);
    fault_pop(&saved, counted);
    mrb->jmp = prev_jmp;
  }
  MRB_CATCH(&c_jmp) {
    mrb_value exc;
    fault_pop(&saved, counted);
    mrb->jmp = prev_jmp;
    exc = mrb_obj_value(mrb->exc);
    mrb->exc = NULL;
    mrb_exc_raise(mrb, exc);
  }
  MRB_END_EXC(&c_jmp);

  return ret;
}

/* AllocFault.count { ... } -> Integer
   The number of allocations the block asks for, refusing none. */
static mrb_value
f_count(mrb_state *mrb, mrb_value self)
{
  mrb_value blk;
  unsigned long counted = 0;

  mrb_get_args(mrb, "&!", &blk);
  scoped_block(mrb, blk, FAULT_NONE, 0, &counted);
  return mrb_int_value(mrb, (mrb_int)counted);
}

/* AllocFault.fail_at(n) { ... }
   Refuse the n-th allocation of the block and every one after it. */
static mrb_value
f_fail_at(mrb_state *mrb, mrb_value self)
{
  mrb_value blk;
  mrb_int n;
  unsigned long counted = 0;

  mrb_get_args(mrb, "i&!", &n, &blk);
  if (n < 1) mrb_raise(mrb, E_ARGUMENT_ERROR, "allocation index starts at 1");
  return scoped_block(mrb, blk, FAULT_STICKY, (unsigned long)n, &counted);
}

/* AllocFault.fail_once(n) { ... }
   Refuse the n-th allocation of the block alone, so that the collection
   gc.c runs on a refusal has room to hand back. */
static mrb_value
f_fail_once(mrb_state *mrb, mrb_value self)
{
  mrb_value blk;
  mrb_int n;
  unsigned long counted = 0;

  mrb_get_args(mrb, "i&!", &n, &blk);
  if (n < 1) mrb_raise(mrb, E_ARGUMENT_ERROR, "allocation index starts at 1");
  return scoped_block(mrb, blk, FAULT_ONCE, (unsigned long)n, &counted);
}

/* AllocFault.armed? -> true/false
   True while the process-wide injection this program was started with is
   running, which is when the block forms above refuse to arm. */
static mrb_value
f_armed_p(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(fault_injecting());
}

static void
init_alloc_fault(mrb_state *mrb)
{
  struct RClass *m = mrb_define_module(mrb, "AllocFault");

  mrb_define_class_method(mrb, m, "count", f_count, MRB_ARGS_BLOCK());
  mrb_define_class_method(mrb, m, "fail_at", f_fail_at, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_class_method(mrb, m, "fail_once", f_fail_once, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_class_method(mrb, m, "armed?", f_armed_p, MRB_ARGS_NONE());
}

/* ------------------------------------------------------------------ */
/* scenarios                                                          */
/* ------------------------------------------------------------------ */

enum outcome {
  OUT_OK,        /* the scenario ran to its end */
  OUT_NOMEM,     /* it stopped at a NoMemoryError, which is what a refusal
                    is meant to raise */
  OUT_OPEN_FAIL, /* mrb_open() answered a state that cannot be used */
  OUT_EXCEPTION, /* something other than NoMemoryError came out */
  OUT_BROKEN     /* the state no longer works once the faults are off */
};

static const char*
outcome_name(enum outcome out)
{
  switch (out) {
  case OUT_OK:        return "ok";
  case OUT_NOMEM:     return "nomem";
  case OUT_OPEN_FAIL: return "open-failure";
  case OUT_EXCEPTION: return "exception";
  default:            return "broken";
  }
}

struct options {
  const char *code;      /* -e */
  const char *file;      /* -f */
  const char *cscenario; /* -c */
  enum fault_mode mode;
  unsigned long at;
  mrb_bool with_open;
  mrb_bool compile_in_scope;
  mrb_bool recheck;
};

static char exc_class[64];
static char exc_message[256];

static void
record_exception(mrb_state *mrb, mrb_value exc)
{
  mrb_value mesg;

  exc_class[0] = exc_message[0] = '\0';
  strncpy(exc_class, mrb_obj_classname(mrb, exc), sizeof(exc_class) - 1);
  exc_class[sizeof(exc_class) - 1] = '\0';

  /* Read the message off the object rather than asking it for one: the
     state has just been refused memory, and nothing here should run Ruby to
     find out what went wrong. */
  if (!mrb_exception_p(exc) || mrb_exc_ptr(exc)->mesg == NULL) return;
  mesg = mrb_obj_value(mrb_exc_ptr(exc)->mesg);
  if (mrb_string_p(mesg)) {
    size_t len = (size_t)RSTRING_LEN(mesg);
    if (len > sizeof(exc_message) - 1) len = sizeof(exc_message) - 1;
    memcpy(exc_message, RSTRING_PTR(mesg), len);
    exc_message[len] = '\0';
  }
}

static mrb_bool
is_nomemory(mrb_state *mrb, mrb_value exc)
{
  return mrb_obj_is_kind_of(mrb, exc, mrb_class_get(mrb, "NoMemoryError"));
}

static void
report(enum outcome out, unsigned long allocations, unsigned long refused)
{
  printf("allocations: %lu\n", allocations);
  printf("refusals: %lu\n", refused);
  printf("outcome: %s\n", outcome_name(out));
  if (exc_class[0]) printf("exception: %s\n", exc_class);
  if (exc_message[0]) printf("message: %s\n", exc_message);
  fflush(stdout);
}

/* An outcome the run is allowed to have: a scenario that completes, one that
   stops at a NoMemoryError, and -- only while a refusal is being injected --
   a state that failed to open.  Everything else is a finding. */
static int
exit_status(enum outcome out, const struct options *o)
{
  switch (out) {
  case OUT_OK:
  case OUT_NOMEM:
    return EXIT_SUCCESS;
  case OUT_OPEN_FAIL:
    return o->mode == FAULT_NONE ? EXIT_FAILURE : EXIT_SUCCESS;
  default:
    return EXIT_FAILURE;
  }
}

/* Ask the state to do a little of everything with the faults off.  A state
   that raised NoMemoryError is expected to carry on, so a failure here is a
   finding about what the unwinding left behind. */
static mrb_bool
state_still_works(mrb_state *mrb)
{
  static const char check[] =
    "s = 'x' * 100\n"
    "a = (1..50).map { |i| i.to_s }\n"
    "h = {}\n"
    "a.each { |v| h[v] = v.dup }\n"
    "s.size == 100 && h.size == 50 && a.last == '50'\n";
  mrb_value v;

  mrb->exc = NULL;
  mrb_full_gc(mrb);
  v = mrb_load_string(mrb, check);
  if (mrb->exc) {
    mrb->exc = NULL;
    return FALSE;
  }
  return mrb_test(v);
}

struct run_arg {
  const struct RProc *proc;
  const char *code;
  size_t len;
  mrb_bool compile;
};

static mrb_value
run_body(mrb_state *mrb, void *ud)
{
  struct run_arg *a = (struct run_arg*)ud;

  if (a->compile) return mrb_load_nstring(mrb, a->code, a->len);
  return mrb_top_run(mrb, a->proc, mrb_top_self(mrb), 0);
}

static int
run_ruby(const struct options *o, const char *code, size_t len)
{
  mrb_state *mrb;
  struct run_arg arg;
  mrb_value result;
  mrb_bool error = FALSE;
  enum outcome out;
  int ai;

  exc_class[0] = exc_message[0] = '\0';
  if (o->with_open) fault_arm(o->mode, o->at);
  mrb = mrb_open();
  if (MRB_OPEN_FAILURE(mrb)) {
    unsigned long counted = fault.count, refused = fault.refused;
    fault_disarm();
    if (mrb && mrb->exc) record_exception(mrb, mrb_obj_value(mrb->exc));
    mrb_close(mrb);
    report(OUT_OPEN_FAIL, counted, refused);
    return exit_status(OUT_OPEN_FAIL, o);
  }
  global_mrb = mrb;
  /* The module allocates as it is defined, so it is only there for a run
     whose region does not cover mrb_open(); a scenario that drives the
     faults itself is run without --with-open. */
  if (!o->with_open) init_alloc_fault(mrb);

  arg.proc = NULL;
  arg.code = code;
  arg.len = len;
  arg.compile = TRUE;

  ai = mrb_gc_arena_save(mrb);
  if (!o->with_open && !o->compile_in_scope) {
    /* Compile first, with the faults off, so that the numbering the fault
       index names follows the scenario's execution and not the parse of its
       source.  The proc stays in the arena, which is not restored until the
       run is over. */
    mrb_ccontext *cxt = mrb_ccontext_new(mrb);
    mrb_value pv;

    mrb_ccontext_filename(mrb, cxt, o->file ? o->file : "-e");
    cxt->no_exec = TRUE;
    pv = mrb_load_nstring_cxt(mrb, code, len, cxt);
    mrb_ccontext_free(mrb, cxt);
    if (mrb->exc || !mrb_proc_p(pv)) {
      if (mrb->exc) mrb_print_error(mrb);
      fprintf(stderr, "mruby-alloc-fault: the scenario does not compile\n");
      mrb_close(mrb);
      return 2;
    }
    arg.proc = mrb_proc_ptr(pv);
    MRB_PROC_SET_TARGET_CLASS((struct RProc*)arg.proc, mrb->object_class);
    arg.compile = FALSE;
  }

  if (!o->with_open) fault_arm(o->mode, o->at);
  result = mrb_protect_error(mrb, run_body, &arg, &error);
  {
    unsigned long counted = fault.count, refused = fault.refused;
    fault_disarm();

    if (error) {
      record_exception(mrb, result);
      out = is_nomemory(mrb, result) ? OUT_NOMEM : OUT_EXCEPTION;
    }
    else if (mrb->exc) {
      /* A compile that failed inside the armed region reports through
         mrb->exc rather than by raising. */
      mrb_value exc = mrb_obj_value(mrb->exc);
      record_exception(mrb, exc);
      out = is_nomemory(mrb, exc) ? OUT_NOMEM : OUT_EXCEPTION;
    }
    else {
      out = OUT_OK;
    }

    mrb->exc = NULL;
    mrb_gc_arena_restore(mrb, ai);
    if (o->recheck && out != OUT_EXCEPTION && !state_still_works(mrb)) {
      out = OUT_BROKEN;
    }
    mrb_close(mrb);
    report(out, counted, refused);
    return exit_status(out, o);
  }
}

/* The C scenarios: what a program does around mrb_open() and mrb_close(),
   which no Ruby scenario can reach because it is what runs the Ruby. */
static int
run_c_scenario(const struct options *o)
{
  mrb_state *mrb;
  unsigned long counted, refused;
  enum outcome out;
  mrb_bool core_only = (strcmp(o->cscenario, "open-core") == 0);

  if (!core_only && strcmp(o->cscenario, "open") != 0) {
    fprintf(stderr, "mruby-alloc-fault: no such C scenario: %s\n", o->cscenario);
    return 2;
  }

  exc_class[0] = exc_message[0] = '\0';
  fault_arm(o->mode, o->at);
  mrb = core_only ? mrb_open_core() : mrb_open();
  counted = fault.count;
  refused = fault.refused;
  fault_disarm();

  if (MRB_OPEN_FAILURE(mrb)) {
    if (mrb && mrb->exc) record_exception(mrb, mrb_obj_value(mrb->exc));
    out = OUT_OPEN_FAIL;
  }
  else {
    global_mrb = mrb;
    out = OUT_OK;
    if (o->recheck && !core_only && !state_still_works(mrb)) out = OUT_BROKEN;
  }

  /* The point of the scenario: a state that failed to open still has to be
     closed, and closing it has to give every allocation that did succeed
     back.  A leak or a double free here is what the sanitizer reports. */
  mrb_close(mrb);
  report(out, counted, refused);
  return exit_status(out, o);
}

/* ------------------------------------------------------------------ */
/* command line                                                       */
/* ------------------------------------------------------------------ */

static void
usage(void)
{
  fputs(
    "usage: mruby-alloc-fault [options] (-e CODE | -f FILE | -c SCENARIO)\n"
    "\n"
    "  -e CODE             run CODE as the scenario\n"
    "  -f FILE             run the contents of FILE as the scenario\n"
    "  -c SCENARIO         run a C scenario: open, open-core\n"
    "\n"
    "  --count             count the scenario's allocations, refuse none\n"
    "  --fail-at N         refuse the N-th allocation and every one after it\n"
    "  --fail-once N       refuse the N-th allocation alone\n"
    "  --with-open         put mrb_open() inside the counted/refused region\n"
    "  --compile-in-scope  compile the scenario inside the region too\n"
    "  --no-recheck        skip the check that the state still works after\n"
    "  -h, --help          this text\n"
    "\n"
    "Reports `allocations:`, `refusals:`, `outcome:` and, where there is one,\n"
    "`exception:` on stdout.  Exits 0 for an accepted outcome, 1 for a\n"
    "finding, 2 for a usage error.\n",
    stderr);
}

static mrb_bool
parse_index(const char *s, unsigned long *out)
{
  char *end;
  unsigned long v;

  if (!s || !*s) return FALSE;
  v = strtoul(s, &end, 10);
  if (*end != '\0' || v < 1) return FALSE;
  *out = v;
  return TRUE;
}

static char*
read_file(const char *path, size_t *len)
{
  FILE *fp = fopen(path, "rb");
  char *buf;
  size_t size, got;

  if (!fp) return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
  size = (size_t)ftell(fp);
  rewind(fp);
  buf = (char*)malloc(size + 1);
  if (!buf) { fclose(fp); return NULL; }
  got = fread(buf, 1, size, fp);
  fclose(fp);
  buf[got] = '\0';
  *len = got;
  return buf;
}

int
main(int argc, char **argv)
{
  struct options o;
  char *owned = NULL;
  const char *code = NULL;
  size_t len = 0;
  int status, i;

  memset(&o, 0, sizeof(o));
  o.mode = FAULT_NONE;
  o.recheck = TRUE;

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];

    if (strcmp(a, "-e") == 0 && i + 1 < argc) o.code = argv[++i];
    else if (strcmp(a, "-f") == 0 && i + 1 < argc) o.file = argv[++i];
    else if (strcmp(a, "-c") == 0 && i + 1 < argc) o.cscenario = argv[++i];
    else if (strcmp(a, "--count") == 0) o.mode = FAULT_NONE;
    else if (strcmp(a, "--fail-at") == 0 && i + 1 < argc) {
      if (!parse_index(argv[++i], &o.at)) { usage(); return 2; }
      o.mode = FAULT_STICKY;
    }
    else if (strcmp(a, "--fail-once") == 0 && i + 1 < argc) {
      if (!parse_index(argv[++i], &o.at)) { usage(); return 2; }
      o.mode = FAULT_ONCE;
    }
    else if (strcmp(a, "--with-open") == 0) o.with_open = TRUE;
    else if (strcmp(a, "--compile-in-scope") == 0) o.compile_in_scope = TRUE;
    else if (strcmp(a, "--no-recheck") == 0) o.recheck = FALSE;
    else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
    else { fprintf(stderr, "mruby-alloc-fault: unknown option: %s\n", a); usage(); return 2; }
  }

  if ((o.code ? 1 : 0) + (o.file ? 1 : 0) + (o.cscenario ? 1 : 0) != 1) {
    usage();
    return 2;
  }

  if (o.cscenario) return run_c_scenario(&o);

  if (o.file) {
    owned = read_file(o.file, &len);
    if (!owned) {
      fprintf(stderr, "mruby-alloc-fault: cannot read %s\n", o.file);
      return 2;
    }
    code = owned;
  }
  else {
    code = o.code;
    len = strlen(code);
  }

  status = run_ruby(&o, code, len);
  free(owned);
  return status;
}
