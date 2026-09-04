#include <string.h>
#include "mrc_ccontext.h"

typedef struct {
  int index;
  const char *lit;
} mrc_sym_entry;

static mrc_sym_entry symTable[] = {
/* mrc_presym.h defines these as lookup macros; drop them first in case it
   is visible in this translation unit (e.g. the amalgamated build) */
#undef MRC_OPSYM_2
#undef MRC_SYM_1
#undef MRC_SYM_2
#define MRC_OPSYM_2(name, lit, num) {num, #lit},
#define MRC_SYM_1(lit, num)         {num, #lit},
#define MRC_SYM_2(name, lit, num)   {num, #lit},
#include "mrc_presym.inc"
#undef MRC_OPSYM_2
#undef MRC_SYM_1
#undef MRC_SYM_2
  {0, NULL} // sentinel
};

/* mrc_presym.inc numbers its literals from 1, so slot 0 is never used and the
   enum values in mrc_presym.h index this array directly. */
#define MRC_PRESYM_SIZE (sizeof(symTable) / sizeof(symTable[0]))

static mrc_sym presym_ids[MRC_PRESYM_SIZE];

mrc_sym
mrc_presym_id(mrc_sym sym)
{
  mrc_assert(0 < sym && (size_t)sym < MRC_PRESYM_SIZE);
  return presym_ids[sym];
}

void
mrc_init_presym(pm_constant_pool_t *pool)
{
  for (int i = 0; ; i++) {
    if (symTable[i].lit == NULL) { break; }
    /* mrc_presym.h turns each literal's number into an index into presym_ids,
       so the numbers have to run from 1 in table order. */
    mrc_assert(symTable[i].index == i + 1);
    /* The pool is not necessarily empty here: when the compile context carries
       enclosing scopes (an eval or a binding), the parser has already interned
       those scopes' local names, and a local can be named after a presym
       literal -- an anonymous rest parameter is stored under the name `*'.
       The insert then hands back the id that name already has, so the presym
       ids are not contiguous and cannot be derived from a single offset. */
    presym_ids[symTable[i].index] =
      (mrc_sym)pm_constant_pool_insert_constant(pool, (const uint8_t *)symTable[i].lit, strlen(symTable[i].lit));
  }
}
