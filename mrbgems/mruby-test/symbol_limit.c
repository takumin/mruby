/*
** symbol_limit.c - the end of the runtime symbol table
**
** A symbol interned at runtime takes the value of its symbol table index plus
** MRB_PRESYM_MAX, and those indices stop below the first value that is read
** back as an inline symbol.  Walking to that end takes about a million
** interns, so this lowers `mrb->symidx_max` for the length of one call
** instead: what the test asks is whether interning past the end raises,
** rather than handing out a value whose name is already something else's.
*/

#include <string.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/error.h>
#include <mruby/presym.h>

#define SYM_LIMIT_PREFIX "symbol_limit_probe_"
#define SYM_LIMIT_NAME_MAX (sizeof(SYM_LIMIT_PREFIX) + 24)

struct sym_limit_probe {
  mrb_sym base;   /* what the names are numbered from, so a later call is new */
  mrb_int tries;  /* how many names to intern */
  mrb_int done;   /* how many were interned before an exception stopped it */
};

/* Write a name long enough that sym_inline_pack() leaves it to the table. */
static size_t
sym_limit_name(char *buf, mrb_sym base, mrb_int i)
{
  size_t len = sizeof(SYM_LIMIT_PREFIX) - 1;
  char digits[24];
  size_t n = 0;
  uint64_t v = (uint64_t)base * 1000 + (uint64_t)i;

  memcpy(buf, SYM_LIMIT_PREFIX, len);
  do {
    digits[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  } while (v > 0);
  while (n > 0) buf[len++] = digits[--n];
  buf[len] = '\0';
  return len;
}

static mrb_value
sym_limit_intern(mrb_state *mrb, void *ud)
{
  struct sym_limit_probe *p = (struct sym_limit_probe*)ud;

  for (mrb_int i = 0; i < p->tries; i++) {
    char buf[SYM_LIMIT_NAME_MAX];
    size_t len = sym_limit_name(buf, p->base, i);
    mrb_sym sym = mrb_intern(mrb, buf, len);
    mrb_int namelen;
    const char *name = mrb_sym_name_len(mrb, sym, &namelen);

    if (name == NULL || (size_t)namelen != len || memcmp(name, buf, len) != 0) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "symbol name did not survive interning");
    }
    p->done++;
  }
  return mrb_nil_value();
}

/* SymbolLimit.probe(headroom) -> [interned, message]
 *
 * Intern distinct names, a few more than `headroom` of them, with the symbol
 * table left `headroom` indices short of its end.  Answers how many names were
 * interned and the message of the exception that stopped the rest, or nil for
 * that message if nothing did.  Each name is checked on the way for the one
 * that matters: that it comes back out of the symbol it went in as.
 */
static mrb_value
sym_limit_probe(mrb_state *mrb, mrb_value self)
{
  struct sym_limit_probe p;
  mrb_sym saved = mrb->symidx_max;
  mrb_int headroom;
  mrb_bool error;
  mrb_value exc, result;

  mrb_get_args(mrb, "i", &headroom);
  if (headroom < 0 || (mrb_sym)headroom > saved - mrb->symidx) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "headroom out of range");
  }

  p.base = mrb->symidx;
  p.tries = headroom + 4;
  p.done = 0;

  mrb->symidx_max = mrb->symidx + (mrb_sym)headroom;
  exc = mrb_protect_error(mrb, sym_limit_intern, &p, &error);
  mrb->symidx_max = saved;

  result = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, result, mrb_int_value(mrb, p.done));
  mrb_ary_push(mrb, result,
               error ? mrb_funcall_id(mrb, exc, MRB_SYM(message), 0) : mrb_nil_value());
  return result;
}

void
mrb_init_test_symbol_limit(mrb_state *mrb)
{
  struct RClass *c = mrb_define_module(mrb, "SymbolLimit");

  mrb_define_module_function(mrb, c, "probe", sym_limit_probe, MRB_ARGS_REQ(1));
}
