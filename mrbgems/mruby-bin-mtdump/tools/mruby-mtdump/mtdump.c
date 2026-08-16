/*
** mtdump.c - dump the method tables of a freshly opened mrb_state
**
** Walks every class and module the VM has and writes one line per method:
** which table the method is in, and what that table dispatches to.
**
** This is the runtime counterpart of the C source scanner in
** tools/mruby_method_index.rb.  The scanner reads the sources as text and has
** to infer the owning class of a ROM table from a local `struct RClass *`; it
** also cannot see which branches of a #if survive or which gems a gembox
** pulls in.  Here the owning class *is* the table the method sits in, and a
** method exists only if this build registered it, so all three stop being
** guesses.
**
** What a method table cannot report is where the registration was written --
** it holds a function pointer, not a source line.  So this dump is
** authoritative for the mapping and the scanner supplies the provenance; see
** MethodIndex.merge in tools/mruby_method_index.rb.
**
** C entry points are printed as runtime addresses, next to the address of one
** named anchor function.  The reader turns the difference between the two
** into a symbol name with `nm` on this same binary, which is what keeps the
** output meaningful under a position-independent executable.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/debug.h>
#include <mruby/gc.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/variable.h>

#define MTDUMP_FORMAT_VERSION 2

/* The symbol the reader resolves every other address against.  Any function
** with external linkage would do; mrb_open is here in every build. */
#define MTDUMP_ANCHOR_NAME "mrb_open"
#define MTDUMP_ANCHOR_FUNC mrb_open

typedef void (*mtdump_anyfunc)(void);

/* Function pointers are not integers, and not void* either.  Reading one back
** through a union is the portable-in-practice way to print it. */
static unsigned long long
addr_of(mtdump_anyfunc f)
{
  union { mtdump_anyfunc f; uintptr_t a; } u;
  u.f = f;
  return (unsigned long long)u.a;
}

#define FUNC_ADDR(f) addr_of((mtdump_anyfunc)(f))

/* ---------------------------------------------------------------- classes */

struct class_list {
  struct RClass **items;
  size_t len;
  size_t cap;
  size_t origin_iclasses;
  int oom;
};

/* Collect class objects only.  Nothing here allocates on the mruby heap: the
** callback runs inside mrb_objspace_each_objects, which is walking the very
** pages an allocation could grow. */
static int
collect_class(mrb_state *mrb, struct RBasic *obj, void *data)
{
  struct class_list *list = (struct class_list*)data;

  if (mrb_object_dead_p(mrb, obj)) return MRB_EACH_OBJ_OK;

  switch (obj->tt) {
  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    break;
  case MRB_TT_ICLASS:
    /* An iclass shares its module's method table, so following it would
    ** report every included method a second time under the including class.
    ** An origin iclass is the exception -- it holds methods that were moved
    ** out of a prepended-into class -- and is counted rather than walked. */
    if (((struct RClass*)obj)->flags & MRB_FL_CLASS_IS_ORIGIN) {
      list->origin_iclasses++;
    }
    return MRB_EACH_OBJ_OK;
  default:
    return MRB_EACH_OBJ_OK;
  }

  if (list->len == list->cap) {
    size_t cap = list->cap ? list->cap * 2 : 256;
    struct RClass **items = (struct RClass**)realloc(list->items, cap * sizeof(*items));
    if (!items) {
      list->oom = 1;
      return MRB_EACH_OBJ_BREAK;
    }
    list->items = items;
    list->cap = cap;
  }
  list->items[list->len++] = (struct RClass*)obj;
  return MRB_EACH_OBJ_OK;
}

/* A class is named once something has cached __classname__ on it, which is
** what mrb_define_class_id and friends do.  Asking mrb_class_name instead
** would manufacture a "#<Class:0x...>" for the unnamed ones, and an address
** is not a name anyone can look a method up by. */
static mrb_bool
class_named_p(mrb_state *mrb, struct RClass *c)
{
  return !mrb_nil_p(mrb_iv_get(mrb, mrb_obj_value(c), MRB_SYM(__classname__)));
}

/* The class a singleton table belongs to, or NULL when the singleton is not
** a class's -- an ordinary object's singleton methods have no `Class.method`
** spelling to report them under. */
static struct RClass*
singleton_owner(mrb_state *mrb, struct RClass *sc)
{
  mrb_value attached = mrb_iv_get(mrb, mrb_obj_value(sc), MRB_SYM(__attached__));

  switch (mrb_type(attached)) {
  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
    return mrb_class_ptr(attached);
  default:
    return NULL;
  }
}

/* ---------------------------------------------------------------- entries */

/* Whether the layer this symbol is served from is a ROM table.  mrb_mt_foreach
** yields the topmost unshadowed entry for a symbol, so the topmost layer
** holding it is the one that answered.  Returns -1 when the walk disagrees
** with the callback, which it should not. */
static int
entry_is_rom(struct RClass *c, mrb_sym sym)
{
  mrb_mt_tbl *t;

  for (t = c->mt; t; t = t->next) {
    int i;
    for (i = 0; i < t->size; i++) {
      if (t->ptr[i].key != sym) continue;
      if (MRB_MT_REMOVED_P(t->ptr[i])) return -1;
      return (t->alloc & MRB_MT_READONLY_BIT) ? 1 : 0;
    }
  }
  return -1;
}

struct dump_state {
  FILE *out;
  const char *klass;
  const char *sep;
  struct RClass *c;
  size_t methods;
  size_t unnamed_syms;
};

/* Tab-separated output over names the VM will hand us verbatim.  Nothing in a
** stock build carries a tab or a newline, but a symbol can hold any byte, and
** one that did would silently shift every later column. */
static void
put_field(FILE *out, const char *s, size_t len)
{
  size_t i;
  for (i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)s[i];
    switch (ch) {
    case '\t': fputs("\\t", out); break;
    case '\n': fputs("\\n", out); break;
    case '\r': fputs("\\r", out); break;
    case '\\': fputs("\\\\", out); break;
    default:   fputc((int)ch, out); break;
    }
  }
}

static int
dump_entry(mrb_state *mrb, mrb_sym sym, mrb_method_t m, void *data)
{
  struct dump_state *st = (struct dump_state*)data;
  mrb_int name_len;
  const char *name = mrb_sym_name_len(mrb, sym, &name_len);
  const char *kind;
  /* Wide enough for a source path; snprintf truncates rather than overruns
  ** if some build ever exceeds it. */
  char target[1024];
  char alias_of[512];
  int rom;

  if (!name) {
    st->unnamed_syms++;
    return 0;
  }

  strcpy(alias_of, "-");

  if (MRB_METHOD_UNDEF_P(m)) {
    /* Explicitly undefined: the entry exists so lookup stops here. */
    kind = "undef";
    strcpy(target, "-");
  }
  else if (MRB_METHOD_FUNC_P(m)) {
    kind = "cfunc";
    snprintf(target, sizeof(target), "0x%llx", FUNC_ADDR(MRB_METHOD_FUNC(m)));
  }
  else {
    const struct RProc *p = MRB_METHOD_PROC(m);

    if (MRB_PROC_ALIAS_P(p)) {
      /* An alias proc carries the name it was defined from in body.mid and
      ** the proc it forwards to in upper.  Following upper is what the VM
      ** itself does before it runs the method (MRB_PROC_RESOLVE_ALIAS in
      ** src/vm.c), so it lands on the body that will actually execute --
      ** which the name cannot do, since the method it forwards to need not
      ** be in this table at all.  Enumerator::Lazy#force is aliased from
      ** entries, and entries lives in Enumerable.
      **
      ** Report the resolved body as the entry, and keep the name it was
      ** aliased from in a column of its own. */
      mrb_int alias_len = 0;
      const char *alias = mrb_sym_name_len(mrb, p->body.mid, &alias_len);

      if (alias) {
        snprintf(alias_of, sizeof(alias_of), "%.*s", (int)alias_len, alias);
      }
      /* mrb_alias_method never aliases an alias -- it copies the entry -- so
      ** this walks once.  Loop anyway rather than assume it. */
      while (p && MRB_PROC_ALIAS_P(p)) {
        p = p->upper;
      }
    }

    if (!p) {
      /* An alias with nothing under it.  Nothing in a freshly opened state
      ** produces one, and reporting it is better than dereferencing NULL. */
      kind = "alias";
      strcpy(target, "-");
    }
    else if (MRB_PROC_CFUNC_P(p)) {
      kind = "cfunc";
      snprintf(target, sizeof(target), "0x%llx", FUNC_ADDR(MRB_PROC_CFUNC(p)));
    }
    else {
      /* Written in Ruby.  There is no C entry point to name, but there is a
      ** source location, if this build kept the irep debug info. */
      const mrb_irep *irep = p->body.irep;
      const char *file = irep ? mrb_debug_get_filename(mrb, irep, 0) : NULL;
      int32_t line = irep ? mrb_debug_get_line(mrb, irep, 0) : -1;

      kind = "proc";
      if (file && line >= 0) {
        snprintf(target, sizeof(target), "%s:%d", file, (int)line);
      }
      else {
        strcpy(target, "-");
      }
    }
  }

  rom = entry_is_rom(st->c, sym);

  fputs("m\t", st->out);
  put_field(st->out, st->klass, strlen(st->klass));
  fputc('\t', st->out);
  fputs(st->sep, st->out);
  fputc('\t', st->out);
  put_field(st->out, name, (size_t)name_len);
  fprintf(st->out, "\t%s\t", kind);
  put_field(st->out, target, strlen(target));
  fprintf(st->out, "\t%s\t", rom == 1 ? "rom" : rom == 0 ? "heap" : "-");
  put_field(st->out, alias_of, strlen(alias_of));
  fputc('\n', st->out);

  st->methods++;
  return 0;
}

/* ------------------------------------------------------------------- dump */

static int
dump(mrb_state *mrb, FILE *out)
{
  struct class_list list;
  struct dump_state st;
  size_t i, classes = 0, anonymous = 0;
  mrb_bool gc_was_disabled;

  memset(&list, 0, sizeof(list));
  memset(&st, 0, sizeof(st));
  st.out = out;

  mrb_objspace_each_objects(mrb, collect_class, &list);
  if (list.oom) {
    fprintf(stderr, "mruby-mtdump: out of memory collecting classes\n");
    free(list.items);
    return 1;
  }

  /* Naming a class allocates a string, and a collection in the middle of the
  ** walk could free a class this list still points at.  Nothing here creates
  ** garbage worth collecting anyway -- the process exits right after. */
  gc_was_disabled = mrb->gc.disabled;
  mrb->gc.disabled = TRUE;

  fprintf(out, "!mtdump\t%d\n", MTDUMP_FORMAT_VERSION);
  fprintf(out, "!anchor\t%s\t0x%llx\n",
          MTDUMP_ANCHOR_NAME, FUNC_ADDR(MTDUMP_ANCHOR_FUNC));

  for (i = 0; i < list.len; i++) {
    struct RClass *c = list.items[i];
    struct RClass *owner = c;

    if (c->tt == MRB_TT_SCLASS) {
      owner = singleton_owner(mrb, c);
      if (!owner) { anonymous++; continue; }
      st.sep = ".";
    }
    else {
      st.sep = "#";
    }

    if (!class_named_p(mrb, owner)) { anonymous++; continue; }

    st.klass = mrb_class_name(mrb, owner);
    if (!st.klass) { anonymous++; continue; }
    st.c = c;
    classes++;
    mrb_mt_foreach(mrb, c, dump_entry, &st);
  }

  fprintf(out, "!stats\tclasses=%lu\tmethods=%lu\tanonymous=%lu"
               "\torigin_iclasses=%lu\tunnamed_syms=%lu\n",
          (unsigned long)classes, (unsigned long)st.methods,
          (unsigned long)anonymous, (unsigned long)list.origin_iclasses,
          (unsigned long)st.unnamed_syms);

  if (list.origin_iclasses > 0) {
    fprintf(stderr, "mruby-mtdump: %lu origin iclasses were not walked; "
                    "methods moved aside by a prepended module are missing\n",
            (unsigned long)list.origin_iclasses);
  }

  mrb->gc.disabled = gc_was_disabled;
  free(list.items);
  return 0;
}

static void
usage(FILE *out, const char *prog)
{
  fprintf(out,
    "Usage: %s [-o FILE]\n"
    "\n"
    "Dump this build's method tables as TSV.  One line per method:\n"
    "\n"
    "  m <class> <#|.> <name> <cfunc|proc|undef> <target> <rom|heap> <aliased-from>\n"
    "\n"
    "A cfunc target is a runtime address, to be read against the !anchor\n"
    "line and `nm %s`.  Consumed by tools/mruby_method_index.rb --runtime.\n",
    prog, prog);
}

int
main(int argc, char **argv)
{
  mrb_state *mrb;
  FILE *out = stdout;
  const char *path = NULL;
  int i, status;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(stdout, argv[0]);
      return 0;
    }
    else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      path = argv[++i];
    }
    else {
      fprintf(stderr, "%s: unknown argument: %s\n", argv[0], argv[i]);
      usage(stderr, argv[0]);
      return 1;
    }
  }

  if (path) {
    out = fopen(path, "w");
    if (!out) {
      fprintf(stderr, "%s: cannot write %s\n", argv[0], path);
      return 1;
    }
  }

  mrb = mrb_open();
  if (!mrb) {
    fprintf(stderr, "%s: mrb_open() failed\n", argv[0]);
    if (path) fclose(out);
    return 1;
  }

  status = dump(mrb, out);

  mrb_close(mrb);
  if (path && fclose(out) != 0) {
    fprintf(stderr, "%s: cannot write %s\n", argv[0], path);
    return 1;
  }
  return status;
}
