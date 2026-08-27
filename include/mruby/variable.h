/**
** @file mruby/variable.h - mruby variables
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_VARIABLE_H
#define MRUBY_VARIABLE_H

#if defined(__GNUC__) || defined(__clang__)
#define MRB_MEM_PREFETCH(addr) __builtin_prefetch(addr, 0, 1)
#else
#define MRB_MEM_PREFETCH(addr)
#endif

#include "common.h"

/**
 * Functions to access mruby variables.
 */
MRB_BEGIN_DECL

MRB_API mrb_value mrb_const_get(mrb_state*, mrb_value, mrb_sym);
MRB_API void mrb_const_set(mrb_state*, mrb_value, mrb_sym, mrb_value);
MRB_API mrb_bool mrb_const_defined(mrb_state*, mrb_value, mrb_sym);
MRB_API void mrb_const_remove(mrb_state*, mrb_value, mrb_sym);

MRB_API mrb_bool mrb_iv_name_sym_p(mrb_state *mrb, mrb_sym sym);
MRB_API void mrb_iv_name_sym_check(mrb_state *mrb, mrb_sym sym);
MRB_API mrb_value mrb_obj_iv_get(mrb_state *mrb, struct RObject *obj, mrb_sym sym);
MRB_API void mrb_obj_iv_set(mrb_state *mrb, struct RObject *obj, mrb_sym sym, mrb_value v);
MRB_API mrb_bool mrb_obj_iv_defined(mrb_state *mrb, struct RObject *obj, mrb_sym sym);
MRB_API mrb_value mrb_iv_get(mrb_state *mrb, mrb_value obj, mrb_sym sym);
MRB_API void mrb_iv_set(mrb_state *mrb, mrb_value obj, mrb_sym sym, mrb_value v);
MRB_API mrb_bool mrb_iv_defined(mrb_state*, mrb_value, mrb_sym);
MRB_API mrb_value mrb_iv_remove(mrb_state *mrb, mrb_value obj, mrb_sym sym);
MRB_API void mrb_iv_copy(mrb_state *mrb, mrb_value dst, mrb_value src);
MRB_API mrb_bool mrb_const_defined_at(mrb_state *mrb, mrb_value mod, mrb_sym id);

/**
 * Get a global variable. Will return nil if the var does not exist
 *
 * Example:
 *
 *     !!!ruby
 *     # Ruby style
 *     var = $value
 *
 *     !!!c
 *     // C style
 *     mrb_sym sym = mrb_intern_lit(mrb, "$value");
 *     mrb_value var = mrb_gv_get(mrb, sym);
 *
 * @param mrb The mruby state reference
 * @param sym The name of the global variable
 * @return The value of that global variable. May be nil
 */
MRB_API mrb_value mrb_gv_get(mrb_state *mrb, mrb_sym sym);

/**
 * Set a global variable
 *
 * Example:
 *
 *     !!!ruby
 *     # Ruby style
 *     $value = "foo"
 *
 *     !!!c
 *     // C style
 *     mrb_sym sym = mrb_intern_lit(mrb, "$value");
 *     mrb_gv_set(mrb, sym, mrb_str_new_lit("foo"));
 *
 * @param mrb The mruby state reference
 * @param sym The name of the global variable
 * @param val The value of the global variable
 */
MRB_API void mrb_gv_set(mrb_state *mrb, mrb_sym sym, mrb_value val);

/**
 * Remove a global variable.
 *
 * Example:
 *
 *     # Ruby style
 *     $value = nil
 *
 *     // C style
 *     mrb_sym sym = mrb_intern_lit(mrb, "$value");
 *     mrb_gv_remove(mrb, sym);
 *
 * @param mrb The mruby state reference
 * @param sym The name of the global variable
 */
MRB_API void mrb_gv_remove(mrb_state *mrb, mrb_sym sym);

/**
 * Turn a global variable name into a virtual one: `mrb_gv_get()` and
 * `mrb_gv_set()` on `sym` call `get` and `set` instead of touching the
 * stored value, so a name whose semantics are not "one process-wide slot"
 * (CRuby's `$~` is per method scope) can keep its global spelling. The
 * dispatch lives behind a sentinel stored in the globals table itself, so
 * the name stays defined and listed in `global_variables`, and an ordinary
 * global pays one type test on access. `set` may raise; removing the name
 * with `mrb_gv_remove()` removes the hook with it.
 *
 * @param mrb The mruby state reference
 * @param sym The name of the global variable
 * @param get Called to produce the variable's value
 * @param set Called with the value assigned to the variable
 */
MRB_API void mrb_gv_define_virtual(mrb_state *mrb, mrb_sym sym, mrb_value (*get)(mrb_state*), void (*set)(mrb_state*, mrb_value));

/**
 * The keys of a Ruby scope's special variables, CRuby's `enum
 * vm_svar_index` with CRuby's numbering. Each scope holds one container of
 * MRB_SVAR_MAX slots (allocated lazily, see mrb_vm_svar_set()), and each
 * key names one slot in it. The namespace is owned by the core: a new
 * special variable takes a new enumerator here, never a key minted at
 * runtime, so a key means the same slot in every build and gem load order.
 */
enum mrb_svar_index {
  MRB_SVAR_LASTLINE = 0,        /* $_ */
  MRB_SVAR_BACKREF,             /* $~ */
  MRB_SVAR_MAX
};

/**
 * Reads one special-variable slot of the owning Ruby scope, resolved like
 * CRuby's svar (a C frame reads through to the Ruby frame below it, a block
 * shares its defining method's container, and a scope that returned keeps
 * its container in its env, so a proc outliving it still reads the value).
 * The core stores and marks the slots but gives them no meaning: a key's
 * semantics belong to whoever pairs these accessors with a virtual global,
 * the way mruby-regexp keeps `$~`'s MatchData under MRB_SVAR_BACKREF.
 */
MRB_API mrb_value mrb_vm_svar_get(mrb_state *mrb, enum mrb_svar_index key);

/**
 * Writes one special-variable slot of the owning Ruby scope. The slot holds
 * any mrb_value, immediates included. Any richer contract, like
 * mruby-regexp's TypeError for `$~ = <not a MatchData>`, belongs to the
 * caller. The scope's container is allocated on the first non-nil write; a
 * nil write into a scope that has none is dropped, nil being what a missing
 * slot already reads as.
 */
MRB_API void mrb_vm_svar_set(mrb_state *mrb, enum mrb_svar_index key, mrb_value v);

MRB_API mrb_value mrb_cv_get(mrb_state *mrb, mrb_value mod, mrb_sym sym);
MRB_API void mrb_mod_cv_set(mrb_state *mrb, struct RClass * c, mrb_sym sym, mrb_value v);
MRB_API void mrb_cv_set(mrb_state *mrb, mrb_value mod, mrb_sym sym, mrb_value v);
MRB_API mrb_bool mrb_cv_defined(mrb_state *mrb, mrb_value mod, mrb_sym sym);

/* return non zero to break the loop */
typedef int (mrb_iv_foreach_func)(mrb_state*,mrb_sym,mrb_value,void*);
MRB_API void mrb_iv_foreach(mrb_state *mrb, mrb_value obj, mrb_iv_foreach_func *func, void *p);

MRB_END_DECL

#endif  /* MRUBY_VARIABLE_H */
