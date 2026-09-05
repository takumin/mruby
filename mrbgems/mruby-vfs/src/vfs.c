/*
** vfs.c - VFS module: the mount table, backends, and the host's files
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/vfs.h>
#include "vfs_hal.h"

#include <string.h>
#include <errno.h>

/* provided by require.c */
void mrb_vfs_init_require(mrb_state *mrb, struct RClass *vfs);

/* A backend written in C: VFS::Backend wraps one of these */
struct vfs_backend {
  const mrb_vfs_ops *ops;
  void *data;
};

static void
vfs_backend_free(mrb_state *mrb, void *ptr)
{
  struct vfs_backend *b = (struct vfs_backend*)ptr;

  if (b == NULL) return;
  if (b->ops && b->ops->dfree) {
    b->ops->dfree(mrb, b->data);
  }
  mrb_free(mrb, b);
}

static const struct mrb_data_type vfs_backend_type = { "VFS::Backend", vfs_backend_free };

static struct RClass*
vfs_module(mrb_state *mrb)
{
  return mrb_module_get_id(mrb, MRB_SYM(VFS));
}

static mrb_value
vfs_kind_to_sym(mrb_state *mrb, enum mrb_vfs_kind kind)
{
  switch (kind) {
  case MRB_VFS_FILE:      return mrb_symbol_value(MRB_SYM(file));
  case MRB_VFS_DIRECTORY: return mrb_symbol_value(MRB_SYM(directory));
  case MRB_VFS_OTHER:     return mrb_symbol_value(MRB_SYM(other));
  default:                return mrb_nil_value();
  }
}

/* What a backend written in Ruby answered to `stat` */
static enum mrb_vfs_kind
vfs_sym_to_kind(mrb_state *mrb, mrb_value v)
{
  if (mrb_nil_p(v)) return MRB_VFS_NONE;
  if (mrb_symbol_p(v)) {
    mrb_sym s = mrb_symbol(v);
    if (s == MRB_SYM(file)) return MRB_VFS_FILE;
    if (s == MRB_SYM(directory)) return MRB_VFS_DIRECTORY;
    if (s == MRB_SYM(other)) return MRB_VFS_OTHER;
  }
  mrb_raisef(mrb, E_TYPE_ERROR, "VFS backend answered %!v to stat (expected :file, :directory, :other or nil)", v);
  return MRB_VFS_NONE; /* not reached */
}

/*
 * The mount table
 *
 * `@mounts` on the VFS module: [prefix, backend] pairs, longest prefix first,
 * with the root ("/") last.  mrblib/vfs.rb keeps it in that order; this side
 * only reads it, and checks what it reads, since the table is Ruby's to edit.
 */
static mrb_value
vfs_mounts(mrb_state *mrb)
{
  mrb_value mounts = mrb_iv_get(mrb, mrb_obj_value(vfs_module(mrb)), MRB_IVSYM(mounts));

  if (!mrb_array_p(mounts)) {
    mrb_raise(mrb, E_TYPE_ERROR, "VFS mount table is not an Array");
  }
  return mounts;
}

/*
 * The backend mounted over `path`, and the path that backend sees: the whole
 * path under the root mount, and under any other the part after the prefix,
 * starting at the slash.  Nil when no mount covers the path.
 */
static mrb_value
vfs_lookup(mrb_state *mrb, mrb_value path, mrb_value *rest)
{
  mrb_value mounts = vfs_mounts(mrb);
  const char *p = RSTRING_PTR(path);
  mrb_int plen = RSTRING_LEN(path);

  for (mrb_int i = 0; i < RARRAY_LEN(mounts); i++) {
    mrb_value pair = RARRAY_PTR(mounts)[i];
    const char *m;
    mrb_int mlen;

    if (!mrb_array_p(pair) || RARRAY_LEN(pair) != 2 || !mrb_string_p(RARRAY_PTR(pair)[0])) {
      mrb_raise(mrb, E_TYPE_ERROR, "VFS mount table entry is not a [prefix, backend] pair");
    }
    m = RSTRING_PTR(RARRAY_PTR(pair)[0]);
    mlen = RSTRING_LEN(RARRAY_PTR(pair)[0]);
    if (mlen == 1 && m[0] == '/') {
      /* the root covers everything */
      *rest = path;
      return RARRAY_PTR(pair)[1];
    }
    if (plen >= mlen && memcmp(p, m, (size_t)mlen) == 0 && (plen == mlen || p[mlen] == '/')) {
      *rest = (plen == mlen) ? mrb_str_new_lit(mrb, "/") : mrb_str_new(mrb, p + mlen, plen - mlen);
      return RARRAY_PTR(pair)[1];
    }
  }
  return mrb_nil_value();
}

/* The C side of `obj`, when it is a VFS::Backend; NULL for a backend
   written in Ruby */
static struct vfs_backend*
vfs_native_backend(mrb_value obj)
{
  if (mrb_data_p(obj) && DATA_TYPE(obj) == &vfs_backend_type) {
    return (struct vfs_backend*)DATA_PTR(obj);
  }
  return NULL;
}

static const mrb_vfs_ops*
vfs_backend_ops(mrb_state *mrb, struct vfs_backend *b)
{
  if (b == NULL || b->ops == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "uninitialized VFS backend");
  }
  return b->ops;
}

MRB_API int
mrb_vfs_stat_str(mrb_state *mrb, mrb_value path, enum mrb_vfs_kind *kind)
{
  mrb_value rest;
  mrb_value backend = vfs_lookup(mrb, path, &rest);
  struct vfs_backend *b;

  if (mrb_nil_p(backend)) {
    *kind = MRB_VFS_NONE;
    return 0;
  }
  b = vfs_native_backend(backend);
  if (b) {
    const mrb_vfs_ops *ops = vfs_backend_ops(mrb, b);
    return ops->stat(mrb, b->data, RSTRING_CSTR(mrb, rest), kind);
  }
  *kind = vfs_sym_to_kind(mrb, mrb_funcall_id(mrb, backend, MRB_SYM(stat), 1, rest));
  return 0;
}

MRB_API int
mrb_vfs_stat(mrb_state *mrb, const char *path, enum mrb_vfs_kind *kind)
{
  return mrb_vfs_stat_str(mrb, mrb_str_new_cstr(mrb, path), kind);
}

MRB_API mrb_value
mrb_vfs_read_str(mrb_state *mrb, mrb_value path)
{
  mrb_value rest;
  mrb_value backend = vfs_lookup(mrb, path, &rest);
  struct vfs_backend *b;
  mrb_value content;

  if (mrb_nil_p(backend)) return mrb_nil_value();
  b = vfs_native_backend(backend);
  if (b) {
    const mrb_vfs_ops *ops = vfs_backend_ops(mrb, b);
    return ops->read(mrb, b->data, RSTRING_CSTR(mrb, rest));
  }
  content = mrb_funcall_id(mrb, backend, MRB_SYM(read), 1, rest);
  if (!mrb_nil_p(content) && !mrb_string_p(content)) {
    mrb_raisef(mrb, E_TYPE_ERROR, "VFS backend answered %T to read (expected a String or nil)", content);
  }
  return content;
}

MRB_API mrb_value
mrb_vfs_read(mrb_state *mrb, const char *path)
{
  return mrb_vfs_read_str(mrb, mrb_str_new_cstr(mrb, path));
}

MRB_API mrb_value
mrb_vfs_backend_new(mrb_state *mrb, const mrb_vfs_ops *ops, void *data)
{
  struct RClass *klass = mrb_class_get_under_id(mrb, vfs_module(mrb), MRB_SYM(Backend));
  struct RData *obj;
  struct vfs_backend *b;

  if (ops == NULL || ops->stat == NULL || ops->read == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "VFS backend operations need stat and read");
  }
  obj = mrb_data_object_alloc(mrb, klass, NULL, &vfs_backend_type);
  b = (struct vfs_backend*)mrb_malloc(mrb, sizeof(struct vfs_backend));
  b->ops = ops;
  b->data = data;
  obj->data = b;
  return mrb_obj_value(obj);
}

MRB_API mrb_value
mrb_vfs_mount(mrb_state *mrb, const char *prefix, mrb_value backend)
{
  return mrb_funcall_id(mrb, mrb_obj_value(vfs_module(mrb)), MRB_SYM(mount), 2,
                        mrb_str_new_cstr(mrb, prefix), backend);
}

MRB_API mrb_value
mrb_vfs_umount(mrb_state *mrb, const char *prefix)
{
  return mrb_funcall_id(mrb, mrb_obj_value(vfs_module(mrb)), MRB_SYM(umount), 1,
                        mrb_str_new_cstr(mrb, prefix));
}

/*
 * call-seq:
 *   VFS.stat(path) -> :file, :directory, :other or nil
 *
 * What path names, asked of the mount that covers it: nil when there is no
 * such entry or no mount covers the path.  Raises a SystemCallError when the
 * backend could not answer.
 *
 *   VFS.stat("/etc/hosts")   #=> :file
 *   VFS.stat("/etc")         #=> :directory
 *   VFS.stat("/no/such")     #=> nil
 */
static mrb_value
vfs_s_stat(mrb_state *mrb, mrb_value self)
{
  mrb_value path;
  enum mrb_vfs_kind kind;

  mrb_get_args(mrb, "S", &path);
  if (mrb_vfs_stat_str(mrb, path, &kind) == -1) {
    mrb_sys_fail(mrb, RSTRING_CSTR(mrb, path));
  }
  return vfs_kind_to_sym(mrb, kind);
}

/*
 * call-seq:
 *   VFS.read(path) -> string
 *
 * The whole content of the regular file at path.  Raises Errno::ENOENT when
 * there is no such file, and whatever the backend raises for one it cannot
 * read.
 *
 *   VFS.read("/etc/hostname")   #=> "example\n"
 */
static mrb_value
vfs_s_read(mrb_state *mrb, mrb_value self)
{
  mrb_value path, content;

  mrb_get_args(mrb, "S", &path);
  content = mrb_vfs_read_str(mrb, path);
  if (mrb_nil_p(content)) {
    errno = ENOENT;
    mrb_sys_fail(mrb, RSTRING_CSTR(mrb, path));
  }
  return content;
}

/*
 * VFS::Backend
 *
 * A backend written in C, made by mrb_vfs_backend_new(): the class is what
 * `VFS::Host` derives from, and what a `stat` or `read` from Ruby reaches
 * the C operations through.
 */
static mrb_value
vfs_backend_init(mrb_state *mrb, mrb_value self)
{
  mrb_raise(mrb, E_NOTIMP_ERROR, "VFS::Backend is made from C; mount a VFS::Host, a VFS::Memory or an object with stat and read");
  return self; /* not reached */
}

static struct vfs_backend*
vfs_get_backend(mrb_state *mrb, mrb_value self)
{
  struct vfs_backend *b = (struct vfs_backend*)mrb_data_get_ptr(mrb, self, &vfs_backend_type);

  vfs_backend_ops(mrb, b);
  return b;
}

/*
 * call-seq:
 *   backend.stat(path) -> :file, :directory, :other or nil
 *
 * What path names in this backend alone, the path taken as the backend sees
 * it: no mount table is consulted.
 */
static mrb_value
vfs_backend_stat(mrb_state *mrb, mrb_value self)
{
  const char *path;
  enum mrb_vfs_kind kind;
  struct vfs_backend *b;

  mrb_get_args(mrb, "z", &path);
  b = vfs_get_backend(mrb, self);
  if (b->ops->stat(mrb, b->data, path, &kind) == -1) {
    mrb_sys_fail(mrb, path);
  }
  return vfs_kind_to_sym(mrb, kind);
}

/*
 * call-seq:
 *   backend.read(path) -> string or nil
 *
 * The content of the regular file at path in this backend alone, or nil when
 * there is no such entry.
 */
static mrb_value
vfs_backend_read(mrb_state *mrb, mrb_value self)
{
  const char *path;
  struct vfs_backend *b;

  mrb_get_args(mrb, "z", &path);
  b = vfs_get_backend(mrb, self);
  return b->ops->read(mrb, b->data, path);
}

#ifdef MRB_HAL_VFS_HAS_HOST
/*
 * VFS::Host: the port's own files, through the HAL
 */
static int
host_stat(mrb_state *mrb, void *data, const char *path, enum mrb_vfs_kind *kind)
{
  mrb_int size;
  (void)data;

  return mrb_hal_vfs_stat(mrb, path, kind, &size);
}

struct host_read {
  mrb_vfs_hal_file *file;
  const char *path;
  mrb_value str;
  mrb_int size;         /* what stat said, -1 when it did not say */
};

static mrb_value
host_read_body(mrb_state *mrb, void *ptr)
{
  struct host_read *r = (struct host_read*)ptr;
  /* Room for the whole file and the read that reports its end, so a file
     whose size stat knew is read without the buffer ever growing. */
  mrb_int capa = (r->size >= 0) ? r->size + 1 : 4096;
  mrb_int len = 0;

  mrb_str_resize(mrb, r->str, capa);
  for (;;) {
    mrb_int n;

    if (len == capa) {
      capa *= 2;
      mrb_str_resize(mrb, r->str, capa);
    }
    n = mrb_hal_vfs_read(mrb, r->file, RSTRING_PTR(r->str) + len, (size_t)(capa - len));
    if (n < 0) {
      mrb_sys_fail(mrb, r->path);
    }
    if (n == 0) break;
    len += n;
  }
  mrb_str_resize(mrb, r->str, len);
  return r->str;
}

static mrb_value
host_read(mrb_state *mrb, void *data, const char *path)
{
  enum mrb_vfs_kind kind;
  struct host_read r;
  mrb_value result;
  (void)data;

  if (mrb_hal_vfs_stat(mrb, path, &kind, &r.size) == -1) {
    mrb_sys_fail(mrb, path);
  }
  if (kind == MRB_VFS_NONE) return mrb_nil_value();
  if (kind == MRB_VFS_DIRECTORY) {
    errno = EISDIR;
    mrb_sys_fail(mrb, path);
  }
  r.file = mrb_hal_vfs_open(mrb, path);
  if (r.file == NULL) {
    /* gone between the stat and the open */
    if (errno == ENOENT || errno == ENOTDIR) return mrb_nil_value();
    mrb_sys_fail(mrb, path);
  }
  r.path = path;
  r.str = mrb_str_new_capa(mrb, 0);
  MRB_ENSURE(mrb, result, host_read_body, &r) {
    int saved_errno = errno;
    mrb_hal_vfs_close(mrb, r.file);
    errno = saved_errno;
  }
  return result;
}

static const mrb_vfs_ops host_ops = { host_stat, host_read, NULL };

/*
 * call-seq:
 *   VFS::Host.new -> host
 *
 * A backend over the files of the machine mruby runs on.  One is mounted at
 * the root when the gem initializes; another is only ever needed to mount
 * the host again somewhere after the root was given to something else.
 */
static mrb_value
vfs_host_init(mrb_state *mrb, mrb_value self)
{
  struct vfs_backend *b = (struct vfs_backend*)DATA_PTR(self);

  if (b) {
    vfs_backend_free(mrb, b);
  }
  DATA_TYPE(self) = &vfs_backend_type;
  DATA_PTR(self) = NULL;

  b = (struct vfs_backend*)mrb_malloc(mrb, sizeof(struct vfs_backend));
  b->ops = &host_ops;
  b->data = NULL;
  DATA_PTR(self) = b;
  return self;
}
#endif /* MRB_HAL_VFS_HAS_HOST */

void
mrb_mruby_vfs_gem_init(mrb_state *mrb)
{
  struct RClass *vfs = mrb_define_module_id(mrb, MRB_SYM(VFS));
  struct RClass *backend;
#ifdef MRB_HAL_VFS_HAS_HOST
  struct RClass *host;
#endif

  mrb_iv_set(mrb, mrb_obj_value(vfs), MRB_IVSYM(mounts), mrb_ary_new(mrb));

  mrb_define_module_function_id(mrb, vfs, MRB_SYM(stat), vfs_s_stat, MRB_ARGS_REQ(1));
  mrb_define_module_function_id(mrb, vfs, MRB_SYM(read), vfs_s_read, MRB_ARGS_REQ(1));

  backend = mrb_define_class_under_id(mrb, vfs, MRB_SYM(Backend), mrb->object_class);
  MRB_SET_INSTANCE_TT(backend, MRB_TT_DATA);
  mrb_define_method_id(mrb, backend, MRB_SYM(initialize), vfs_backend_init, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, backend, MRB_SYM(stat), vfs_backend_stat, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, backend, MRB_SYM(read), vfs_backend_read, MRB_ARGS_REQ(1));

#ifdef MRB_HAL_VFS_HAS_HOST
  mrb_hal_vfs_init(mrb);
  host = mrb_define_class_under_id(mrb, vfs, MRB_SYM(Host), backend);
  mrb_define_method_id(mrb, host, MRB_SYM(initialize), vfs_host_init, MRB_ARGS_NONE());
#endif

  mrb_vfs_init_require(mrb, vfs);
}

void
mrb_mruby_vfs_gem_final(mrb_state *mrb)
{
#ifdef MRB_HAL_VFS_HAS_HOST
  mrb_hal_vfs_final(mrb);
#else
  (void)mrb;
#endif
}
