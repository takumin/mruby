/*
** spawn.c - Process.__spawn
**
** See Copyright Notice in mruby.h
**
** The C half of Process.spawn.  CRuby's argument shape is `[env,]
** command..., [options]`, and taking that apart in C would pile Hash, Array
** and IO case analysis in here; mrblib/process.rb does it instead and hands
** this down flat integer and string arrays.  What is left is the part that
** has to happen in C: validating the strings, marshalling them into the
** structs the HAL takes, and owning the child the port created, so that the
** wait it now owes has something owing it.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/error.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <string.h>

#ifndef MRB_NO_PROCESS_SPAWN

/* The three arrays the HAL reads.  They are freed together, including on the
   way out of a failure, because mrb_sys_fail() leaves by longjmp and takes
   any chance to free them with it. */
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
  bufs->argv = NULL;
  bufs->env = NULL;
  bufs->redirects = NULL;
}

/* A string the operating system can take: really a String, and without an
   embedded NUL, which mrb_string_value_cstr() is what checks. */
static const char*
element_cstr(mrb_state *mrb, mrb_value ary, mrb_int idx)
{
  mrb_value v = mrb_ary_ref(mrb, ary, idx);

  if (!mrb_string_p(v)) {
    mrb_raisef(mrb, E_TYPE_ERROR, "no implicit conversion of %Y into String", v);
  }
  return mrb_string_value_cstr(mrb, &v);
}

static mrb_int
element_int(mrb_state *mrb, mrb_value ary, mrb_int idx)
{
  return mrb_as_int(mrb, mrb_ary_ref(mrb, ary, idx));
}

/*
 * call-seq:
 *   Process.__spawn(kind, argv, env, redirects, flags, chdir) -> pid
 *
 * The primitive Process.spawn is written on.  Everything here is already
 * flat: +argv+ is an array of Strings, +env+ a [name, value_or_nil, ...]
 * array, +redirects+ a [child_fd, kind, source_fd, ...] array read in order.
 */
static mrb_value
process_s___spawn(mrb_state *mrb, mrb_value self)
{
  mrb_value argv_ary, env_ary, table_ary, chdir;
  mrb_int kind, flags, argc, envc, tablec, i;
  mrb_process_spawn_params params;
  struct spawn_bufs bufs = { NULL, NULL, NULL };
  mrb_hal_process_child *child;
  mrb_process_table *table;
  mrb_process_record *record;
  int err;

  mrb_get_args(mrb, "iAAAiS!", &kind, &argv_ary, &env_ary, &table_ary, &flags, &chdir);

  argc = RARRAY_LEN(argv_ary);
  envc = RARRAY_LEN(env_ary);
  tablec = RARRAY_LEN(table_ary);

  if (kind != MRB_PROCESS_SPAWN_ARGV && kind != MRB_PROCESS_SPAWN_SHELL) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown spawn kind %i", kind);
  }
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
    mrb_int rkind = element_int(mrb, table_ary, i + 1);
    if (rkind != MRB_PROCESS_REDIR_PARENT && rkind != MRB_PROCESS_REDIR_CHILD &&
        rkind != MRB_PROCESS_REDIR_CLOSE) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown redirection kind %i", rkind);
    }
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
  if (!mrb_nil_p(chdir)) mrb_string_value_cstr(mrb, &chdir);

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
    r->kind = (mrb_process_redirect_kind)element_int(mrb, table_ary, i + 1);
    r->source_fd = element_int(mrb, table_ary, i + 2);
  }

  params.kind = (mrb_process_spawn_kind)kind;
  params.argv = bufs.argv;
  params.env = bufs.env;
  params.nenv = (size_t)(envc / 2);
  params.redirects = bufs.redirects;
  params.nredirects = (size_t)(tablec / 3);
  params.chdir = mrb_nil_p(chdir) ? NULL : RSTRING_PTR(chdir);
  params.flags = (unsigned int)flags &
                 (MRB_PROCESS_SPAWN_CLOSE_OTHERS | MRB_PROCESS_SPAWN_UNSETENV_OTHERS);

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
                                MRB_ARGS_REQ(6));
}

#else /* MRB_NO_PROCESS_SPAWN */

void
mrb_process_spawn_init(mrb_state *mrb, struct RClass *process)
{
  /* Without a way to create a process, Process.spawn is not defined at all:
     mrblib/process.rb defines it only where this primitive exists, so a
     program is told by the missing method rather than by a failure at the
     point of no return. */
  (void)mrb; (void)process;
}

#endif /* MRB_NO_PROCESS_SPAWN */
