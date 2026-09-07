#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/internal.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <stdio.h>
#include <stdlib.h>

static mrb_value
fiber_s_yield_by_c_func(mrb_state *mrb, mrb_value self)
{
  mrb_value a = mrb_get_arg1(mrb);
  return mrb_fiber_yield(mrb, 1, &a);
}

static mrb_value
fiber_s_yield_by_c_method(mrb_state *mrb, mrb_value self)
{
  mrb_value a = mrb_get_arg1(mrb);
  return mrb_funcall_argv(mrb, self, mrb_intern_lit(mrb, "yield"), 1, &a);
}

static mrb_value
fiber_resume_by_c_func(mrb_state *mrb, mrb_value self)
{
  ptrdiff_t ci_index = mrb->c->ci - mrb->c->cibase;
  mrb_value ret = mrb_fiber_resume(mrb, self, 0, NULL);
  if (ci_index != mrb->c->ci - mrb->c->cibase) {
    mrb_raisef(mrb, E_EXCEPTION,
               "[BUG] INVALID CI POSITION (expected %d, but actual %d) [BUG]",
               (int)ci_index, (int)(mrb->c->ci - mrb->c->cibase));
  }
  return ret;
}

static mrb_value
fiber_resume_by_c_method(mrb_state *mrb, mrb_value self)
{
  ptrdiff_t ci_index = mrb->c->ci - mrb->c->cibase;
  mrb_value ret = mrb_funcall_argv(mrb, self, mrb_intern_lit(mrb, "resume"), 0, NULL);
  if (ci_index != mrb->c->ci - mrb->c->cibase) {
    mrb_raisef(mrb, E_EXCEPTION,
               "[BUG] INVALID CI POSITION (expected %d, but actual %d) [BUG]",
               (int)ci_index, (int)(mrb->c->ci - mrb->c->cibase));
  }
  return ret;
}

static mrb_value
fiber_transfer_by_c(mrb_state *mrb, mrb_value self)
{
  return mrb_funcall_argv(mrb, self, mrb_intern_lit(mrb, "transfer"), 0, NULL);
}

/*
 * `recv.send_k(target, mid)` sends `mid` to `target` by handing this frame
 * back with mrb_funcall_k(), and answers what the send answered.  It gives a
 * test a C method of its own to redefine while the send is running, which the
 * core methods that hand their frame back cannot be without losing them for
 * the rest of the run.
 */
static mrb_value
cont_send_k(mrb_state *mrb, mrb_value self)
{
  mrb_value result;
  if (mrb_funcall_k_resumed(mrb, &result, NULL)) return result;

  mrb_value target;
  mrb_sym mid;
  mrb_get_args(mrb, "on", &target, &mid);
  return mrb_funcall_k(mrb, target, mid, 0, NULL, mrb_nil_value());
}

/*
 * What a test redefines `send_k` to while it is suspended.  The frame is the
 * one `send_k` handed back, so this reads the answer of the send out of it.
 */
static mrb_value
cont_resume_k(mrb_state *mrb, mrb_value self)
{
  mrb_value result;
  if (!mrb_funcall_k_resumed(mrb, &result, NULL)) {
    return mrb_symbol_value(mrb_intern_lit(mrb, "fresh"));
  }
  return mrb_assoc_new(mrb, mrb_symbol_value(mrb_intern_lit(mrb, "taken_over")), result);
}

/*
 * Puts both of the above on `mod`, so a test that redefines one of them has a
 * class of its own to do it in and leaves the others alone.
 */
static mrb_value
fiber_s_define_cont_methods(mrb_state *mrb, mrb_value self)
{
  struct RClass *c = mrb_class_ptr(mrb_get_arg1(mrb));
  mrb_define_method(mrb, c, "send_k", cont_send_k, MRB_ARGS_ANY());
  mrb_define_method(mrb, c, "resume_k", cont_resume_k, MRB_ARGS_ANY());
  return mrb_nil_value();
}

static mrb_value
proc_s_c_tunnel(mrb_state *mrb, mrb_value self)
{
  mrb_value b;
  mrb_get_args(mrb, "&!", &b);
  return mrb_yield_argv(mrb, b, 0, NULL);
}

static void
check_activity(mrb_state *mrb)
{
  mrb_value act = mrb_gv_get(mrb, mrb_intern_lit(mrb, "$fiber_test_activity"));
  if (mrb_test(act)) {
    act = mrb_obj_as_string(mrb, act);
    fprintf(stderr, "\n\t<<<%s%.*s>>>\n",
            "mruby VM has an unexpected outage in ", (int)RSTRING_LEN(act), RSTRING_PTR(act));
    abort();
  }
}

void
mrb_mruby_fiber_gem_test(mrb_state *mrb)
{
  struct RClass *fiber_class = mrb_class_get(mrb, "Fiber");
  mrb_define_class_method(mrb, fiber_class, "yield_by_c_func", fiber_s_yield_by_c_func, MRB_ARGS_ANY());
  mrb_define_class_method(mrb, fiber_class, "yield_by_c_method", fiber_s_yield_by_c_method, MRB_ARGS_ANY());
  mrb_define_method(mrb, fiber_class, "resume_by_c_func", fiber_resume_by_c_func, MRB_ARGS_NONE());
  mrb_define_method(mrb, fiber_class, "resume_by_c_method", fiber_resume_by_c_method, MRB_ARGS_NONE());
  mrb_define_method(mrb, fiber_class, "transfer_by_c", fiber_transfer_by_c, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, fiber_class, "define_cont_methods", fiber_s_define_cont_methods, MRB_ARGS_REQ(1));

  mrb_define_class_method(mrb, mrb->proc_class, "c_tunnel", proc_s_c_tunnel, MRB_ARGS_NONE() | MRB_ARGS_BLOCK());

  mrb_gv_set(mrb, mrb_intern_lit(mrb, "$fiber_test_activity"), mrb_nil_value());
  mrb_state_atexit(mrb, check_activity);
}
