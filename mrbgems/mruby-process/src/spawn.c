/*
** spawn.c - Process.__spawn
**
** See Copyright Notice in mruby.h
**
** The C half of Process.spawn.  CRuby's argument shape is `command...`, and
** deciding which form was written is string work; mrblib/process.rb does it
** and hands this down a flat array.  What is left is the part that has to
** happen in C: checking that every element really is a String the operating
** system can take, and marshalling them into the struct the HAL reads.
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/error.h>
#include <mruby/string.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>

#ifndef MRB_NO_PROCESS_SPAWN

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

/*
 * call-seq:
 *   Process.__spawn(kind, argv) -> pid
 *
 * The primitive Process.spawn is written on.  +argv+ is an array of Strings,
 * and +kind+ says whether it is a command line for the shell or an image and
 * its arguments.
 */
static mrb_value
process_s___spawn(mrb_state *mrb, mrb_value self)
{
  mrb_value argv_ary;
  mrb_int kind, argc, i;
  mrb_process_spawn_params params;
  const char **argv;
  mrb_int pid;
  int err;

  mrb_get_args(mrb, "iA", &kind, &argv_ary);
  argc = RARRAY_LEN(argv_ary);

  if (kind != MRB_PROCESS_SPAWN_ARGV && kind != MRB_PROCESS_SPAWN_SHELL) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown spawn kind %i", kind);
  }
  if (argc < 1) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no command given");
  }

  /* Everything that can raise happens before anything is allocated: the
     strings are checked here, and read again below once nothing else can go
     wrong between the allocation and the call. */
  for (i = 0; i < argc; i++) {
    element_cstr(mrb, argv_ary, i);
  }

  argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (size_t)(argc + 1));
  if (argv == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "out of memory");
  }
  for (i = 0; i < argc; i++) {
    argv[i] = element_cstr(mrb, argv_ary, i);
  }
  argv[argc] = NULL;

  params.kind = (mrb_process_spawn_kind)kind;
  params.argv = argv;

  if (mrb_hal_process_spawn(mrb, &params, &pid) != 0) {
    /* mrb_sys_fail() leaves by longjmp, so the buffer is freed first and the
       errno it reports on is saved across the free. */
    err = errno;
    mrb_free(mrb, argv);
    errno = err;
    mrb_sys_fail(mrb, "spawn");
  }
  mrb_free(mrb, argv);
  return mrb_int_value(mrb, pid);
}

void
mrb_process_spawn_init(mrb_state *mrb, struct RClass *process)
{
  mrb_define_module_function_id(mrb, process, MRB_SYM(__spawn), process_s___spawn,
                                MRB_ARGS_REQ(2));
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
