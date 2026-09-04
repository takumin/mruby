/*
** rom_coverage.c - how much of the Ruby code a build loads could be defined
** from ROM instead of at startup
**
** See Copyright Notice in mruby.h
*/

/* Every method written in Ruby costs an RProc in the object heap at startup,
   even though the irep behind it is already `static const`. Moving those
   definitions into read-only method tables would take them out of RAM, but
   only the definitions a build can pin down: this reports how many there are.

   It compiles each unit of Ruby source the way the build does (the files of
   `mrblib`, then each gem's `mrblib`, concatenated in the same order), never
   runs it, and walks the irep tree counting `def`s against the conditions a
   build-time transformation could work under.

   A `def` is counted as movable when all of the following hold:

   - it sits in the body of a class or module opened at the top level of the
     unit, so its lexical scope is that class and `Object`, which is what a
     lookup falling through to the class reaches anyway;
   - the superclass is a constant or absent, so the build can name it;
   - the body has no branch, so the definition is unconditional.

   Two things deliberately do not disqualify a body. A body that also opens a
   nested scope keeps its own `def`s, because a transformation in the code
   generator emits ROM entries for those and leaves the rest of the body to
   run as it does today. A visibility call with arguments (`private :foo`)
   does not disqualify either: it writes a copy of the entry into the class's
   writable layer (see `mrb_mod_visibility()` in src/class.c), which sits in
   front of the read-only one, so the visibility still changes.

   Usage: rom_coverage <manifest> [-v]

   The manifest holds one unit per line: its name, then the source files that
   make it up, separated by tabs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/opcode.h>

/* Instruction sizes, built from the same table mruby/ops.h feeds the code
   generator. */
#define Z 1
#define S 3
#define W 4
#define OPCODE(_,x) x,
static const uint8_t insn_size[] = {
#define B 2
#define BB 3
#define BBB 4
#define BS 4
#define BSS 6
#include <mruby/ops.h>
#undef B
#undef BB
#undef BBB
#undef BS
#undef BSS
};
static const uint8_t insn_size1[] = {
#define B 3
#define BB 4
#define BBB 5
#define BS 5
#define BSS 7
#include <mruby/ops.h>
#undef B
#undef BB
#undef BBB
#undef BS
#undef BSS
};
static const uint8_t insn_size2[] = {
#define B 2
#define BB 3
#define BBB 4
#define BS 4
#define BSS 6
#include <mruby/ops.h>
#undef B
#undef BB
#undef BBB
#undef BS
#undef BSS
};
static const uint8_t insn_size3[] = {
#define B 3
#define BB 5
#define BBB 6
#define BS 5
#define BSS 7
#include <mruby/ops.h>
#undef B
#undef BB
#undef BBB
#undef BS
#undef BSS
};
#undef OPCODE
#undef Z
#undef S
#undef W

static size_t
ins_len(const mrb_code *pc)
{
  switch (pc[0]) {
  case OP_EXT1: return (size_t)insn_size1[pc[1]] + 1;
  case OP_EXT2: return (size_t)insn_size2[pc[1]] + 1;
  case OP_EXT3: return (size_t)insn_size3[pc[1]] + 1;
  default: return insn_size[pc[0]];
  }
}

struct stats {
  int defs;             /* every def in the unit */
  int movable;          /* defs a transformation could take */
  int in_bodies;        /* defs found in a class or module body */
  int held_nested;      /* held back: the body is a nested one */
  int held_branch;      /* held back: the body has a branch */
  int held_super;       /* held back: the superclass is not a constant */
  int held_toplevel;    /* def written at the top level of the unit */
  int bodies, bodies_movable;
};

static mrb_state *mrb;
static mrb_bool verbose;

static mrb_bool
branch_p(uint8_t op)
{
  switch (op) {
  case OP_JMP: case OP_JMPIF: case OP_JMPNOT: case OP_JMPNIL:
  case OP_JMPUW: case OP_EXCEPT: case OP_RESCUE:
    return TRUE;
  default:
    return FALSE;
  }
}

static mrb_bool
def_p(uint8_t op)
{
  return op == OP_TDEF || op == OP_SDEF || op == OP_DEF;
}

/* Every def in a subtree, so that a unit's total counts the ones written
   inside a method or a block as well. */
static int
count_defs(const mrb_irep *irep)
{
  int n = 0;
  const mrb_code *pc = irep->iseq, *end = irep->iseq + irep->ilen;

  while (pc < end) {
    if (def_p(*pc)) n++;
    pc += ins_len(pc);
  }
  for (int i = 0; i < irep->rlen; i++) {
    n += count_defs(irep->reps[i]);
  }
  return n;
}

/* One class or module body. `depth` 0 is a body opened at the top level of the
   unit; deeper is a nested class, a nested module or a `class << self`. */
static void
walk_body(struct stats *s, const mrb_irep *body, mrb_bool super_known,
          const char *name, int depth)
{
  int defs = 0;
  mrb_bool branch = FALSE;
  const mrb_code *pc = body->iseq, *end = body->iseq + body->ilen;

  while (pc < end) {
    if (def_p(*pc)) defs++;
    else if (branch_p(*pc)) branch = TRUE;
    pc += ins_len(pc);
  }

  s->bodies++;
  s->in_bodies += defs;

  if (!super_known)   s->held_super += defs;
  else if (depth > 0) s->held_nested += defs;
  else if (branch)    s->held_branch += defs;
  else {
    s->bodies_movable++;
    s->movable += defs;
    if (verbose) printf("    %-32s %3d defs\n", name, defs);
  }

  for (pc = body->iseq; pc < end; pc += ins_len(pc)) {
    if (*pc == OP_EXEC) {
      walk_body(s, body->reps[pc[2]], TRUE, name, depth + 1);
    }
  }
}

/* What a register holds, to the one precision the superclass test needs. */
enum { R_OTHER, R_NIL, R_CONST };

static void
walk_top(struct stats *s, const mrb_irep *irep)
{
  uint8_t reg[256];
  const mrb_code *pc = irep->iseq, *end = irep->iseq + irep->ilen;
  mrb_bool super_known = TRUE;
  const char *name = "(anonymous)";

  memset(reg, R_OTHER, sizeof(reg));
  while (pc < end) {
    uint8_t op = *pc;

    switch (op) {
    case OP_LOADNIL:
      reg[pc[1]] = R_NIL;
      break;
    case OP_OCLASS: case OP_GETCONST: case OP_GETMCNST:
      reg[pc[1]] = R_CONST;
      break;
    case OP_CLASS:
      super_known = (reg[pc[1]] != R_OTHER) && (reg[pc[1]+1] != R_OTHER);
      name = mrb_sym_name(mrb, irep->syms[pc[2]]);
      reg[pc[1]] = R_CONST;
      break;
    case OP_MODULE:
      super_known = reg[pc[1]] != R_OTHER;
      name = mrb_sym_name(mrb, irep->syms[pc[2]]);
      reg[pc[1]] = R_CONST;
      break;
    case OP_EXEC:
      walk_body(s, irep->reps[pc[2]], super_known, name, 0);
      reg[pc[1]] = R_OTHER;
      break;
    case OP_TDEF: case OP_SDEF: case OP_DEF:
      s->held_toplevel++;
      break;
    default:
      /* Anything else writing a register makes it unreadable to the test
         above; the one-operand forms all write R[a]. */
      if (op != OP_EXT1 && op != OP_EXT2 && op != OP_EXT3 && insn_size[op] >= 2) {
        reg[pc[1]] = R_OTHER;
      }
      break;
    }
    pc += ins_len(pc);
  }
}

static char*
read_sources(char **paths, int n)
{
  size_t cap = 1 << 16, len = 0;
  char *buf = (char*)malloc(cap);

  if (!buf) return NULL;
  buf[0] = '\0';
  for (int i = 0; i < n; i++) {
    FILE *fp = fopen(paths[i], "rb");
    long size;

    if (!fp) {
      fprintf(stderr, "rom_coverage: cannot read %s\n", paths[i]);
      free(buf);
      return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    while (len + (size_t)size + 2 > cap) {
      char *p;
      cap *= 2;
      p = (char*)realloc(buf, cap);
      if (!p) { free(buf); fclose(fp); return NULL; }
      buf = p;
    }
    if (fread(buf + len, 1, (size_t)size, fp) != (size_t)size) {
      fprintf(stderr, "rom_coverage: short read on %s\n", paths[i]);
      free(buf);
      fclose(fp);
      return NULL;
    }
    len += (size_t)size;
    buf[len++] = '\n';
    buf[len] = '\0';
    fclose(fp);
  }
  return buf;
}

static mrb_bool
scan_unit(struct stats *s, const char *name, char **paths, int npaths)
{
  char *src = read_sources(paths, npaths);
  mrb_ccontext *c;
  mrb_value v;

  if (!src) return FALSE;
  c = mrb_ccontext_new(mrb);
  c->no_exec = TRUE;
  mrb_ccontext_filename(mrb, c, name);
  v = mrb_load_string_cxt(mrb, src, c);
  free(src);
  if (mrb->exc) {
    fprintf(stderr, "rom_coverage: %s does not compile\n", name);
    mrb_print_error(mrb);
    mrb_ccontext_free(mrb, c);
    return FALSE;
  }

  const mrb_irep *top = mrb_proc_ptr(v)->body.irep;
  s->defs = count_defs(top);
  if (verbose) printf("  %s\n", name);
  walk_top(s, top);
  mrb_ccontext_free(mrb, c);
  return TRUE;
}

static void
print_row(const char *name, const struct stats *s)
{
  printf("%-24s %5d %8d %7.1f%%   %3d/%-3d %6d %6d %6d %6d\n",
         name, s->defs, s->movable,
         s->defs ? 100.0 * s->movable / s->defs : 0.0,
         s->bodies_movable, s->bodies,
         s->held_nested, s->held_branch, s->held_super,
         s->defs - s->in_bodies);
}

static void
add(struct stats *t, const struct stats *s)
{
  t->defs += s->defs;
  t->movable += s->movable;
  t->in_bodies += s->in_bodies;
  t->held_nested += s->held_nested;
  t->held_branch += s->held_branch;
  t->held_super += s->held_super;
  t->held_toplevel += s->held_toplevel;
  t->bodies += s->bodies;
  t->bodies_movable += s->bodies_movable;
}

#define MAX_PATHS 256

int
main(int argc, char **argv)
{
  const char *manifest = NULL;
  FILE *fp;
  char line[8192];
  struct stats total = {0};

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0) verbose = TRUE;
    else manifest = argv[i];
  }
  if (!manifest) {
    fprintf(stderr, "usage: rom_coverage <manifest> [-v]\n");
    return 2;
  }
  fp = fopen(manifest, "r");
  if (!fp) {
    fprintf(stderr, "rom_coverage: cannot read %s\n", manifest);
    return 1;
  }

  mrb = mrb_open();
  if (!mrb) {
    fprintf(stderr, "rom_coverage: cannot open an mrb_state\n");
    fclose(fp);
    return 1;
  }
  printf("%-24s %5s %8s %8s   %-7s %6s %6s %6s %6s\n",
         "unit", "defs", "movable", "", "bodies",
         "nested", "branch", "super", "outside");
  for (;;) {
    char *paths[MAX_PATHS];
    char *name, *p;
    int npaths = 0;
    struct stats s = {0};

    if (!fgets(line, sizeof(line), fp)) break;
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0') continue;
    name = strtok(line, "\t");
    if (!name) continue;
    while ((p = strtok(NULL, "\t")) != NULL && npaths < MAX_PATHS) {
      paths[npaths++] = p;
    }
    if (npaths == 0) continue;
    if (!scan_unit(&s, name, paths, npaths)) {
      fclose(fp);
      mrb_close(mrb);
      return 1;
    }
    print_row(name, &s);
    add(&total, &s);
  }
  fclose(fp);

  printf("%-24s %5s %8s %8s   %-7s %6s %6s %6s %6s\n",
         "", "", "", "", "", "", "", "", "");
  print_row("TOTAL", &total);

  mrb_close(mrb);
  return 0;
}
