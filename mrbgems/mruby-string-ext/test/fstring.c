/*
** fstring.c - what a test cannot see from Ruby about the shared strings
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/string.h>
#include <mruby/gc.h>

/*
 * Whether the string stands in the program's own read-only data, which is
 * where `mrbc` writes the literals of the code it dumps as C. Nothing about
 * a string says so in Ruby -- it is frozen and it is a String either way --
 * so the test asks here.
 */
static mrb_value
str_readonly_p(mrb_state *mrb, mrb_value self)
{
  mrb_value str;

  mrb_get_args(mrb, "S", &str);
  return mrb_bool_value(mrb_str_ptr(str)->gc_color == MRB_GC_RED);
}

/* A string of the read-only data written by hand, so that a test has one to
   ask about wherever it runs: the tests themselves are loaded as a binary,
   whose literals are made in the heap as they are read. The bytes are not
   ASCII, since what reading them finds cannot be written back to a string
   standing here. */
static const char ro_bytes[] = "\xe3\x81\x82 read-only";
static const struct RString ro_str =
  MRB_ROM_STRING(ro_bytes, sizeof(ro_bytes) - 1, MRB_STR_CODERANGE_UNKNOWN);

static mrb_value
str_readonly_sample(mrb_state *mrb, mrb_value self)
{
  return mrb_obj_value((void*)&ro_str);
}

void
mrb_mruby_string_ext_gem_test(mrb_state *mrb)
{
  /* Named rather than presymbolized: the presym table is built from the
     library's own sources, which a gem's tests are not among. */
  mrb_define_module_function(mrb, mrb->kernel_module, "__fstr_readonly?",
                             str_readonly_p, MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb->kernel_module, "__fstr_readonly_sample",
                             str_readonly_sample, MRB_ARGS_NONE());
}
