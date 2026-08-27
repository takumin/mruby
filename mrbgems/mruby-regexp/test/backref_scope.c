#include <mruby.h>
#include <mruby/compile.h>

/* Runs a script through mrb_load_string() while the VM is mid-execution,
   the way an embedding host does from a C function it defines: mrb_top_run()
   pushes a fresh frame whose proc captured no scope, the frame that is
   transparent to `$~` owner resolution in backref_owner() (see vm.c). The
   Ruby-visible shape is pinned by test/backref_scope.rb. */
static mrb_value
backref_nested_load(mrb_state *mrb, mrb_value self)
{
  const char *s;
  mrb_get_args(mrb, "z", &s);
  return mrb_load_string(mrb, s);
}

void
mrb_mruby_regexp_gem_test(mrb_state *mrb)
{
  mrb_define_method(mrb, mrb->object_class, "__backref_nested_load", backref_nested_load, MRB_ARGS_REQ(1));
}
