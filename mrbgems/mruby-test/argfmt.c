/*
 * Methods that exercise the `mrb_get_args` format itself: whether a specifier
 * takes an implicit conversion is stated by the `~` modifier, so a test needs
 * both a marked and an unmarked form of the same specifier, and both the fast
 * and the general path through the parser.
 */

#include <mruby.h>
#include <mruby/class.h>

/* marked: takes the conversion, fast path */
static mrb_value
af_conv_str(mrb_state *mrb, mrb_value klass)
{
  mrb_value v;
  mrb_get_args(mrb, "S~", &v);
  return v;
}

static mrb_value
af_conv_ary(mrb_state *mrb, mrb_value klass)
{
  mrb_value v;
  mrb_get_args(mrb, "A~", &v);
  return v;
}

static mrb_value
af_conv_hash(mrb_state *mrb, mrb_value klass)
{
  mrb_value v;
  mrb_get_args(mrb, "H~", &v);
  return v;
}

/* unmarked: the specifier means what it meant before the modifier existed */
static mrb_value
af_strict_str(mrb_state *mrb, mrb_value klass)
{
  mrb_value v;
  mrb_get_args(mrb, "S", &v);
  return v;
}

/* marked and optional: `~` is a modifier, so it must not count as an argument */
static mrb_value
af_conv_opt(mrb_state *mrb, mrb_value klass)
{
  mrb_value v = mrb_nil_value();
  mrb_get_args(mrb, "|S~", &v);
  return v;
}

/* marked with `!`: the general path, where a nil argument skips the conversion */
static mrb_value
af_conv_alt(mrb_state *mrb, mrb_value klass)
{
  mrb_value v;
  mrb_get_args(mrb, "S~!", &v);
  return v;
}

/* marked, but not the last argument: the conversion cannot be arranged there */
static mrb_value
af_conv_first(mrb_state *mrb, mrb_value klass)
{
  mrb_value v, w;
  mrb_get_args(mrb, "S~o", &v, &w);
  return v;
}

/* `~` on a specifier that names no type to convert to */
static mrb_value
af_bad_modifier(mrb_state *mrb, mrb_value klass)
{
  mrb_int n;
  mrb_get_args(mrb, "i~", &n);
  return mrb_fixnum_value(n);
}

void
mrb_init_test_argfmt(mrb_state *mrb)
{
  struct RClass *af = mrb_define_module(mrb, "TestArgFormat");

  mrb_define_class_method(mrb, af, "conv_str", af_conv_str, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_ary", af_conv_ary, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_hash", af_conv_hash, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "strict_str", af_strict_str, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_opt", af_conv_opt, MRB_ARGS_OPT(1));
  mrb_define_class_method(mrb, af, "conv_alt", af_conv_alt, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_first", af_conv_first, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, af, "bad_modifier", af_bad_modifier, MRB_ARGS_REQ(1));
}
