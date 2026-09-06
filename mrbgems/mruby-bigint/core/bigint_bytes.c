/**
** @file mruby/bigint_bytes.c - Integer as a spelled byte order
**
** See Copyright Notice in mruby.h
*/

/* Nothing in mruby calls what is here.  It is a translation unit of its own
   so that a program links it only where it calls it: an encoder for a wire
   format that carries arbitrary-precision integers, CBOR's bignum tags or
   ASN.1's INTEGER, which has to write the value in an order the format
   spells rather than the one this machine happens to store limbs in. */

#include <mruby.h>
#include <mruby/numeric.h>
#include <mruby/object.h>
#include <string.h>
#include "bigint.h"

/* How many bytes the magnitude of `v` occupies, leading zeroes dropped. */
static size_t
uint_bytes(mrb_uint v)
{
  size_t n = 0;
  while (v) { n++; v >>= 8; }
  return n;
}

/* Write the low `n` bytes of `v` at `buf`, most significant first. */
static void
put_uint(uint8_t *buf, mrb_uint v, size_t n)
{
  while (n-- > 0) {
    buf[n] = (uint8_t)(v & 0xff);
    v >>= 8;
  }
}

MRB_API size_t
mrb_integer_to_bytes(mrb_state *mrb, mrb_value x, uint8_t *buf, size_t len, int *sign)
{
  if (mrb_integer_p(x)) {
    mrb_int v = mrb_integer(x);
    /* Negating through mrb_uint rather than mrb_int: the magnitude of the
       most negative value has no mrb_int to hold it. */
    mrb_uint m = (v < 0) ? (mrb_uint)0 - (mrb_uint)v : (mrb_uint)v;
    size_t n = uint_bytes(m);

    if (sign) *sign = (v > 0) ? 1 : (v < 0) ? -1 : 0;
    if (buf && len >= n) put_uint(buf, m, n);
    return n;
  }
  if (mrb_bigint_p(x)) {
    struct RBigint *b = RBIGINT(x);
    const mp_limb *ary = RBIGINT_ARY(b);
    size_t sz = RBIGINT_SIZE(b);
    size_t top, n;

    while (sz > 0 && ary[sz-1] == 0) sz--;
    if (sign) *sign = (sz == 0) ? 0 : (RBIGINT_SIGN(b) < 0) ? -1 : 1;
    if (sz == 0) return 0;

    /* The leading limb spells only the bytes it needs; every limb below it
       spells all of them, zeroes included. */
    top = uint_bytes((mrb_uint)ary[sz-1]);
    n = top + (sz - 1) * sizeof(mp_limb);
    if (buf && len >= n) {
      size_t i;
      put_uint(buf, (mrb_uint)ary[sz-1], top);
      buf += top;
      for (i = sz - 1; i > 0; i--) {
        put_uint(buf, (mrb_uint)ary[i-1], sizeof(mp_limb));
        buf += sizeof(mp_limb);
      }
    }
    return n;
  }
  mrb_raisef(mrb, E_TYPE_ERROR, "%Y is not an Integer", x);
  return 0;
}

MRB_API mrb_value
mrb_integer_from_bytes(mrb_state *mrb, const uint8_t *bytes, size_t len, int sign)
{
  size_t nl, i;
  mpz_t z;
  MPZ_CTX_INIT(mrb, ctx, pool);

  while (len > 0 && *bytes == 0) { bytes++; len--; }
  if (len == 0 || sign == 0) return mrb_fixnum_value(0);
  if (len > SIZE_MAX - (sizeof(mp_limb) - 1)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "bigint size too large");
  }
  nl = (len + sizeof(mp_limb) - 1) / sizeof(mp_limb);

  z.p = (mp_limb*)mrb_calloc(mrb, nl, sizeof(mp_limb));
  z.sz = nl;
  z.sn = (sign < 0) ? -1 : 1;
  /* Byte i counts for 8*(len-1-i), which names both the limb it lands in and
     where in that limb it sits, on a machine of either byte order. */
  for (i = 0; i < len; i++) {
    size_t place = len - 1 - i;
    z.p[place / sizeof(mp_limb)] |=
      (mp_limb)bytes[i] << ((place % sizeof(mp_limb)) * 8);
  }
  return mrb_bint_new_mpz(ctx, &z);
}
