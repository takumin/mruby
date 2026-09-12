#ifndef MRC_CCONTEXT_H
#define MRC_CCONTEXT_H

#include "mrc_common.h"
#include "mrc_diagnostic.h"
#include "mrc_throw.h"
#include "mrc_pool.h"
#include <stddef.h>

MRC_BEGIN_DECL

typedef pm_node_t mrc_node;
typedef pm_parser_t mrc_parser_state;
typedef pm_constant_id_list_t mrc_constant_id_list;
typedef struct {
  pm_parser_t parser;
  pm_options_t options;
  pm_string_t input;
  bool parsed;
} pm_parse_result_t;

struct mrc_diagnostic_list;

typedef struct mrc_filename_table {
  const char *filename;
  uint32_t start;
  /* Where this file's own `# frozen_string_literal` comment is, MRC_POS_NONE
     where it has none, and what it asked for; see src/compile.c. */
  uint32_t frozen_comment;
  mrc_bool frozen_string_literal;
} mrc_filename_table;

/* Further along the source than any offset in it: `start` is 32 bits wide,
   so a source that reached this could not be indexed by one. */
#define MRC_POS_NONE UINT32_MAX

/* The file a byte offset of the joined source falls in. */
static inline uint16_t
mrc_filename_index(const mrc_filename_table *table, uint16_t length, uint32_t pos)
{
  uint16_t lo = 0, hi = length;

  while (1 < hi - lo) {
    uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
    if (table[mid].start <= pos) lo = mid;
    else hi = mid;
  }
  return lo;
}

typedef struct mrc_ccontext {
  mrb_state *mrb;
  struct mrc_jmpbuf *jmp;
  mrc_parser_state *p;
  pm_options_t *options; // instead of mrb_sym *syms
  int slen;
  char *filename;
  uint16_t lineno;
  struct RClass *target_class;
  mrc_bool capture_errors:1;   /* output: an error was recorded */
  mrc_bool quiet_errors:1;     /* input: caller reports them itself (eval) */
  mrc_bool dump_ast:1;
  mrc_bool dump_result:1;
  mrc_bool no_exec:1;
  mrc_bool keep_lv:1;
  mrc_bool no_optimize:1;
  mrc_bool no_ext_ops:1;
#if defined(MRC_TARGET_MRUBY)
  const struct RProc *upper;
#endif

  // TODO
  //size_t parser_nerr;
  struct mrc_diagnostic_list *diagnostic_list;

  // For PICOIRB
  uint16_t scope_sp;

  /* Where in the joined source each of the files given to this context
     begins, so that a position can be told which file it came from. The
     codegen and the diagnostics both read it, whether or not stdio is in. */
  mrc_filename_table *filename_table;
  uint16_t filename_table_length;
  uint16_t current_filename_index;

#ifndef MRC_NO_STDIO
  mrc_pool *pool; // for codedump
#endif

  /* The arena everything Prism allocates for this context is taken from, and
     the arena of the context this one was made inside of, put back when this
     one is freed. Unused where Prism allocates through libc; see
     prism_xallocator.h for what the arena is for. */
  void *prism_arena;
  void *prism_arena_outer;

  /* How deep the brackets stand where the lexer is, so that a nesting Prism
     would recurse through is refused instead. See src/compile.c. */
  uint32_t nesting;
} mrc_ccontext;                 /* compiler context */

#ifdef MRC_TARGET_MRUBY
static inline int mrc_gc_arena_save(mrc_ccontext *c)
{
  if (!c->mrb) return 0;
  return mrb_gc_arena_save(c->mrb);
}
static inline void mrc_gc_arena_restore(mrc_ccontext *c, int ai)
{
  if (!c->mrb) return;
  mrb_gc_arena_restore(c->mrb, ai);
}
#else
# define mrc_gc_arena_save(c)        0;(void)ai
# define mrc_gc_arena_restore(c,ai)
#endif

mrc_ccontext *mrc_ccontext_new(mrb_state *mrb);
void mrc_ccontext_cleanup_local_variables(mrc_ccontext *c);
const char *mrc_ccontext_filename(mrc_ccontext *c, const char *s);
void mrc_ccontext_free(mrc_ccontext *c);

MRC_END_DECL

#endif // MRC_CCONTEXT_H
