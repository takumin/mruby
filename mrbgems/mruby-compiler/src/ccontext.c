#include <string.h>
#include "../include/mrc_ccontext.h"
#include "../include/mrc_parser_util.h"

#if defined(MRC_TARGET_MRUBY)
/* The Prism xallocator routes allocations through this mrb_state. Define it
   in the compiler library so every executable that links libmruby (not just
   the mrbc/mruby/mirb front-ends) resolves the symbol. The front-ends assign
   it unconditionally for the mruby target, so it must exist regardless of
   MRC_ALLOC_LIBC even though only the non-libc allocator dereferences it. */
mrb_state *global_mrb = NULL;
#endif

MRC_API mrc_ccontext *
mrc_ccontext_new(mrb_state *mrb)
{
  mrc_ccontext temp_c = {0};
#if defined(MRC_TARGET_MRUBY) && !defined(MRC_ALLOC_LIBC)
  global_mrb = mrb;
#endif
  temp_c.mrb = mrb;
  mrc_ccontext *c = (mrc_ccontext *)mrc_calloc((&temp_c), 1, sizeof(mrc_ccontext));
  c->p = (mrc_parser_state *)mrc_calloc((&temp_c), 1, sizeof(mrc_parser_state));
  c->mrb = temp_c.mrb;
  return c;
}


MRC_API void
mrc_ccontext_cleanup_local_variables(mrc_ccontext *cc)
{
  cc->keep_lv = FALSE;

  if (cc->options && cc->options->scopes) {
    for (size_t i = 0; i < cc->options->scopes[0].locals_count; i++) {
      mrc_free(cc, (void *)cc->options->scopes[0].locals[i].source);
    }
    mrc_free(cc, cc->options);
  }
}

MRC_API mrc_bool
mrc_ccontext_push_upper_scope(mrc_ccontext *c, const mrc_upper_scope *scope)
{
  mrc_upper_scope *scopes, *dst;
  size_t i;

  scopes = (mrc_upper_scope *)mrc_realloc(c, c->upper_scopes,
                                          sizeof(mrc_upper_scope) * (c->upper_scopes_count + 1));
  if (scopes == NULL) return FALSE;
  c->upper_scopes = scopes;

  dst = &scopes[c->upper_scopes_count];
  *dst = *scope;
  dst->locals = NULL;
  dst->locals_count = 0;
  /* Counted before it is filled in, so that whatever has been copied so far is
     released with the context even if a copy below fails. */
  c->upper_scopes_count++;

  if (scope->locals_count > 0) {
    dst->locals = (mrc_upper_local *)mrc_calloc(c, scope->locals_count, sizeof(mrc_upper_local));
    if (dst->locals == NULL) return FALSE;
    dst->locals_count = scope->locals_count;
    for (i = 0; i < scope->locals_count; i++) {
      const mrc_upper_local *src = &scope->locals[i];
      char *copy;

      if (src->name == NULL) continue;
      /* One byte over the length: the names are compared by length, but a
         terminator keeps them printable in a debugger. */
      copy = (char *)mrc_malloc(c, src->length + 1);
      if (copy == NULL) return FALSE;
      memcpy(copy, src->name, src->length);
      copy[src->length] = '\0';
      dst->locals[i].name = copy;
      dst->locals[i].length = src->length;
    }
  }
  return TRUE;
}

static void
mrc_ccontext_upper_scopes_free(mrc_ccontext *c)
{
  size_t i, j;

  for (i = 0; i < c->upper_scopes_count; i++) {
    mrc_upper_scope *scope = &c->upper_scopes[i];
    for (j = 0; j < scope->locals_count; j++) {
      if (scope->locals[j].name) mrc_free(c, (void *)scope->locals[j].name);
    }
    if (scope->locals) mrc_free(c, scope->locals);
  }
  if (c->upper_scopes) mrc_free(c, c->upper_scopes);
  c->upper_scopes = NULL;
  c->upper_scopes_count = 0;
}

MRC_API const char *
mrc_ccontext_filename(mrc_ccontext *c, const char *s)
{
  if (s) {
    size_t len = strlen(s);
    char *p = (char*)mrc_malloc(c, len + 1);

    if (p == NULL) return NULL;
    memcpy(p, s, len + 1);
    if (c->filename) {
      mrc_free(c, c->filename);
    }
    c->filename = p;
  }
  return c->filename;
}

MRC_API void
mrc_ccontext_free(mrc_ccontext *c)
{
  if (c->options) {
    /* pm_options_free() releases the scope and locals arrays but not the
       per-local name copies (they are PM_STRING_CONSTANT, which pm_string_free
       leaves alone) nor the options struct itself, so free those here. The
       copies must go first, before pm_options_free() releases the arrays. */
    for (size_t s = 0; s < c->options->scopes_count; s++) {
      pm_options_scope_t *scope = &c->options->scopes[s];
      for (size_t l = 0; l < scope->locals_count; l++) {
        mrc_free(c, (void *)scope->locals[l].source);
      }
    }
    pm_options_free(c->options);
    mrc_free(c, c->options);
    c->options = NULL;
  }
  mrc_ccontext_upper_scopes_free(c);
  if (c->filename_table) mrc_free(c, c->filename_table);
  if (c->filename) mrc_free(c, c->filename);
  pm_parser_free(c->p);
  mrc_diagnostic_list_free(c);
  if (c->p->lex_callback) {
    mrc_free(c, c->p->lex_callback);
  }
  mrc_free(c, c->p);
  mrc_free(c, c);
}
