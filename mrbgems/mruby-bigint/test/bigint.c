#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/internal.h>
#include <mruby/numeric.h>
#include <string.h>

/* BigintTest.int64_roundtrip(integer) -> Integer
 *
 * Sends an Integer through the int64_t conversion a C extension uses to read
 * one, then builds an Integer back from the result.  A value the conversion
 * carries intact therefore comes back equal to itself, and one it refuses
 * raises RangeError instead.  Whether the argument arrives as a Bignum
 * depends on the width of mrb_int, so both representations are accepted.
 */
static mrb_value
test_int64_roundtrip(mrb_state *mrb, mrb_value self)
{
  mrb_value v;
  int64_t n;

  mrb_get_args(mrb, "o", &v);
  if (mrb_integer_p(v)) {
    n = (int64_t)mrb_integer(v);
  }
  else if (mrb_bigint_p(v)) {
    n = mrb_bint_as_int64(mrb, v);
  }
  else {
    mrb_raisef(mrb, E_TYPE_ERROR, "%Y is not an Integer", v);
    return mrb_nil_value();
  }

#ifdef MRB_INT32
  if (n < MRB_INT_MIN || n > MRB_INT_MAX) {
    return mrb_bint_new_int64(mrb, n);
  }
#endif
  return mrb_int_value(mrb, (mrb_int)n);
}


/* BigintTest.to_bytes(integer) -> [sign, [byte, ...]]
 *
 * The magnitude an extension writing a wire format would write, asked for
 * twice the way such a caller asks: once with no buffer, to learn the
 * length, and once with one.  Both answers are returned so a test can hold
 * them to each other.
 */
static mrb_value
test_to_bytes(mrb_state *mrb, mrb_value self)
{
  mrb_value v, ary;
  uint8_t *buf;
  size_t len, got, i;
  int sign = 0;

  mrb_get_args(mrb, "o", &v);
  len = mrb_integer_to_bytes(mrb, v, NULL, 0, NULL);
  buf = (uint8_t*)mrb_malloc(mrb, len ? len : 1);
  got = mrb_integer_to_bytes(mrb, v, buf, len, &sign);
  if (got != len) {
    mrb_free(mrb, buf);
    mrb_raise(mrb, E_RUNTIME_ERROR, "length asked for and length written differ");
  }
  ary = mrb_ary_new_capa(mrb, (mrb_int)len);
  for (i = 0; i < len; i++) {
    mrb_ary_push(mrb, ary, mrb_fixnum_value(buf[i]));
  }
  mrb_free(mrb, buf);
  return mrb_assoc_new(mrb, mrb_fixnum_value(sign), ary);
}

/* BigintTest.from_bytes(sign, [byte, ...]) -> Integer */
static mrb_value
test_from_bytes(mrb_state *mrb, mrb_value self)
{
  mrb_value ary, ret;
  mrb_int sign;
  uint8_t *buf;
  mrb_int len, i;

  mrb_get_args(mrb, "iA", &sign, &ary);
  len = RARRAY_LEN(ary);
  buf = (uint8_t*)mrb_malloc(mrb, len ? (size_t)len : 1);
  for (i = 0; i < len; i++) {
    buf[i] = (uint8_t)mrb_integer(mrb_ary_entry(ary, i));
  }
  ret = mrb_integer_from_bytes(mrb, buf, (size_t)len, (int)sign);
  mrb_free(mrb, buf);
  return ret;
}

/* BigintTest.to_bytes_short(integer) -> Integer
 *
 * What a buffer one byte too small answers: the length it needed, with
 * nothing written.
 */
static mrb_value
test_to_bytes_short(mrb_state *mrb, mrb_value self)
{
  mrb_value v;
  uint8_t buf[64];
  size_t len;

  mrb_get_args(mrb, "o", &v);
  len = mrb_integer_to_bytes(mrb, v, NULL, 0, NULL);
  if (len == 0 || len > sizeof(buf)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "value does not suit this probe");
  }
  memset(buf, 0xff, sizeof(buf));
  if (mrb_integer_to_bytes(mrb, v, buf, len - 1, NULL) != len) return mrb_fixnum_value(-1);
  for (size_t i = 0; i < sizeof(buf); i++) {
    if (buf[i] != 0xff) return mrb_fixnum_value(-2);   /* it wrote something */
  }
  return mrb_fixnum_value((mrb_int)len);
}

void
mrb_mruby_bigint_gem_test(mrb_state *mrb)
{
  struct RClass *test = mrb_define_module(mrb, "BigintTest");

  mrb_define_module_function(mrb, test, "int64_roundtrip", test_int64_roundtrip,
                             MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, test, "to_bytes", test_to_bytes, MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, test, "from_bytes", test_from_bytes, MRB_ARGS_REQ(2));
  mrb_define_module_function(mrb, test, "to_bytes_short", test_to_bytes_short,
                             MRB_ARGS_REQ(1));
}
