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

/* marked, on the specifiers that hand out a pointer rather than the value:
   the conversion has to happen before the pointer is taken */
static mrb_value
af_conv_ptr(mrb_state *mrb, mrb_value klass)
{
  const char *p;
  mrb_int len;
  mrb_get_args(mrb, "s~", &p, &len);
  return mrb_str_new(mrb, p, len);
}

static mrb_value
af_conv_cstr(mrb_state *mrb, mrb_value klass)
{
  const char *p;
  mrb_get_args(mrb, "z~", &p);
  return mrb_str_new_cstr(mrb, p);
}

static mrb_value
af_conv_alist(mrb_state *mrb, mrb_value klass)
{
  const mrb_value *p;
  mrb_int len;
  mrb_get_args(mrb, "a~", &p, &len);
  return len > 0 ? p[0] : mrb_nil_value();
}

/* unmarked: the pointer specifiers demand their type as they always did */
static mrb_value
af_strict_ptr(mrb_state *mrb, mrb_value klass)
{
  const char *p;
  mrb_int len;
  mrb_get_args(mrb, "s", &p, &len);
  return mrb_str_new(mrb, p, len);
}

/* marked with `!`, so the general path: a nil argument skips the conversion
   and gives the empty pointer */
static mrb_value
af_conv_ptr_alt(mrb_state *mrb, mrb_value klass)
{
  const char *p;
  mrb_int len;
  mrb_get_args(mrb, "s~!", &p, &len);
  return p ? mrb_str_new(mrb, p, len) : mrb_nil_value();
}

static mrb_value
af_conv_cstr_alt(mrb_state *mrb, mrb_value klass)
{
  const char *p;
  mrb_get_args(mrb, "z~!", &p);
  return p ? mrb_str_new_cstr(mrb, p) : mrb_nil_value();
}

static mrb_value
af_conv_alist_alt(mrb_state *mrb, mrb_value klass)
{
  const mrb_value *p;
  mrb_int len;
  mrb_get_args(mrb, "a~!", &p, &len);
  return p ? mrb_int_value(mrb, len) : mrb_nil_value();
}

/* `~` on a specifier that names no type to convert to */
static mrb_value
af_bad_modifier(mrb_state *mrb, mrb_value klass)
{
  mrb_bool t;
  mrb_get_args(mrb, "b~", &t);
  return mrb_bool_value(t);
}

/* marked `i`: the numeric types are read as they always were, and only a
   value none of them reads is asked for `to_int` */
static mrb_value
af_conv_int(mrb_state *mrb, mrb_value klass)
{
  mrb_int n;
  mrb_get_args(mrb, "i~", &n);
  return mrb_fixnum_value(n);
}

/* unmarked: `i` demands a value it can read on its own, as it always did */
static mrb_value
af_strict_int(mrb_state *mrb, mrb_value klass)
{
  mrb_int n;
  mrb_get_args(mrb, "i", &n);
  return mrb_fixnum_value(n);
}

/* marked `i` on the general path */
static mrb_value
af_conv_int_alt(mrb_state *mrb, mrb_value klass)
{
  mrb_int n;
  mrb_bool given;
  mrb_get_args(mrb, "|i~?", &n, &given);
  return given ? mrb_fixnum_value(n) : mrb_nil_value();
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
  mrb_define_class_method(mrb, af, "conv_ptr", af_conv_ptr, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_cstr", af_conv_cstr, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_alist", af_conv_alist, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "strict_ptr", af_strict_ptr, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_ptr_alt", af_conv_ptr_alt, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_cstr_alt", af_conv_cstr_alt, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_alist_alt", af_conv_alist_alt, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_int", af_conv_int, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "strict_int", af_strict_int, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, af, "conv_int_alt", af_conv_int_alt, MRB_ARGS_OPT(1));
}
