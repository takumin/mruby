/*
** spawn.c - Process.__spawn
**
** See Copyright Notice in mruby.h
**
** The C half of Process.spawn.  CRuby's argument shape is `command...,
** [options]`, and taking that apart in C would pile Hash, Array and IO case
** analysis in here; mrblib/process.rb does it instead and hands this down
** flat integer and string arrays.  What is left is the part that has to
** happen in C: checking that every element really is something the operating
** system can take, and marshalling them into the structs the HAL reads.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/error.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>

#ifndef MRB_NO_PROCESS_SPAWN

/* The two arrays the HAL reads.  They are freed together, including on the
   way out of a failure, because mrb_sys_fail() leaves by longjmp and takes
   any chance to free them with it. */
struct spawn_bufs {
  const char **argv;
  mrb_process_redirect *redirects;
};

static void
bufs_free(mrb_state *mrb, struct spawn_bufs *bufs)
{
  mrb_free(mrb, bufs->argv);
  mrb_free(mrb, bufs->redirects);
  bufs->argv = NULL;
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
 *   Process.__spawn(kind, argv, redirects, flags) -> pid
 *
 * The primitive Process.spawn is written on.  Everything here is already
 * flat: +argv+ is an array of Strings, and +redirects+ a [child_fd, kind,
 * source_fd, ...] array read in order.
 */
static mrb_value
process_s___spawn(mrb_state *mrb, mrb_value self)
{
  mrb_value argv_ary, table_ary;
  mrb_int kind, flags, argc, tablec, i;
  mrb_process_spawn_params params;
  struct spawn_bufs bufs = { NULL, NULL };
  mrb_int pid;
  int err;

  mrb_get_args(mrb, "iAAi", &kind, &argv_ary, &table_ary, &flags);
  argc = RARRAY_LEN(argv_ary);
  tablec = RARRAY_LEN(table_ary);

  if (kind != MRB_PROCESS_SPAWN_ARGV && kind != MRB_PROCESS_SPAWN_SHELL) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown spawn kind %i", kind);
  }
  if (argc < 1) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no command given");
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

  bufs.argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (size_t)(argc + 1));
  if (tablec > 0) {
    bufs.redirects = (mrb_process_redirect*)
      mrb_malloc_simple(mrb, sizeof(mrb_process_redirect) * (size_t)(tablec / 3));
  }
  if (bufs.argv == NULL || (tablec > 0 && bufs.redirects == NULL)) {
    bufs_free(mrb, &bufs);
    mrb_raise(mrb, E_RUNTIME_ERROR, "out of memory");
  }

  for (i = 0; i < argc; i++) {
    bufs.argv[i] = element_cstr(mrb, argv_ary, i);
  }
  bufs.argv[argc] = NULL;
  for (i = 0; i < tablec; i += 3) {
    mrb_process_redirect *r = &bufs.redirects[i / 3];
    r->child_fd = element_int(mrb, table_ary, i);
    r->kind = (mrb_process_redirect_kind)element_int(mrb, table_ary, i + 1);
    r->source_fd = element_int(mrb, table_ary, i + 2);
  }

  params.kind = (mrb_process_spawn_kind)kind;
  params.argv = bufs.argv;
  params.redirects = bufs.redirects;
  params.nredirects = (size_t)(tablec / 3);
  params.flags = (unsigned int)flags & MRB_PROCESS_SPAWN_CLOSE_OTHERS;

  if (mrb_hal_process_spawn(mrb, &params, &pid) != 0) {
    /* mrb_sys_fail() leaves by longjmp, so the buffers are freed first and
       the errno it reports on is saved across the free. */
    err = errno;
    bufs_free(mrb, &bufs);
    errno = err;
    mrb_sys_fail(mrb, "spawn");
  }
  bufs_free(mrb, &bufs);
  return mrb_int_value(mrb, pid);
}

void
mrb_process_spawn_init(mrb_state *mrb, struct RClass *process)
{
  mrb_define_module_function_id(mrb, process, MRB_SYM(__spawn), process_s___spawn,
                                MRB_ARGS_REQ(4));
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
