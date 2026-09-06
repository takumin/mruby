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
} mrc_filename_table;

/* One enclosing scope, as the compiler sees it while compiling an `eval` or a
   `binding`.  The host walks whatever it uses to represent a call frame and
   hands the names down through mrc_ccontext_push_upper_scope(), so the parser
   and the code generator resolve outer local variables without reading a VM
   structure or interning into a VM symbol table. */
typedef struct mrc_upper_local {
  const char *name;             /* NULL where the local carries no name */
  size_t length;
} mrc_upper_local;

typedef struct mrc_upper_scope {
  mrc_upper_local *locals;
  size_t locals_count;
  uint16_t nlocals;             /* the locals plus the receiver */
  mrc_bool has_locals:1;        /* the frame carries a local variable table */
  mrc_bool boundary:1;          /* the search for an outer local stops here */
  mrc_bool lvspace:1;           /* holds no locals of its own; not a parser scope */
  /* The one block the copied names of this scope live in.  It belongs to the
     context that stored the scope; a caller filling a scope to push leaves it
     NULL and keeps its own names wherever they already are. */
  char *name_pool;
} mrc_upper_scope;

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
  /* the local names in `options` are this context's to free; clear when they
     point into the name pool of an enclosing scope, which owns them instead */
  mrc_bool options_locals_owned:1;
  /* enclosing scopes, innermost first */
  mrc_upper_scope *upper_scopes;
  size_t upper_scopes_count;

  // TODO
  //size_t parser_nerr;
  struct mrc_diagnostic_list *diagnostic_list;

  // For PICOIRB
  uint16_t scope_sp;

#ifndef MRC_NO_STDIO
  mrc_pool *pool; // for codedump

  mrc_filename_table *filename_table;
  uint16_t filename_table_length;
  uint16_t current_filename_index;
#endif
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
/* Append one enclosing scope, from the innermost outwards.  The names are
   copied, so the caller keeps ownership of the scope it fills in. */
mrc_bool mrc_ccontext_push_upper_scope(mrc_ccontext *c, const mrc_upper_scope *scope);
const char *mrc_ccontext_filename(mrc_ccontext *c, const char *s);
void mrc_ccontext_free(mrc_ccontext *c);

MRC_END_DECL

#endif // MRC_CCONTEXT_H
