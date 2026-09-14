/*
** funcall_tail.c - what a C method's tail call to a Ruby method does
**
** mrb_funcall_tail() hands the frame the VM dispatched a C method on to the
** method that C method wanted to call, instead of running the call on a
** nested mrb_vm_exec().  Nothing in the tree calls it yet: the conversions
** that will are their own stages, and an API left without a caller is how
** mrb_yield_cont() came to have a crash sitting in it.  The methods below
** are the caller until then.
*/

#include <mruby.h>
#include <mruby/class.h>

static void
tail_args(mrb_state *mrb, mrb_value *recv, mrb_sym *mid, const mrb_value **argv, mrb_int *argc)
{
  mrb_get_args(mrb, "on*", recv, mid, argv, argc);
}

/* FuncallTail.tail(recv, mid, *args) -> what the method answered
 *
 * A C method whose whole body is the call, which is the shape the API asks
 * for.
 */
static mrb_value
funcall_tail_tail(mrb_state *mrb, mrb_value self)
{
  mrb_value recv;
  mrb_sym mid;
  const mrb_value *argv;
  mrb_int argc;

  tail_args(mrb, &recv, &mid, &argv, &argc);
  return mrb_funcall_tail(mrb, recv, mid, argc, argv);
}

/* FuncallTail.nested(recv, mid, *args) -> what the method answered
 *
 * The same call the old way, so a test can say that the two answer alike
 * rather than only that the new one answers something.
 */
static mrb_value
funcall_tail_nested(mrb_state *mrb, mrb_value self)
{
  mrb_value recv;
  mrb_sym mid;
  const mrb_value *argv;
  mrb_int argc;

  tail_args(mrb, &recv, &mid, &argv, &argc);
  return mrb_funcall_argv(mrb, recv, mid, argc, argv);
}

/* FuncallTail.from_c(recv, mid, *args) -> what the method answered
 *
 * Reaches the tail call through mrb_funcall(), so the frame it would replace
 * is one this method's C caller is waiting for.  Replacing it would drop
 * that caller's return, and the fallback is what keeps the answer right.
 */
static mrb_value
funcall_tail_from_c(mrb_state *mrb, mrb_value self)
{
  mrb_value recv;
  mrb_sym mid;
  const mrb_value *argv;
  mrb_int argc;
  mrb_value args[16];

  tail_args(mrb, &recv, &mid, &argv, &argc);
  if (argc > 14) argc = 14;
  args[0] = recv;
  args[1] = mrb_symbol_value(mid);
  for (mrb_int i = 0; i < argc; i++) {
    args[i+2] = argv[i];
  }
  return mrb_funcall_argv(mrb, self, mrb_intern_lit(mrb, "tail"), argc+2, args);
}

/* FuncallTail.depth -> the number of frames below this one
 *
 * A tail call leaves the caller where it found it.  Reading this on either
 * side of one says so, which is the invariant fibertest.c asserts for a
 * fiber switch.
 */
static mrb_value
funcall_tail_depth(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, (mrb_int)(mrb->c->ci - mrb->c->cibase));
}

void
mrb_init_test_funcall_tail(mrb_state *mrb)
{
  struct RClass *c = mrb_define_module(mrb, "FuncallTail");

  mrb_define_module_function(mrb, c, "tail", funcall_tail_tail, MRB_ARGS_REQ(2)|MRB_ARGS_REST());
  mrb_define_module_function(mrb, c, "nested", funcall_tail_nested, MRB_ARGS_REQ(2)|MRB_ARGS_REST());
  mrb_define_module_function(mrb, c, "from_c", funcall_tail_from_c, MRB_ARGS_REQ(2)|MRB_ARGS_REST());
  mrb_define_module_function(mrb, c, "depth", funcall_tail_depth, MRB_ARGS_NONE());
}
