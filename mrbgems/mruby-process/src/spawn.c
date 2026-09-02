/*
** spawn.c - Process.__spawn
**
** See Copyright Notice in mruby.h
**
** The C half of Process.spawn.  CRuby's argument shape is `[env,]
** command..., [options]`, and taking that apart in C would pile Hash, Array
** and IO case analysis in here; mrblib/process.rb does it instead and hands
** this down flat arrays and a Hash of the options it has already read.
** What is left is the part that has to happen in C: validating the
** strings, marshalling everything into the structs the HAL takes, and
** owning the child the port created, so that the wait it now owes has
** something owing it.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <string.h>

#ifdef MRB_HAL_PROCESS_HAS_SPAWN

/* The arrays the HAL reads.  They are freed together, including on the way
   out of a failure, because mrb_sys_fail() leaves by longjmp and takes any
   chance to free them with it. */
struct spawn_bufs {
  const char **argv;
  mrb_process_env_entry *env;
  mrb_process_redirect *redirects;
};

static void
bufs_free(mrb_state *mrb, struct spawn_bufs *bufs)
{
  mrb_free(mrb, bufs->argv);
  mrb_free(mrb, bufs->env);
  mrb_free(mrb, bufs->redirects);
  memset(bufs, 0, sizeof(*bufs));
}

/* A string the operating system can take: really a String, and without an
   embedded NUL, which mrb_string_value_cstr() is what checks. */
static const char*
value_cstr(mrb_state *mrb, mrb_value v)
{
  if (!mrb_string_p(v)) {
    mrb_raisef(mrb, E_TYPE_ERROR, "no implicit conversion of %Y into String", v);
  }
  return mrb_string_value_cstr(mrb, &v);
}

static const char*
element_cstr(mrb_state *mrb, mrb_value ary, mrb_int idx)
{
  return value_cstr(mrb, mrb_ary_ref(mrb, ary, idx));
}

static mrb_int
element_int(mrb_state *mrb, mrb_value ary, mrb_int idx)
{
  return mrb_as_int(mrb, mrb_ary_ref(mrb, ary, idx));
}

/*
 * The options Hash
 *
 * Every key is one mrblib/process.rb has already recognised, so what is
 * checked here is the value's shape.
 */

static mrb_value
opt_get(mrb_state *mrb, mrb_value opts, mrb_sym key)
{
  return mrb_hash_fetch(mrb, opts, mrb_symbol_value(key), mrb_nil_value());
}

static mrb_bool
opt_bool(mrb_state *mrb, mrb_value opts, mrb_sym key)
{
  mrb_value v = opt_get(mrb, opts, key);

  if (mrb_nil_p(v) || mrb_false_p(v)) return FALSE;
  if (mrb_true_p(v)) return TRUE;
  mrb_raisef(mrb, E_ARGUMENT_ERROR, "expected true or false as %n: %!v", key, v);
  return FALSE; /* not reached */
}

static const char*
opt_cstr(mrb_state *mrb, mrb_value opts, mrb_sym key)
{
  mrb_value v = opt_get(mrb, opts, key);

  if (mrb_nil_p(v)) return NULL;
  return value_cstr(mrb, v);
}

/* What a redirection table entry is, from the Symbol the Ruby side wrote:
   `:parent` duplicates a descriptor of this process's, `:child` one the
   table has set so far, `:close` closes. */
static mrb_process_redirect_kind
redirect_kind(mrb_state *mrb, mrb_value v)
{
  if (mrb_symbol_p(v)) {
    mrb_sym s = mrb_symbol(v);
    if (s == MRB_SYM(parent)) return MRB_PROCESS_REDIR_PARENT;
    if (s == MRB_SYM(child))  return MRB_PROCESS_REDIR_CHILD;
    if (s == MRB_SYM(close))  return MRB_PROCESS_REDIR_CLOSE;
  }
  mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown redirection kind %!v", v);
  return MRB_PROCESS_REDIR_CLOSE; /* not reached */
}

/*
 * call-seq:
 *   Process.__spawn(argv, env, redirects, options) -> pid
 *
 * The primitive Process.spawn is written on.  +argv+ is an Array of
 * Strings, +env+ a [name, value_or_nil, ...] Array, +redirects+ a
 * [child_fd, kind, source_fd, ...] Array read in order, and +options+ a
 * Hash holding what the caller's option Hash was read into: `:shell`,
 * `:chdir`, `:close_others` and `:unsetenv_others`, each present only
 * where the caller gave it.
 */
static mrb_value
process_s___spawn(mrb_state *mrb, mrb_value self)
{
  mrb_value argv_ary, env_ary, table_ary, opts;
  mrb_int argc, envc, tablec, i;
  mrb_process_spawn_params params;
  struct spawn_bufs bufs = { NULL, NULL, NULL };
  mrb_hal_process_child *child;
  mrb_process_table *table;
  mrb_process_record *record;
  int err;

  mrb_get_args(mrb, "AAAH", &argv_ary, &env_ary, &table_ary, &opts);

  argc = RARRAY_LEN(argv_ary);
  envc = RARRAY_LEN(env_ary);
  tablec = RARRAY_LEN(table_ary);

  if (argc < 1) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no command given");
  }
  if (envc % 2 != 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "malformed environment");
  }
  if (tablec % 3 != 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "malformed redirection table");
  }

  /* Everything that can raise happens before anything is allocated: the
     strings are checked here, and read again below once nothing else can go
     wrong between the allocation and the call. */
  for (i = 0; i < argc; i++) {
    element_cstr(mrb, argv_ary, i);
  }
  for (i = 0; i < envc; i += 2) {
    const char *name = element_cstr(mrb, env_ary, i);
    mrb_value value = mrb_ary_ref(mrb, env_ary, i + 1);
    if (strchr(name, '=') != NULL) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "environment name contains '=': %s", name);
    }
    if (!mrb_nil_p(value)) element_cstr(mrb, env_ary, i + 1);
  }
  for (i = 0; i < tablec; i += 3) {
    mrb_process_redirect_kind rkind = redirect_kind(mrb, mrb_ary_ref(mrb, table_ary, i + 1));

    if (element_int(mrb, table_ary, i) < 0) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong exec redirect: negative descriptor");
    }
    /* A port narrows a descriptor to the `int` the platform numbers them
       with, so a number no `int` can hold would arrive there as some other
       descriptor entirely.  Refused here, where the size is what is wrong
       with it, as Ruby refuses it. */
    mrb_process_int_arg(mrb, element_int(mrb, table_ary, i), "descriptor");
    if (rkind != MRB_PROCESS_REDIR_CLOSE) {
      mrb_int source = element_int(mrb, table_ary, i + 2);
      if (source < 0) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong exec redirect: negative descriptor");
      }
      mrb_process_int_arg(mrb, source, "descriptor");
    }
  }

  memset(&params, 0, sizeof(params));
  params.kind = opt_bool(mrb, opts, MRB_SYM(shell)) ? MRB_PROCESS_SPAWN_SHELL
                                                    : MRB_PROCESS_SPAWN_ARGV;
  params.prog = opt_cstr(mrb, opts, MRB_SYM(prog));
  params.chdir = opt_cstr(mrb, opts, MRB_SYM(chdir));
  if (opt_bool(mrb, opts, MRB_SYM(close_others))) params.flags |= MRB_PROCESS_SPAWN_CLOSE_OTHERS;
  if (opt_bool(mrb, opts, MRB_SYM(unsetenv_others))) params.flags |= MRB_PROCESS_SPAWN_UNSETENV_OTHERS;

  bufs.argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (size_t)(argc + 1));
  if (envc > 0) {
    bufs.env = (mrb_process_env_entry*)
      mrb_malloc_simple(mrb, sizeof(mrb_process_env_entry) * (size_t)(envc / 2));
  }
  if (tablec > 0) {
    bufs.redirects = (mrb_process_redirect*)
      mrb_malloc_simple(mrb, sizeof(mrb_process_redirect) * (size_t)(tablec / 3));
  }
  if (bufs.argv == NULL || (envc > 0 && bufs.env == NULL) ||
      (tablec > 0 && bufs.redirects == NULL)) {
    bufs_free(mrb, &bufs);
    mrb_raise(mrb, E_RUNTIME_ERROR, "out of memory");
  }

  for (i = 0; i < argc; i++) {
    bufs.argv[i] = element_cstr(mrb, argv_ary, i);
  }
  bufs.argv[argc] = NULL;
  for (i = 0; i < envc; i += 2) {
    mrb_value value = mrb_ary_ref(mrb, env_ary, i + 1);
    bufs.env[i / 2].name = element_cstr(mrb, env_ary, i);
    bufs.env[i / 2].value = mrb_nil_p(value) ? NULL : element_cstr(mrb, env_ary, i + 1);
  }
  for (i = 0; i < tablec; i += 3) {
    mrb_process_redirect *r = &bufs.redirects[i / 3];
    r->child_fd = element_int(mrb, table_ary, i);
    r->kind = redirect_kind(mrb, mrb_ary_ref(mrb, table_ary, i + 1));
    r->source_fd = (r->kind == MRB_PROCESS_REDIR_CLOSE) ? -1 : element_int(mrb, table_ary, i + 2);
  }

  params.argv = bufs.argv;
  params.env = bufs.env;
  params.nenv = (size_t)(envc / 2);
  params.redirects = bufs.redirects;
  params.nredirects = (size_t)(tablec / 3);

  /* The record is reserved before the child exists, because that is the step
     that can fail: once the OS has created a process, nothing here may raise
     before something owns the wait it owes. */
  table = mrb_process_table_get(mrb);
  record = mrb_process_record_reserve(mrb, table);

  if (mrb_hal_process_spawn(mrb, mrb_process_table_context(table), &params, &child) != 0) {
    /* mrb_sys_fail() leaves by longjmp, so the buffers are freed first and
       the errno it reports on is saved across the free. */
    err = errno;
    mrb_process_record_discard(mrb, record);
    bufs_free(mrb, &bufs);
    errno = err;
    mrb_sys_fail(mrb, "spawn");
  }
  bufs_free(mrb, &bufs);

  mrb_process_record_commit(record, child);
  return mrb_int_value(mrb, record->pid);
}

void
mrb_process_spawn_init(mrb_state *mrb, struct RClass *process)
{
  mrb_define_module_function_id(mrb, process, MRB_SYM(__spawn), process_s___spawn,
                                MRB_ARGS_REQ(4));
}

#else /* !MRB_HAL_PROCESS_HAS_SPAWN */

void
mrb_process_spawn_init(mrb_state *mrb, struct RClass *process)
{
  /* Without a way to create a process, Process.spawn is not defined at all:
     mrblib/process.rb defines it only where this primitive exists, so a
     program is told by the missing method rather than by a failure at the
     point of no return.  Whether there is a way is the port's answer, or a
     build's veto of it; see process_hal.h. */
  (void)mrb; (void)process;
}

#endif /* MRB_HAL_PROCESS_HAS_SPAWN */
