/*
** vfstest.c - files on the host for the mruby-vfs tests
**
** A sandbox directory with a few files in it, for the tests of the port's
** host backend; the rest of the tests read from a VFS::Memory and touch no
** file at all.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <mruby.h>
#include <mruby/array.h>
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

#if defined(_WIN32)
# include <io.h>
# include <direct.h>
# define vfstest_mkdir(path) _mkdir(path)
# define vfstest_rmdir(path) _rmdir(path)
#else
# include <sys/types.h>
# include <sys/stat.h>
# include <unistd.h>
# define vfstest_mkdir(path) mkdir(path, 0700)
# define vfstest_rmdir(path) rmdir(path)
#endif

struct vfstest_file {
  const char *name;
  const char *content;
};

/* the sandbox's files; a name with a slash names a subdirectory made first */
static const struct vfstest_file vfstest_files[] = {
  { "lib/vfs_host_hello.rb", "def vfs_host_hello\n  \"hello from the host\"\nend\n" },
  { "lib/vfs_host_relative.rb", "require_relative \"vfs_host_hello\"\n$vfs_host_relative_dir = __dir__\n" },
  { "plain.txt", "plain text\n" },
  { NULL, NULL }
};

static const char *const vfstest_dirs[] = { "lib", NULL };

static void
write_file(mrb_state *mrb, const char *path, const char *content)
{
  FILE *fp = fopen(path, "wb");

  if (fp == NULL) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "fopen(%s) failed", path);
  }
  fputs(content, fp);
  fclose(fp);
}

static mrb_value
join(mrb_state *mrb, mrb_value dir, const char *name)
{
  mrb_value path = mrb_str_dup(mrb, dir);

  mrb_str_cat_lit(mrb, path, "/");
  mrb_str_cat_cstr(mrb, path, name);
  return path;
}

/*
 * VFSTest.setup -> sandbox path
 *
 * Makes the sandbox under the temporary directory and fills it.
 */
/* The temporary directory with no separator at its end: macOS sets TMPDIR
   to one with a slash, and the tests compare paths built on this one with
   what the loader recorded after folding the doubled slash away. */
static size_t
tmpdir_len(const char *tmp)
{
  size_t len = strlen(tmp);

  while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) len--;
  return len;
}

static mrb_value
vfstest_setup(mrb_state *mrb, mrb_value klass)
{
  char buf[1024];
  mrb_value sandbox;

#if defined(_WIN32)
  {
    const char *tmp = getenv("TEMP");
    if (tmp == NULL || *tmp == '\0') tmp = ".";
    snprintf(buf, sizeof(buf), "%.*s\\mruby-vfs-test.XXXXXX", (int)tmpdir_len(tmp), tmp);
    if (_mktemp(buf) == NULL || vfstest_mkdir(buf) != 0) {
      mrb_raisef(mrb, E_RUNTIME_ERROR, "mkdir(%s) failed", buf);
    }
  }
#else
  {
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || *tmp == '\0') tmp = "/tmp";
    snprintf(buf, sizeof(buf), "%.*s/mruby-vfs-test.XXXXXX", (int)tmpdir_len(tmp), tmp);
    if (mkdtemp(buf) == NULL) {
      mrb_raisef(mrb, E_RUNTIME_ERROR, "mkdtemp(%s) failed", buf);
    }
  }
#endif
  sandbox = mrb_str_new_cstr(mrb, buf);
  mrb_cv_set(mrb, klass, mrb_intern_lit(mrb, "sandbox"), sandbox);

  for (int i = 0; vfstest_dirs[i]; i++) {
    mrb_value dir = join(mrb, sandbox, vfstest_dirs[i]);
    if (vfstest_mkdir(RSTRING_CSTR(mrb, dir)) != 0) {
      mrb_raisef(mrb, E_RUNTIME_ERROR, "mkdir(%v) failed", dir);
    }
  }
  for (int i = 0; vfstest_files[i].name; i++) {
    mrb_value path = join(mrb, sandbox, vfstest_files[i].name);
    write_file(mrb, RSTRING_CSTR(mrb, path), vfstest_files[i].content);
  }
  return sandbox;
}

/*
 * VFSTest.teardown -> true
 *
 * Removes the sandbox and what setup put in it.
 */
static mrb_value
vfstest_teardown(mrb_state *mrb, mrb_value klass)
{
  mrb_value sandbox = mrb_cv_get(mrb, klass, mrb_intern_lit(mrb, "sandbox"));

  for (int i = 0; vfstest_files[i].name; i++) {
    mrb_value path = join(mrb, sandbox, vfstest_files[i].name);
    remove(RSTRING_CSTR(mrb, path));
  }
  for (int i = 0; vfstest_dirs[i]; i++) {
    mrb_value dir = join(mrb, sandbox, vfstest_dirs[i]);
    vfstest_rmdir(RSTRING_CSTR(mrb, dir));
  }
  if (vfstest_rmdir(RSTRING_CSTR(mrb, sandbox)) != 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "rmdir(%v) failed", sandbox);
  }
  return mrb_true_value();
}

/*
 * VFSTest.sandbox -> sandbox path
 */
static mrb_value
vfstest_sandbox(mrb_state *mrb, mrb_value klass)
{
  return mrb_cv_get(mrb, klass, mrb_intern_lit(mrb, "sandbox"));
}

/*
 * VFSTest.compile(source) -> RITE bytecode as a String, or nil
 *
 * What mrbc would write for source, for the tests of loading bytecode; nil
 * in a build without the compiler.
 */
static mrb_value
vfstest_compile(mrb_state *mrb, mrb_value klass)
{
#ifdef MRB_VFS_HAVE_COMPILER
  const char *source;
  mrb_ccontext *cxt;
  mrb_value proc;
  uint8_t *bin = NULL;
  size_t bin_size = 0;
  int result;

  mrb_get_args(mrb, "z", &source);
  cxt = mrb_ccontext_new(mrb);
  cxt->no_exec = TRUE;
  proc = mrb_load_string_cxt(mrb, source, cxt);
  mrb_ccontext_free(mrb, cxt);
  if (mrb->exc) {
    mrb_exc_raise(mrb, mrb_obj_value(mrb->exc));
  }
  if (!mrb_proc_p(proc)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "compile did not answer a proc");
  }
  result = mrb_dump_irep(mrb, mrb_proc_ptr(proc)->body.irep, MRB_DUMP_DEBUG_INFO, &bin, &bin_size);
  if (result != MRB_DUMP_OK) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "irep dump failed");
  }
  mrb_value str = mrb_str_new(mrb, (const char*)bin, (mrb_int)bin_size);
  mrb_free(mrb, bin);
  return str;
#else
  const char *source;

  mrb_get_args(mrb, "z", &source);
  return mrb_nil_value();
#endif
}

/*
 * The C API, driven from Ruby
 *
 * A backend over a table compiled into this file, the way a firmware would
 * hold its scripts, mounted and asked through <mruby/vfs.h>.
 */
struct table_entry {
  const char *path;
  const char *content;
};

static const struct table_entry table_entries[] = {
  { "/vfs_table_hello.rb", "$vfs_table_hello = [__FILE__, __dir__]\n" },
  { "/sub/vfs_table_deep.rb", "$vfs_table_deep = require_relative('../vfs_table_hello')\n" },
  { "/plain.txt", "table text\n" },
  { NULL, NULL }
};

static const struct table_entry*
table_find(const struct table_entry *entries, const char *path)
{
  for (int i = 0; entries[i].path; i++) {
    if (strcmp(entries[i].path, path) == 0) return &entries[i];
  }
  return NULL;
}

static int
table_stat(mrb_state *mrb, void *data, const char *path, enum mrb_vfs_kind *kind)
{
  const struct table_entry *entries = (const struct table_entry*)data;
  size_t len = strlen(path);

  if (table_find(entries, path)) {
    *kind = MRB_VFS_FILE;
    return 0;
  }
  if (len == 1 && path[0] == '/') {
    *kind = MRB_VFS_DIRECTORY;
    return 0;
  }
  for (int i = 0; entries[i].path; i++) {
    if (strncmp(entries[i].path, path, len) == 0 && entries[i].path[len] == '/') {
      *kind = MRB_VFS_DIRECTORY;
      return 0;
    }
  }
  *kind = MRB_VFS_NONE;
  return 0;
}

static mrb_value
table_read(mrb_state *mrb, void *data, const char *path)
{
  const struct table_entry *e = table_find((const struct table_entry*)data, path);

  return e ? mrb_str_new_static(mrb, e->content, strlen(e->content)) : mrb_nil_value();
}

static void
table_dfree(mrb_state *mrb, void *data)
{
  (void)mrb;
  (void)data;
}

static const mrb_vfs_ops table_ops = { table_stat, table_read, table_dfree };

/* VFSTest.table_backend -> a VFS::Backend over the table */
static mrb_value
vfstest_table_backend(mrb_state *mrb, mrb_value klass)
{
  return mrb_vfs_backend_new(mrb, &table_ops, (void*)table_entries);
}

/* VFSTest.c_mount(prefix, backend), VFSTest.c_umount(prefix) */
static mrb_value
vfstest_c_mount(mrb_state *mrb, mrb_value klass)
{
  const char *prefix;
  mrb_value backend;

  mrb_get_args(mrb, "zo", &prefix, &backend);
  return mrb_vfs_mount(mrb, prefix, backend);
}

static mrb_value
vfstest_c_umount(mrb_state *mrb, mrb_value klass)
{
  const char *prefix;

  mrb_get_args(mrb, "z", &prefix);
  return mrb_vfs_umount(mrb, prefix);
}

/* VFSTest.c_stat(path) -> the mrb_vfs_kind as an Integer, or the errno negated */
static mrb_value
vfstest_c_stat(mrb_state *mrb, mrb_value klass)
{
  const char *path;
  enum mrb_vfs_kind kind;

  mrb_get_args(mrb, "z", &path);
  if (mrb_vfs_stat(mrb, path, &kind) == -1) {
    return mrb_fixnum_value(-errno);
  }
  return mrb_fixnum_value((mrb_int)kind);
}

/* VFSTest.c_read(path) -> String or nil */
static mrb_value
vfstest_c_read(mrb_state *mrb, mrb_value klass)
{
  const char *path;

  mrb_get_args(mrb, "z", &path);
  return mrb_vfs_read(mrb, path);
}

/* VFSTest.c_require(feature) -> true or false */
static mrb_value
vfstest_c_require(mrb_state *mrb, mrb_value klass)
{
  const char *feature;

  mrb_get_args(mrb, "z", &feature);
  return mrb_bool_value(mrb_vfs_require(mrb, feature));
}

/* VFSTest.c_load(path) -> the file's value */
static mrb_value
vfstest_c_load(mrb_state *mrb, mrb_value klass)
{
  const char *path;

  mrb_get_args(mrb, "z", &path);
  return mrb_vfs_load(mrb, path);
}

/* VFSTest.c_load_path_push(dir) */
static mrb_value
vfstest_c_load_path_push(mrb_state *mrb, mrb_value klass)
{
  const char *dir;

  mrb_get_args(mrb, "z", &dir);
  mrb_vfs_load_path_push(mrb, dir);
  return mrb_nil_value();
}

void
mrb_mruby_vfs_gem_test(mrb_state *mrb)
{
  /* test sources are not scanned for presyms, so these go by name */
  struct RClass *c = mrb_define_module(mrb, "VFSTest");

  mrb_define_class_method(mrb, c, "setup", vfstest_setup, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, c, "teardown", vfstest_teardown, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, c, "sandbox", vfstest_sandbox, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, c, "compile", vfstest_compile, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, c, "table_backend", vfstest_table_backend, MRB_ARGS_NONE());
  mrb_define_class_method(mrb, c, "c_mount", vfstest_c_mount, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, c, "c_umount", vfstest_c_umount, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, c, "c_stat", vfstest_c_stat, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, c, "c_read", vfstest_c_read, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, c, "c_require", vfstest_c_require, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, c, "c_load", vfstest_c_load, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, c, "c_load_path_push", vfstest_c_load_path_push, MRB_ARGS_REQ(1));
}
