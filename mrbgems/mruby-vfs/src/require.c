/*
** require.c - Kernel#require, #require_relative, #load and #__dir__ over the VFS
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/debug.h>
#include <mruby/dump.h>
#include <mruby/error.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/vfs.h>
#ifdef MRB_VFS_HAVE_COMPILER
# include <mruby/compile.h>
#endif

#include <string.h>

#define E_LOAD_ERROR mrb_exc_get_id(mrb, MRB_SYM(LoadError))

static mrb_bool
vfs_separator_p(char c)
{
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

static mrb_bool
vfs_absolute_p(const char *s, mrb_int len)
{
  if (len == 0) return FALSE;
  if (vfs_separator_p(s[0])) return TRUE;
#ifdef _WIN32
  if (len >= 2 && s[1] == ':' && ((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z')) return TRUE;
#endif
  return FALSE;
}

/* A path that says where it starts, absolute or "./" or "../": looked up as
   it is, never under $LOAD_PATH */
static mrb_bool
vfs_explicit_p(const char *s, mrb_int len)
{
  if (vfs_absolute_p(s, len)) return TRUE;
  if (len >= 2 && s[0] == '.' && vfs_separator_p(s[1])) return TRUE;
  if (len >= 3 && s[0] == '.' && s[1] == '.' && vfs_separator_p(s[2])) return TRUE;
  return FALSE;
}

static mrb_bool
vfs_has_ext_p(const char *s, mrb_int len)
{
  return (len > 3 && memcmp(s + len - 3, ".rb", 3) == 0) ||
         (len > 4 && memcmp(s + len - 4, ".mrb", 4) == 0);
}

/*
 * `path` with its "." and ".." segments and repeated slashes folded away, so
 * that the one file has the one name in $LOADED_FEATURES and in __FILE__
 * whichever way it was reached.  A ".." that would climb above the start of
 * a relative path is kept, since only the host knows where that leads.
 */
static mrb_value
vfs_clean_path(mrb_state *mrb, mrb_value path)
{
  const char *s = RSTRING_PTR(path);
  mrb_int len = RSTRING_LEN(path);
  mrb_bool absolute = len > 0 && s[0] == '/';
  mrb_value out = mrb_str_new_capa(mrb, len);
  mrb_int i = 0;

  while (i < len) {
    mrb_int start = i;
    mrb_int seglen;

    while (i < len && s[i] != '/') i++;
    seglen = i - start;
    if (i < len) i++;  /* the slash */
    if (seglen == 0 || (seglen == 1 && s[start] == '.')) continue;
    if (seglen == 2 && s[start] == '.' && s[start + 1] == '.') {
      mrb_int olen = RSTRING_LEN(out);
      const char *o = RSTRING_PTR(out);
      mrb_int j = olen;

      while (j > 0 && o[j - 1] != '/') j--;
      if (olen > 0 && !(olen - j == 2 && o[j] == '.' && o[j + 1] == '.')) {
        /* a segment to climb out of */
        mrb_str_resize(mrb, out, j > 0 ? j - 1 : 0);
        continue;
      }
      if (absolute) continue;  /* the root has no parent */
    }
    if (RSTRING_LEN(out) > 0) mrb_str_cat_lit(mrb, out, "/");
    mrb_str_cat(mrb, out, s + start, (size_t)seglen);
  }
  if (absolute) {
    mrb_value abs = mrb_str_new_lit(mrb, "/");
    return mrb_str_cat_str(mrb, abs, out);
  }
  if (RSTRING_LEN(out) == 0) return mrb_str_new_lit(mrb, ".");
  return out;
}

static mrb_value
vfs_join(mrb_state *mrb, mrb_value dir, const char *name, mrb_int nlen)
{
  mrb_int dlen = RSTRING_LEN(dir);
  mrb_value path = mrb_str_new_capa(mrb, dlen + 1 + nlen);

  if (dlen > 0) {
    mrb_str_cat(mrb, path, RSTRING_PTR(dir), (size_t)dlen);
    if (!vfs_separator_p(RSTRING_PTR(dir)[dlen - 1])) {
      mrb_str_cat_lit(mrb, path, "/");
    }
  }
  mrb_str_cat(mrb, path, name, (size_t)nlen);
  return path;
}

/* The directory part of `file`: "." when it has none, the root when that is
   all there is */
static mrb_value
vfs_dirname(mrb_state *mrb, const char *file)
{
  size_t i = strlen(file);

  while (i > 0 && !vfs_separator_p(file[i - 1])) i--;
  if (i == 0) return mrb_str_new_lit(mrb, ".");
  while (i > 1 && vfs_separator_p(file[i - 1])) i--;
  return mrb_str_new(mrb, file, (mrb_int)i);
}

/* A regular file at `path`; a backend that cannot answer is one that does
   not have it, as with every other place a search looks */
static mrb_bool
vfs_file_p(mrb_state *mrb, mrb_value path)
{
  enum mrb_vfs_kind kind;

  if (mrb_vfs_stat_str(mrb, path, &kind) == -1) return FALSE;
  return kind == MRB_VFS_FILE;
}

/*
 * The file `name` stands for under `dir` (as it is, when dir is nil): the
 * name itself when it carries an extension, else name.rb and then name.mrb.
 * Nil when there is none.
 */
static mrb_value
vfs_probe(mrb_state *mrb, mrb_value dir, const char *name, mrb_int nlen)
{
  static const char *const exts[] = { "", ".rb", ".mrb" };
  int first = vfs_has_ext_p(name, nlen) ? 0 : 1;
  int last = (first == 0) ? 0 : 2;
  int ai = mrb_gc_arena_save(mrb);

  for (int i = first; i <= last; i++) {
    mrb_value path = mrb_nil_p(dir) ? mrb_str_new(mrb, name, nlen) : vfs_join(mrb, dir, name, nlen);

    mrb_str_cat_cstr(mrb, path, exts[i]);
    path = vfs_clean_path(mrb, path);
    if (vfs_file_p(mrb, path)) {
      mrb_gc_arena_restore(mrb, ai);
      mrb_gc_protect(mrb, path);
      return path;
    }
    mrb_gc_arena_restore(mrb, ai);
  }
  return mrb_nil_value();
}

static mrb_value
vfs_load_path(mrb_state *mrb)
{
  mrb_value load_path = mrb_gv_get(mrb, MRB_GVSYM(LOAD_PATH));

  if (!mrb_array_p(load_path)) {
    mrb_raise(mrb, E_TYPE_ERROR, "$LOAD_PATH is not an Array");
  }
  return load_path;
}

static mrb_value
vfs_loaded_features(mrb_state *mrb)
{
  mrb_value features = mrb_gv_get(mrb, MRB_GVSYM(LOADED_FEATURES));

  if (!mrb_array_p(features)) {
    mrb_raise(mrb, E_TYPE_ERROR, "$LOADED_FEATURES is not an Array");
  }
  return features;
}

/* The features being loaded right now, innermost last: what a `require`
   that arrives at one of them again answers false to */
static mrb_value
vfs_loading(mrb_state *mrb)
{
  return mrb_iv_get(mrb, mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(VFS))), MRB_IVSYM(loading));
}

static mrb_int
vfs_ary_index(mrb_state *mrb, mrb_value ary, mrb_value str)
{
  for (mrb_int i = 0; i < RARRAY_LEN(ary); i++) {
    if (mrb_str_equal(mrb, str, RARRAY_PTR(ary)[i])) return i;
  }
  return -1;
}

static void
vfs_ary_delete(mrb_state *mrb, mrb_value ary, mrb_value str)
{
  mrb_int i = vfs_ary_index(mrb, ary, str);

  if (i >= 0) {
    mrb_ary_splice(mrb, ary, i, 1, mrb_undef_value());
  }
}

static mrb_noreturn void
vfs_raise_load_error(mrb_state *mrb, mrb_value name)
{
  mrb_value exc = mrb_exc_new_str(mrb, E_LOAD_ERROR, mrb_format(mrb, "cannot load such file -- %v", name));

  mrb_iv_set(mrb, exc, MRB_IVSYM(path), name);
  mrb_exc_raise(mrb, exc);
}

/*
 * Running a file
 *
 * The content is RITE bytecode when it says so, and Ruby source otherwise.
 * Either runs at the top level, as the main program does: self is main, the
 * target class Object, and the locals its own.  What the file raises is
 * raised here again, after the compiler context is released.
 */
struct vfs_run {
  mrb_value path;
  mrb_value content;
#ifdef MRB_VFS_HAVE_COMPILER
  mrb_ccontext *cxt;
#endif
};

static mrb_value
vfs_run_body(mrb_state *mrb, void *ptr)
{
  struct vfs_run *r = (struct vfs_run*)ptr;
  const char *buf = RSTRING_PTR(r->content);
  size_t len = (size_t)RSTRING_LEN(r->content);
  mrb_value result = mrb_nil_value();

  if (len >= 4 && memcmp(buf, RITE_BINARY_IDENT, 4) == 0) {
    /* Not mrb_load_irep_buf(): the proc it makes takes its scope from the
       frame it is made in, which here is the C method that called us, and
       a file is its own top level with no scope around it, as the source
       path below makes it. */
    mrb_irep *irep = mrb_read_irep_buf(mrb, buf, len);
    struct RProc *proc;

    if (irep == NULL) {
      mrb_raise(mrb, E_SCRIPT_ERROR, "irep load error");
    }
    proc = mrb_proc_new(mrb, irep);
    mrb_irep_decref(mrb, irep);
    proc->c = NULL;
    proc->upper = NULL;
    MRB_PROC_SET_TARGET_CLASS(proc, mrb->object_class);
    result = mrb_top_run(mrb, proc, mrb_top_self(mrb), 0);
  }
  else {
#ifdef MRB_VFS_HAVE_COMPILER
    struct mrb_parser_state *p;

    if (RSTRING_LEN(r->path) >= UINT16_MAX) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "filename too long");
    }
    r->cxt = mrb_ccontext_new(mrb);
    mrb_ccontext_filename(mrb, r->cxt, RSTRING_CSTR(mrb, r->path));
    r->cxt->capture_errors = TRUE;
    p = mrb_parse_nstring(mrb, buf, len, r->cxt);
    if (p == NULL) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "failed to create parser state (out of memory)");
    }
    if (p->nerr > 0) {
      mrb_value msg;

      if (p->error_buffer[0].message) {
        msg = mrb_format(mrb, "%v:%d: %s", r->path, (int)p->error_buffer[0].lineno, p->error_buffer[0].message);
      }
      else {
        msg = mrb_format(mrb, "%v: syntax error", r->path);
      }
      mrb_parser_free(p);
      mrb_exc_raise(mrb, mrb_exc_new_str(mrb, E_SYNTAX_ERROR, msg));
    }
    result = mrb_load_exec(mrb, p, r->cxt);
#else
    mrb_raisef(mrb, E_LOAD_ERROR, "cannot load Ruby source without mruby-compiler -- %v", r->path);
#endif
  }
  if (mrb->exc) {
    struct RObject *exc = mrb->exc;

    mrb->exc = NULL;
    mrb_exc_raise(mrb, mrb_obj_value(exc));
  }
  return result;
}

/* `content` runs as the file `path`.  Nothing on the caller's stack is safe
   to read afterwards: the file's registers overlay the frame of the C
   method that called this. */
static mrb_value
vfs_run(mrb_state *mrb, mrb_value path, mrb_value content)
{
  struct vfs_run r;
  mrb_value result;

  r.path = path;
  r.content = content;
#ifdef MRB_VFS_HAVE_COMPILER
  r.cxt = NULL;
#endif
  MRB_ENSURE(mrb, result, vfs_run_body, &r) {
#ifdef MRB_VFS_HAVE_COMPILER
    if (r.cxt) {
      mrb_ccontext_free(mrb, r.cxt);
      r.cxt = NULL;
    }
#endif
  }
  return result;
}

static mrb_value
vfs_run_path(mrb_state *mrb, mrb_value path)
{
  mrb_value content = mrb_vfs_read_str(mrb, path);

  if (mrb_nil_p(content)) {
    vfs_raise_load_error(mrb, path);
  }
  return vfs_run(mrb, path, content);
}

/*
 * Loading a feature
 *
 * `path` is where the search ended.  A path in $LOADED_FEATURES has been
 * loaded and is not loaded again; one being loaded right now is a circular
 * require, answered false as well.  A file that raises is not recorded, so
 * a later require tries it again.
 */
static mrb_value
vfs_feature_body(mrb_state *mrb, void *ptr)
{
  return vfs_run_path(mrb, *(mrb_value*)ptr);
}

static mrb_bool
vfs_load_feature(mrb_state *mrb, mrb_value path)
{
  mrb_value features = vfs_loaded_features(mrb);
  mrb_value loading = vfs_loading(mrb);
  mrb_value result;

  if (vfs_ary_index(mrb, features, path) >= 0) return FALSE;
  if (vfs_ary_index(mrb, loading, path) >= 0) return FALSE;
  mrb_ary_push(mrb, loading, path);
  MRB_ENSURE(mrb, result, vfs_feature_body, &path) {
    vfs_ary_delete(mrb, loading, path);
  }
  mrb_ary_push(mrb, features, path);
  return TRUE;
}

/* Where `name` is found: as it is when it says where it starts, else under
   each $LOAD_PATH entry in turn.  Raises LoadError when nowhere. */
static mrb_value
vfs_find_feature(mrb_state *mrb, mrb_value name)
{
  const char *s = RSTRING_PTR(name);
  mrb_int len = RSTRING_LEN(name);
  mrb_value found = mrb_nil_value();

  if (len == 0) {
    vfs_raise_load_error(mrb, name);
  }
  if (vfs_explicit_p(s, len)) {
    found = vfs_probe(mrb, mrb_nil_value(), s, len);
  }
  else {
    mrb_value load_path = vfs_load_path(mrb);

    for (mrb_int i = 0; i < RARRAY_LEN(load_path); i++) {
      mrb_value dir = RARRAY_PTR(load_path)[i];

      if (!mrb_string_p(dir)) {
        mrb_raisef(mrb, E_TYPE_ERROR, "$LOAD_PATH holds %Y (expected a String)", dir);
      }
      found = vfs_probe(mrb, dir, s, len);
      if (!mrb_nil_p(found)) break;
    }
  }
  if (mrb_nil_p(found)) {
    vfs_raise_load_error(mrb, name);
  }
  return found;
}

/* The file `name` names, without adding an extension: as it is, and when it
   does not say where it starts, under $LOAD_PATH as well */
static mrb_value
vfs_find_file(mrb_state *mrb, mrb_value name)
{
  const char *s = RSTRING_PTR(name);
  mrb_int len = RSTRING_LEN(name);
  int ai = mrb_gc_arena_save(mrb);

  if (len > 0) {
    mrb_value path = vfs_clean_path(mrb, name);

    if (vfs_file_p(mrb, path)) return path;
    mrb_gc_arena_restore(mrb, ai);
  }
  if (len > 0 && !vfs_explicit_p(s, len)) {
    mrb_value load_path = vfs_load_path(mrb);

    for (mrb_int i = 0; i < RARRAY_LEN(load_path); i++) {
      mrb_value dir = RARRAY_PTR(load_path)[i];
      mrb_value path;

      if (!mrb_string_p(dir)) {
        mrb_raisef(mrb, E_TYPE_ERROR, "$LOAD_PATH holds %Y (expected a String)", dir);
      }
      path = vfs_clean_path(mrb, vfs_join(mrb, dir, s, len));
      if (vfs_file_p(mrb, path)) {
        mrb_gc_arena_restore(mrb, ai);
        mrb_gc_protect(mrb, path);
        return path;
      }
      mrb_gc_arena_restore(mrb, ai);
    }
  }
  vfs_raise_load_error(mrb, name);
  return mrb_nil_value(); /* not reached */
}

/* The file of the Ruby code that called the current C method: NULL when
   there is none, or it was compiled without debug information */
static const char*
vfs_caller_file(mrb_state *mrb)
{
  struct mrb_context *c = mrb->c;
  mrb_callinfo *ci;

  if (!c->cibase || c->ci == c->cibase) return NULL;
  for (ci = c->ci - 1; ci >= c->cibase; ci--) {
    const struct RProc *proc = ci->proc;
    const mrb_irep *irep;

    if (!proc || MRB_PROC_CFUNC_P(proc)) continue;
    irep = proc->body.irep;
    if (!irep || !irep->debug_info || !ci->pc) return NULL;
    return mrb_debug_get_filename(mrb, irep, (uint32_t)(ci->pc - irep->iseq - 1));
  }
  return NULL;
}

MRB_API mrb_bool
mrb_vfs_require(mrb_state *mrb, const char *feature)
{
  int ai = mrb_gc_arena_save(mrb);
  mrb_value path = vfs_find_feature(mrb, mrb_str_new_cstr(mrb, feature));
  mrb_bool loaded = vfs_load_feature(mrb, path);

  mrb_gc_arena_restore(mrb, ai);
  return loaded;
}

MRB_API mrb_value
mrb_vfs_load(mrb_state *mrb, const char *path)
{
  int ai = mrb_gc_arena_save(mrb);
  mrb_value result = vfs_run_path(mrb, mrb_str_new_cstr(mrb, path));

  mrb_gc_arena_restore(mrb, ai);
  mrb_gc_protect(mrb, result);
  return result;
}

MRB_API void
mrb_vfs_load_path_push(mrb_state *mrb, const char *dir)
{
  mrb_ary_push(mrb, vfs_load_path(mrb), mrb_str_new_cstr(mrb, dir));
}

/*
 * call-seq:
 *   require(name) -> true or false
 *
 * Loads the feature name once.  A name that starts with "/", "./" or "../"
 * is taken as it is; any other is looked for under each directory in
 * $LOAD_PATH in turn.  Without an extension, name.rb is tried before
 * name.mrb.  The path it resolved to is recorded in $LOADED_FEATURES, and a
 * name that resolves to a recorded path is not loaded again: the method
 * answers true the first time and false after that.  Raises LoadError when
 * nothing is found.
 *
 *   $LOAD_PATH << "/app/lib"
 *   require "greeting"     #=> true, ran /app/lib/greeting.rb
 *   require "greeting"     #=> false
 */
static mrb_value
kernel_require(mrb_state *mrb, mrb_value self)
{
  mrb_value name, path;
  mrb_bool loaded;
  int ai = mrb_gc_arena_save(mrb);

  mrb_get_args(mrb, "S", &name);
  path = vfs_find_feature(mrb, name);
  loaded = vfs_load_feature(mrb, path);
  mrb_gc_arena_restore(mrb, ai);
  return mrb_bool_value(loaded);
}

/*
 * call-seq:
 *   require_relative(name) -> true or false
 *
 * Like require, with name taken relative to the directory of the file the
 * call is written in rather than to $LOAD_PATH; an absolute name is taken as
 * it is.  Raises LoadError when the calling file is not known, as it is not
 * for code given to eval without a file name.
 *
 *   # in /app/lib/greeting.rb
 *   require_relative "greeting/words"   # loads /app/lib/greeting/words.rb
 */
static mrb_value
kernel_require_relative(mrb_state *mrb, mrb_value self)
{
  mrb_value name, base, path;
  const char *file;
  mrb_bool loaded;
  int ai = mrb_gc_arena_save(mrb);

  mrb_get_args(mrb, "S", &name);
  file = vfs_caller_file(mrb);
  if (file == NULL) {
    mrb_raise(mrb, E_LOAD_ERROR, "cannot infer basepath");
  }
  if (vfs_absolute_p(RSTRING_PTR(name), RSTRING_LEN(name))) {
    base = name;
  }
  else {
    base = vfs_join(mrb, vfs_dirname(mrb, file), RSTRING_PTR(name), RSTRING_LEN(name));
  }
  path = vfs_probe(mrb, mrb_nil_value(), RSTRING_PTR(base), RSTRING_LEN(base));
  if (mrb_nil_p(path)) {
    vfs_raise_load_error(mrb, base);
  }
  loaded = vfs_load_feature(mrb, path);
  mrb_gc_arena_restore(mrb, ai);
  return mrb_bool_value(loaded);
}

/*
 * call-seq:
 *   load(path) -> true
 *
 * Runs the file at path, every time it is called, and records nothing in
 * $LOADED_FEATURES.  The path is taken as it is, with no extension added;
 * one that does not start with "/", "./" or "../" and is not found as it is
 * is looked for under $LOAD_PATH.  Raises LoadError when nothing is found.
 * The wrap argument of CRuby is not supported: a true value raises
 * NotImplementedError.
 *
 *   load "/app/config.rb"   #=> true
 */
static mrb_value
kernel_load(mrb_state *mrb, mrb_value self)
{
  mrb_value name, path;
  mrb_value wrap = mrb_false_value();
  int ai = mrb_gc_arena_save(mrb);

  mrb_get_args(mrb, "S|o", &name, &wrap);
  if (mrb_test(wrap)) {
    mrb_raise(mrb, E_NOTIMP_ERROR, "load with a wrap module is not supported");
  }
  path = vfs_find_file(mrb, name);
  vfs_run_path(mrb, path);
  mrb_gc_arena_restore(mrb, ai);
  return mrb_true_value();
}

/*
 * call-seq:
 *   __dir__ -> string or nil
 *
 * The directory of the file the call is written in, as the file was named
 * when it was loaded: "." for a file loaded by a bare name.  Nil when the
 * file is not known.
 *
 *   # in /app/lib/greeting.rb
 *   __dir__   #=> "/app/lib"
 */
static mrb_value
kernel_dir(mrb_state *mrb, mrb_value self)
{
  const char *file = vfs_caller_file(mrb);

  if (file == NULL) return mrb_nil_value();
  return vfs_dirname(mrb, file);
}

void
mrb_vfs_init_require(mrb_state *mrb, struct RClass *vfs)
{
  struct RClass *kernel = mrb->kernel_module;
  mrb_value load_path = mrb_ary_new(mrb);
  mrb_value features = mrb_ary_new(mrb);

  mrb_define_class_id(mrb, MRB_SYM(LoadError), E_SCRIPT_ERROR);

  mrb_iv_set(mrb, mrb_obj_value(vfs), MRB_IVSYM(loading), mrb_ary_new(mrb));
  /* $: and $" are the same Arrays under a second name, not aliases of the
     variables: assigning $LOAD_PATH a new Array leaves $: with the old one */
  mrb_gv_set(mrb, MRB_GVSYM(LOAD_PATH), load_path);
  mrb_gv_set(mrb, mrb_intern_lit(mrb, "$:"), load_path);
  mrb_gv_set(mrb, MRB_GVSYM(LOADED_FEATURES), features);
  mrb_gv_set(mrb, mrb_intern_lit(mrb, "$\""), features);

  mrb_define_module_function_id(mrb, kernel, MRB_SYM(require), kernel_require, MRB_ARGS_REQ(1));
  mrb_define_module_function_id(mrb, kernel, MRB_SYM(require_relative), kernel_require_relative, MRB_ARGS_REQ(1));
  mrb_define_module_function_id(mrb, kernel, MRB_SYM(load), kernel_load, MRB_ARGS_ARG(1, 1));
  mrb_define_module_function_id(mrb, kernel, MRB_SYM(__dir__), kernel_dir, MRB_ARGS_NONE());
}
