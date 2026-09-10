/*
** frozen_str.c - the instruction a frozen string literal turns into
**
** OP_LOADL reaches a string pool entry only from a literal that its file
** froze, and no compiler in this tree emits that yet.  FrozenLit.site()
** builds the two instruction irep by hand so that the VM's side of it - the
** string a pool entry is answered with, whether the entry owns its bytes or
** points at the ones the binary already holds - is testable on its own.
*/

#include <string.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/irep.h>
#include <mruby/opcode.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/internal.h>

/* R1 = Pool[0]; return R1 */
static const mrb_code fsl_iseq[] = { OP_LOADL, 1, 0, OP_RETURN, 1 };

static mrb_value
fsl_proc(mrb_state *mrb, mrb_irep_pool *pool)
{
  mrb_irep *irep = mrb_add_irep(mrb);
  mrb_value proc;

  irep->iseq = fsl_iseq;
  irep->ilen = sizeof(fsl_iseq);
  irep->flags = MRB_ISEQ_NO_FREE;
  irep->pool = pool;
  irep->plen = 1;
  irep->nlocals = 1;
  irep->nregs = 2;
  proc = mrb_obj_value(mrb_proc_new(mrb, irep));
  mrb_irep_decref(mrb, irep); /* the proc holds it now */
  return proc;
}

/* FrozenLit.site(str) -> Proc
 *
 * A place in a program where the frozen literal `str` stands.  Calling it
 * runs the literal.
 */
static mrb_value
fsl_site(mrb_state *mrb, mrb_value self)
{
  const char *s;
  mrb_int len;
  mrb_irep_pool *pool;
  char *p;

  mrb_get_args(mrb, "s", &s, &len);
  pool = (mrb_irep_pool*)mrb_malloc(mrb, sizeof(mrb_irep_pool));
  p = (char*)mrb_malloc(mrb, len+1);
  memcpy(p, s, len);
  p[len] = '\0';
  pool[0].tt = (uint32_t)(len<<2) | IREP_TT_STR;
  pool[0].u.str = p;
  return fsl_proc(mrb, pool);
}

/* FrozenLit.static_site -> Proc
 *
 * The same, over a pool entry that points into read-only data instead of
 * owning its bytes, which is what a program run from ROM carries.
 */
static mrb_value
fsl_static_site(mrb_state *mrb, mrb_value self)
{
  static const char str[] = "a literal the binary already holds";
  mrb_irep_pool *pool = (mrb_irep_pool*)mrb_malloc(mrb, sizeof(mrb_irep_pool));

  pool[0].tt = (uint32_t)((sizeof(str)-1)<<2) | IREP_TT_SSTR;
  pool[0].u.str = str;
  return fsl_proc(mrb, pool);
}

void
mrb_init_test_frozen_str(mrb_state *mrb)
{
  struct RClass *c = mrb_define_module(mrb, "FrozenLit");

  mrb_define_module_function(mrb, c, "site", fsl_site, MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, c, "static_site", fsl_static_site, MRB_ARGS_NONE());
}
