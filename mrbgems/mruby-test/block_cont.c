/*
** block_cont.c - what a C method's block call through the VM does
**
** mrb_block_cont() is mrb_funcall_cont() for a block rather than for a
** method a C method sends: the call goes to the VM and the C method is
** resumed with its result, so the walk around it never sits on the C stack.
** A method converted with it looks like BlockCont.detect below, which is
** Array#index's shape with nothing else in it.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>

static mrb_value block_cont_resume(mrb_state *mrb, mrb_value result, mrb_int i);

/* The walk, resumable from any index. The array and the block are read from
   the frame rather than held in C locals: the walk returns to the VM on every
   element, and only the frame survives that. */
static mrb_value
block_cont_walk(mrb_state *mrb, mrb_int i)
{
  mrb_value ary = mrb->c->ci->stack[1];

  /* The block may have changed the array's length, so it is read afresh, and
     so is the frame: a call that was not handed over ran a VM of its own. */
  for (; i < RARRAY_LEN(ary); i++) {
    mrb_value v = RARRAY_PTR(ary)[i], r;

    if (mrb_block_cont_p(mrb, &r, block_cont_resume, i, mrb->c->ci->stack[2], 1, &v)) {
      return r;
    }
    if (mrb_test(r)) return v;
  }
  return mrb_nil_value();
}

static mrb_value
block_cont_resume(mrb_state *mrb, mrb_value result, mrb_int i)
{
  if (mrb_test(result)) {
    mrb_value ary = mrb->c->ci->stack[1];
    if (i < RARRAY_LEN(ary)) return RARRAY_PTR(ary)[i];
    return mrb_nil_value();
  }
  return block_cont_walk(mrb, i + 1);
}

/* BlockCont.detect(ary) { |x| ... } -> the first element the block accepts */
static mrb_value
block_cont_detect(mrb_state *mrb, mrb_value self)
{
  mrb_value ary, blk;

  mrb_get_args(mrb, "A&", &ary, &blk);
  if (mrb_nil_p(blk)) mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  return block_cont_walk(mrb, 0);
}

/* BlockCont.detect_nested(ary) { |x| ... } -> the same, the old way
 *
 * mrb_yield() on every element, so a test can say the two answer alike
 * rather than only that the new one answers something.
 */
static mrb_value
block_cont_detect_nested(mrb_state *mrb, mrb_value self)
{
  mrb_value ary, blk;

  mrb_get_args(mrb, "A&", &ary, &blk);
  if (mrb_nil_p(blk)) mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  for (mrb_int i = 0; i < RARRAY_LEN(ary); i++) {
    mrb_value v = RARRAY_PTR(ary)[i];
    if (mrb_test(mrb_yield(mrb, blk, v))) return v;
  }
  return mrb_nil_value();
}

/* BlockCont.apply(ary) { |*args| ... } -> what the block answers
 *
 * One call, taking the array's elements as its arguments. A count of 15 or
 * more is the one mrb_block_cont() cannot lay out register by register and
 * packs into an array instead, and no walk reaches that path.
 */
static mrb_value
block_cont_apply_resume(mrb_state *mrb, mrb_value result, mrb_int state)
{
  return result;
}

static mrb_value
block_cont_apply(mrb_state *mrb, mrb_value self)
{
  mrb_value ary, blk;

  mrb_get_args(mrb, "A&", &ary, &blk);
  if (mrb_nil_p(blk)) mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  return mrb_block_cont(mrb, block_cont_apply_resume, 0, blk,
                        RARRAY_LEN(ary), RARRAY_PTR(ary));
}

/* BlockCont.apply_nested(ary) { |*args| ... } -> the same, the old way */
static mrb_value
block_cont_apply_nested(mrb_state *mrb, mrb_value self)
{
  mrb_value ary, blk;

  mrb_get_args(mrb, "A&", &ary, &blk);
  if (mrb_nil_p(blk)) mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  return mrb_yield_argv(mrb, blk, RARRAY_LEN(ary), RARRAY_PTR(ary));
}

/* BlockCont.under(mod) { ... } -> mod
 *
 * The block runs as a class body: self and the class a `def` in it lands on
 * are both `mod`, and the answer is `mod` rather than the block's value,
 * which is the shape Class.new and Module.new have.
 */
static mrb_value
block_cont_under_resume(mrb_state *mrb, mrb_value result, mrb_int state)
{
  /* The module is read from the register it came in on: a C local does not
     survive the return to the VM. */
  return mrb->c->ci->stack[1];
}

static mrb_value
block_cont_under(mrb_state *mrb, mrb_value self)
{
  mrb_value mod, blk;

  mrb_get_args(mrb, "C&", &mod, &blk);
  if (mrb_nil_p(blk)) mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  return mrb_block_cont_under(mrb, block_cont_under_resume, 0, blk, 1, &mod,
                              mod, mrb_class_ptr(mod));
}

/* BlockCont.under_nested(mod) { ... } -> mod, the same the old way */
static mrb_value
block_cont_under_nested(mrb_state *mrb, mrb_value self)
{
  mrb_value mod, blk;

  mrb_get_args(mrb, "C&", &mod, &blk);
  if (mrb_nil_p(blk)) mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  mrb_yield_with_class(mrb, blk, 1, &mod, mod, mrb_class_ptr(mod));
  return mod;
}

/* BlockCont.from_c(ary) { ... } -> what BlockCont.detect answers
 *
 * The same walk, reached through mrb_funcall_with_block() rather than from
 * bytecode. The frame is one a C caller is waiting on, so not one call the
 * walk makes can be handed to the VM: every element takes the fallback, which
 * is the path that must not cost a C frame per element.
 */
static mrb_value
block_cont_from_c(mrb_state *mrb, mrb_value self)
{
  mrb_value ary, blk;

  mrb_get_args(mrb, "A&", &ary, &blk);
  return mrb_funcall_with_block(mrb, self, mrb_intern_lit(mrb, "detect"), 1, &ary, blk);
}

/* BlockCont.depth -> the number of frames below this one */
static mrb_value
block_cont_depth(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, (mrb_int)(mrb->c->ci - mrb->c->cibase));
}

void
mrb_init_test_block_cont(mrb_state *mrb)
{
  struct RClass *c = mrb_define_module(mrb, "BlockCont");

  mrb_define_module_function(mrb, c, "detect", block_cont_detect, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_module_function(mrb, c, "detect_nested", block_cont_detect_nested, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_module_function(mrb, c, "apply", block_cont_apply, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_module_function(mrb, c, "apply_nested", block_cont_apply_nested, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_module_function(mrb, c, "under", block_cont_under, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_module_function(mrb, c, "under_nested", block_cont_under_nested, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_module_function(mrb, c, "from_c", block_cont_from_c, MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK());
  mrb_define_module_function(mrb, c, "depth", block_cont_depth, MRB_ARGS_NONE());
}
