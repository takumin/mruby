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

void
mrb_mruby_os_memsize_gem_test(mrb_state *mrb)
{
  struct RClass *os = mrb_module_get(mrb, "ObjectSpace");
  mrb_define_module_function(mrb, os, "__memsize_slot", memsize_slot, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_value_width", memsize_value_width, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_heap_number", memsize_heap_number, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_nofree_string", memsize_nofree_string, MRB_ARGS_NONE());
}
