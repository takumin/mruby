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
** strings, refusing the options this port has not declared it acts on,
** marshalling everything into the structs the HAL takes, and owning the
** child the port created, so that the wait it now owes has something owing
** it.
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
  mrb_process_rlimit *rlimits;
};

static void
bufs_free(mrb_state *mrb, struct spawn_bufs *bufs)
{
  mrb_free(mrb, bufs->argv);
  mrb_free(mrb, bufs->env);
  mrb_free(mrb, bufs->redirects);
  mrb_free(mrb, bufs->rlimits);
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
 * checked here is the value's shape and whether this port acts on the
 * option at all.  An option the port has not declared is refused as one
 * that does not exist, which is what CRuby answers for `new_pgroup` on
 * POSIX and `pgroup` on Windows: `ArgumentError`, naming the symbol.
 */

static mrb_value
opt_get(mrb_state *mrb, mrb_value opts, mrb_sym key)
{
  return mrb_hash_fetch(mrb, opts, mrb_symbol_value(key), mrb_nil_value());
}

static void
opt_refuse(mrb_state *mrb, mrb_value opts, mrb_sym key)
{
  if (!mrb_nil_p(opt_get(mrb, opts, key))) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "wrong exec option symbol: %n", key);
  }
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

/* A number that is a pid, a mode or an id: an Integer this platform's `int`
   holds, and not negative.  nil is the option not given. */
static mrb_int
opt_uint(mrb_state *mrb, mrb_value opts, mrb_sym key)
{
  mrb_value v = opt_get(mrb, opts, key);
  mrb_int n;

  if (mrb_nil_p(v)) return MRB_PROCESS_SPAWN_UNSET;
  n = mrb_as_int(mrb, v);
  if (n < 0) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative %n: %i", key, n);
  }
  return mrb_process_int_arg(mrb, n, mrb_sym_name(mrb, key));
}

static const char*
opt_cstr(mrb_state *mrb, mrb_value opts, mrb_sym key)
{
  mrb_value v = opt_get(mrb, opts, key);

  if (mrb_nil_p(v)) return NULL;
  return value_cstr(mrb, v);
}

#ifdef MRB_HAL_PROCESS_HAS_RLIMIT

/* The resource an `rlimit_<name>` option names, as the interface numbers
   it.  The names are CRuby's, downcased as the option keys spell them. */
static const struct { const char *name; mrb_process_rlimit_id id; } rlimit_names[] = {
  { "as",         MRB_PROCESS_RLIMIT_AS },
  { "core",       MRB_PROCESS_RLIMIT_CORE },
  { "cpu",        MRB_PROCESS_RLIMIT_CPU },
  { "data",       MRB_PROCESS_RLIMIT_DATA },
  { "fsize",      MRB_PROCESS_RLIMIT_FSIZE },
  { "memlock",    MRB_PROCESS_RLIMIT_MEMLOCK },
  { "msgqueue",   MRB_PROCESS_RLIMIT_MSGQUEUE },
  { "nice",       MRB_PROCESS_RLIMIT_NICE },
  { "nofile",     MRB_PROCESS_RLIMIT_NOFILE },
  { "nproc",      MRB_PROCESS_RLIMIT_NPROC },
  { "npts",       MRB_PROCESS_RLIMIT_NPTS },
  { "rss",        MRB_PROCESS_RLIMIT_RSS },
  { "rtprio",     MRB_PROCESS_RLIMIT_RTPRIO },
  { "rttime",     MRB_PROCESS_RLIMIT_RTTIME },
  { "sbsize",     MRB_PROCESS_RLIMIT_SBSIZE },
  { "sigpending", MRB_PROCESS_RLIMIT_SIGPENDING },
  { "stack",      MRB_PROCESS_RLIMIT_STACK },
};

/* Which resource `name` is, or a refusal naming the option as CRuby names
   it: a resource this platform has no number for is as absent as one no
   platform has. */
static mrb_process_rlimit_id
rlimit_id(mrb_state *mrb, mrb_value name)
{
  size_t i;

  if (mrb_symbol_p(name)) {
    const char *s = mrb_sym_name(mrb, mrb_symbol(name));

    for (i = 0; i < sizeof(rlimit_names) / sizeof(rlimit_names[0]); i++) {
      if (strcmp(s, rlimit_names[i].name) == 0 &&
          mrb_hal_process_rlimit_p(rlimit_names[i].id)) {
        return rlimit_names[i].id;
      }
    }
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "wrong exec option symbol: rlimit_%s", s);
  }
  mrb_raisef(mrb, E_ARGUMENT_ERROR, "wrong exec option");
  return MRB_PROCESS_RLIMIT_COUNT; /* not reached */
}

/* A limit is a count the platform's `rlim_t` holds, and every value below
   zero is either its "no limit" or nothing at all; neither is a number a
   caller can mean here. */
static mrb_int
rlimit_value(mrb_state *mrb, mrb_value v)
{
  mrb_int n = mrb_as_int(mrb, v);

  if (n < 0) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative resource limit: %i", n);
  }
  return n;
}

/* The `rlimits` option is an Array of [name, cur, max], checked here and
   read again below once nothing else can go wrong. */
static void
rlimits_check(mrb_state *mrb, mrb_value ary)
{
  mrb_int i;

  if (!mrb_array_p(ary)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "malformed resource limits");
  }
  for (i = 0; i < RARRAY_LEN(ary); i++) {
    mrb_value e = mrb_ary_ref(mrb, ary, i);

    if (!mrb_array_p(e) || RARRAY_LEN(e) != 3) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "malformed resource limits");
    }
    rlimit_id(mrb, mrb_ary_ref(mrb, e, 0));
    rlimit_value(mrb, mrb_ary_ref(mrb, e, 1));
    rlimit_value(mrb, mrb_ary_ref(mrb, e, 2));
  }
}

#endif /* MRB_HAL_PROCESS_HAS_RLIMIT */

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
 * `:prog`, `:chdir`, `:close_others`, `:unsetenv_others`, `:pgroup`,
 * `:new_pgroup`, `:umask`, `:uid`, `:gid` and `:rlimits`, each present only
 * where the caller gave it.
 */
static mrb_value
process_s___spawn(mrb_state *mrb, mrb_value self)
{
  mrb_value argv_ary, env_ary, table_ary, opts, rlimits;
  mrb_int argc, envc, tablec, i;
  mrb_process_spawn_params params;
  struct spawn_bufs bufs = { NULL, NULL, NULL, NULL };
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
  params.pgroup = MRB_PROCESS_SPAWN_UNSET;
  params.umask = MRB_PROCESS_SPAWN_UNSET;
  params.uid = MRB_PROCESS_SPAWN_UNSET;
  params.gid = MRB_PROCESS_SPAWN_UNSET;

  /* The options a port acts on are the ones it declared; see
     process_hal_features.h.  The rest are refused as CRuby refuses an
     option its platform has no call behind. */
#ifdef MRB_HAL_PROCESS_HAS_NEW_PGROUP
  if (opt_bool(mrb, opts, MRB_SYM(new_pgroup))) params.flags |= MRB_PROCESS_SPAWN_NEW_PGROUP;
#else
  opt_refuse(mrb, opts, MRB_SYM(new_pgroup));
#endif
#ifdef MRB_HAL_PROCESS_HAS_PGROUP
  params.pgroup = opt_uint(mrb, opts, MRB_SYM(pgroup));
#else
  opt_refuse(mrb, opts, MRB_SYM(pgroup));
#endif
#ifdef MRB_HAL_PROCESS_HAS_UMASK
  params.umask = opt_uint(mrb, opts, MRB_SYM(umask));
#else
  opt_refuse(mrb, opts, MRB_SYM(umask));
#endif
#ifdef MRB_HAL_PROCESS_HAS_UID
  params.uid = opt_uint(mrb, opts, MRB_SYM(uid));
  params.gid = opt_uint(mrb, opts, MRB_SYM(gid));
#else
  opt_refuse(mrb, opts, MRB_SYM(uid));
  opt_refuse(mrb, opts, MRB_SYM(gid));
#endif
  rlimits = opt_get(mrb, opts, MRB_SYM(rlimits));
#ifdef MRB_HAL_PROCESS_HAS_RLIMIT
  if (!mrb_nil_p(rlimits)) rlimits_check(mrb, rlimits);
#else
  if (!mrb_nil_p(rlimits) && (!mrb_array_p(rlimits) || RARRAY_LEN(rlimits) > 0)) {
    mrb_value first = mrb_array_p(rlimits) ? mrb_ary_ref(mrb, rlimits, 0) : mrb_nil_value();
    mrb_value name = mrb_array_p(first) ? mrb_ary_ref(mrb, first, 0) : mrb_nil_value();
    if (mrb_symbol_p(name)) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "wrong exec option symbol: rlimit_%n", mrb_symbol(name));
    }
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong exec option");
  }
#endif

  bufs.argv = (const char**)mrb_malloc_simple(mrb, sizeof(char*) * (size_t)(argc + 1));
  if (envc > 0) {
    bufs.env = (mrb_process_env_entry*)
      mrb_malloc_simple(mrb, sizeof(mrb_process_env_entry) * (size_t)(envc / 2));
  }
  if (tablec > 0) {
    bufs.redirects = (mrb_process_redirect*)
      mrb_malloc_simple(mrb, sizeof(mrb_process_redirect) * (size_t)(tablec / 3));
  }
#ifdef MRB_HAL_PROCESS_HAS_RLIMIT
  if (!mrb_nil_p(rlimits) && RARRAY_LEN(rlimits) > 0) {
    bufs.rlimits = (mrb_process_rlimit*)
      mrb_malloc_simple(mrb, sizeof(mrb_process_rlimit) * (size_t)RARRAY_LEN(rlimits));
  }
#endif
  if (bufs.argv == NULL || (envc > 0 && bufs.env == NULL) ||
      (tablec > 0 && bufs.redirects == NULL) ||
      (!mrb_nil_p(rlimits) && RARRAY_LEN(rlimits) > 0 && bufs.rlimits == NULL)) {
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
#ifdef MRB_HAL_PROCESS_HAS_RLIMIT
  if (bufs.rlimits != NULL) {
    for (i = 0; i < RARRAY_LEN(rlimits); i++) {
      mrb_value e = mrb_ary_ref(mrb, rlimits, i);
      bufs.rlimits[i].resource = rlimit_id(mrb, mrb_ary_ref(mrb, e, 0));
      bufs.rlimits[i].cur = rlimit_value(mrb, mrb_ary_ref(mrb, e, 1));
      bufs.rlimits[i].max = rlimit_value(mrb, mrb_ary_ref(mrb, e, 2));
    }
    params.rlimits = bufs.rlimits;
    params.nrlimits = (size_t)RARRAY_LEN(rlimits);
  }
#endif

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
