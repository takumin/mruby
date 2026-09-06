#include <string.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/error.h>
#include <mruby/gc.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/value.h>
#include <mruby/debug.h>
#include <mruby/internal.h>

/* What the tests of ObjectSpace.memsize_of ask about but cannot reach from
 * Ruby: the size of an object slot and of an mrb_value, and objects that only
 * C builds. Each helper answers in bytes or hands out the object itself. */

static mrb_value
memsize_slot(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, (mrb_int)mrb_objspace_page_slot_size());
}

static mrb_value
memsize_value_width(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, (mrb_int)sizeof(mrb_value));
}

static void
push_number(mrb_state *mrb, mrb_value ary, mrb_value v)
{
  mrb_value pair = mrb_ary_new_capa(mrb, 2);

  mrb_ary_push(mrb, pair, v);
  mrb_ary_push(mrb, pair, mrb_bool_value(mrb_immediate_p(v)));
  mrb_ary_push(mrb, ary, pair);
}

/* An Integer and a Float the boxed word cannot hold, each paired with whether
 * the boxing kept it immediate after all: MRB_NO_BOXING has room for both,
 * and word boxing for a Float. The Float pair only where there is one. */
static mrb_value
memsize_heap_number(mrb_state *mrb, mrb_value self)
{
  mrb_value ary = mrb_ary_new_capa(mrb, 2);

  push_number(mrb, ary, mrb_int_value(mrb, MRB_INT_MAX));
#ifndef MRB_NO_FLOAT
  push_number(mrb, ary, mrb_float_value(mrb, 1.5));
#endif
  return ary;
}

/* A string over static storage, as OP_STRING builds from a pool string a
 * static irep holds; long enough not to embed in the object header. */
static mrb_value
memsize_nofree_string(mrb_state *mrb, mrb_value self)
{
  static const char body[] = "a body long enough to sit outside the object header";
  return mrb_str_new_static(mrb, body, sizeof(body) - 1);
}

static mrb_value
raise_for_backtrace(mrb_state *mrb, void *data)
{
  mrb_raise(mrb, E_RUNTIME_ERROR, "memsize");
  return mrb_nil_value();
}

/* The packed backtrace an exception keeps from being raised, with how many
 * frames it holds and what one frame takes. The object is internal
 * (MRB_TT_BACKTRACE); memsize_of is the only thing to ask it. */
static mrb_value
memsize_backtrace(mrb_state *mrb, mrb_value self)
{
  mrb_bool error;
  mrb_value exc = mrb_protect_error(mrb, raise_for_backtrace, NULL, &error);
  struct RBacktrace *bt;
  mrb_value ary;

  mrb_gc_protect(mrb, exc); /* the backtrace is reachable through it alone */
  bt = (struct RBacktrace*)mrb_exc_ptr(exc)->backtrace;
  ary = mrb_ary_new_capa(mrb, 3);

  mrb_assert(error && bt && bt->tt == MRB_TT_BACKTRACE);
  mrb_ary_push(mrb, ary, mrb_obj_value(bt));
  mrb_ary_push(mrb, ary, mrb_int_value(mrb, (mrb_int)bt->len));
  mrb_ary_push(mrb, ary, mrb_int_value(mrb, (mrb_int)sizeof(struct mrb_backtrace_location)));
  return ary;
}

static void
push_irep_case(mrb_state *mrb, mrb_value ary, const char *name, mrb_irep *irep, size_t owned)
{
  int ai = mrb_gc_arena_save(mrb);
  mrb_value row = mrb_ary_new_capa(mrb, 3);

  mrb_ary_push(mrb, row, mrb_str_new_cstr(mrb, name));
  mrb_ary_push(mrb, row, mrb_obj_value(mrb_proc_new(mrb, irep)));
  mrb_ary_push(mrb, row, mrb_int_value(mrb, (mrb_int)owned));
  mrb_ary_push(mrb, ary, row);
  mrb_irep_decref(mrb, irep); /* the proc holds it now */
  mrb_gc_arena_restore(mrb, ai);
}

/* Procs over ireps built by hand, each with the bytes its irep owns: what
 * mrb_irep_free() would release, at the size it was allocated. None of them
 * is ever run. */
static mrb_value
memsize_ireps(mrb_state *mrb, mrb_value self)
{
  static const mrb_code static_iseq[4] = { 0, 0, 0, 0 };
  static const char static_str[] = "a pool string in read-only data";
  static mrb_irep static_irep;
  mrb_value ary = mrb_ary_new(mrb);
  mrb_irep *irep;
  mrb_irep_pool *pool;
  size_t owned;

  /* a bigint entry: a length byte with its top bit set, a sign byte, digits */
  irep = mrb_add_irep(mrb);
  pool = (mrb_irep_pool*)mrb_malloc(mrb, sizeof(mrb_irep_pool));
  {
    const size_t digits = 200;
    char *p = (char*)mrb_malloc(mrb, digits + 2);
    p[0] = (char)digits;
    p[1] = 0;
    memset(p + 2, 1, digits);
    pool[0].tt = IREP_TT_BIGINT;
    pool[0].u.str = p;
    owned = sizeof(mrb_irep) + sizeof(mrb_irep_pool) + digits + 2;
  }
  irep->pool = pool;
  irep->plen = 1;
  push_irep_case(mrb, ary, "bigint pool entry", irep, owned);

  /* a heap pool string is copied with its NUL */
  irep = mrb_add_irep(mrb);
  pool = (mrb_irep_pool*)mrb_malloc(mrb, sizeof(mrb_irep_pool));
  {
    const size_t len = sizeof(static_str) - 1;
    char *p = (char*)mrb_malloc(mrb, len + 1);
    memcpy(p, static_str, len + 1);
    pool[0].tt = (uint32_t)(len << 2) | IREP_TT_STR;
    pool[0].u.str = p;
    owned = sizeof(mrb_irep) + sizeof(mrb_irep_pool) + len + 1;
  }
  irep->pool = pool;
  irep->plen = 1;
  push_irep_case(mrb, ary, "heap pool string", irep, owned);

  /* a static pool string stays where the binary put it */
  irep = mrb_add_irep(mrb);
  pool = (mrb_irep_pool*)mrb_malloc(mrb, sizeof(mrb_irep_pool));
  pool[0].tt = (uint32_t)((sizeof(static_str) - 1) << 2) | IREP_TT_SSTR;
  pool[0].u.str = static_str;
  irep->pool = pool;
  irep->plen = 1;
  push_irep_case(mrb, ary, "static pool string", irep, sizeof(mrb_irep) + sizeof(mrb_irep_pool));

  /* the catch handler table shares the iseq's block */
  irep = mrb_add_irep(mrb);
  {
    const size_t len = 8 * sizeof(mrb_code) + 2 * sizeof(struct mrb_irep_catch_handler);
    irep->iseq = (const mrb_code*)mrb_calloc(mrb, 1, len);
    irep->ilen = 8;
    irep->clen = 2;
    owned = sizeof(mrb_irep) + len;
  }
  push_irep_case(mrb, ary, "iseq and catch handlers", irep, owned);

  /* a static iseq, as a binary in read-only data is read */
  irep = mrb_add_irep(mrb);
  irep->iseq = static_iseq;
  irep->ilen = sizeof(static_iseq);
  irep->flags |= MRB_ISEQ_NO_FREE;
  push_irep_case(mrb, ary, "static iseq", irep, sizeof(mrb_irep));

  /* local variable names, one per local past self */
  irep = mrb_add_irep(mrb);
  irep->nlocals = 3;
  irep->lv = (const mrb_sym*)mrb_calloc(mrb, 2, sizeof(mrb_sym));
  push_irep_case(mrb, ary, "local variable names", irep, sizeof(mrb_irep) + 2 * sizeof(mrb_sym));

  /* debug info: the info, its file table, one file with a packed line map */
  irep = mrb_add_irep(mrb);
  {
    const size_t packed = 5;
    mrb_irep_debug_info *d = (mrb_irep_debug_info*)mrb_calloc(mrb, 1, sizeof(*d));
    mrb_irep_debug_info_file *f = (mrb_irep_debug_info_file*)mrb_calloc(mrb, 1, sizeof(*f));

    f->line_type = mrb_debug_line_packed_map;
    f->line_entry_count = (uint32_t)packed;
    f->lines.ptr = mrb_calloc(mrb, 1, packed);
    d->flen = 1;
    d->files = (mrb_irep_debug_info_file**)mrb_calloc(mrb, 1, sizeof(*d->files));
    d->files[0] = f;
    irep->debug_info = d;
    owned = sizeof(mrb_irep) + sizeof(*d) + sizeof(*d->files) + sizeof(*f) + packed;
  }
  push_irep_case(mrb, ary, "debug info", irep, owned);

  /* a static irep: everything it points to is read-only data */
  static_irep.flags = MRB_IREP_NO_FREE;
  static_irep.refcnt = 1;
  push_irep_case(mrb, ary, "static irep", &static_irep, 0);

  return ary;
}

void
mrb_mruby_os_memsize_gem_test(mrb_state *mrb)
{
  struct RClass *os = mrb_module_get(mrb, "ObjectSpace");
  mrb_define_module_function(mrb, os, "__memsize_slot", memsize_slot, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_value_width", memsize_value_width, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_heap_number", memsize_heap_number, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_nofree_string", memsize_nofree_string, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_backtrace", memsize_backtrace, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_ireps", memsize_ireps, MRB_ARGS_NONE());
}
