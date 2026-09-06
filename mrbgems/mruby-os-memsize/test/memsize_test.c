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

void
mrb_mruby_os_memsize_gem_test(mrb_state *mrb)
{
  struct RClass *os = mrb_module_get(mrb, "ObjectSpace");
  mrb_define_module_function(mrb, os, "__memsize_slot", memsize_slot, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, os, "__memsize_value_width", memsize_value_width, MRB_ARGS_NONE());
}
