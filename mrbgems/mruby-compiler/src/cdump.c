/*
** cdump.c - mruby binary dumper (in C)
**
** See Copyright Notice in mruby.h
*/

#include <stdio.h>
#include <string.h>
#include "../include/mrc_ccontext.h"
#include "../include/mrc_irep.h"
#include "../include/mrc_dump.h"
#include "../include/mrc_debug.h"
#include "../include/mrc_irep_pool_type.h"
#include "../include/mrc_opcode.h"

#ifndef MRC_NO_STDIO

#ifndef MRC_NO_FLOAT
//#include <../include/endian.h>
#define MRC_FLOAT_FMT "%.17g"
#endif

#define ISALPHA(c) ((((unsigned)(c) | 0x20) - 'a') < 26)
#define ISDIGIT(c) (((unsigned)(c) - '0') < 10)
#define ISALNUM(c) (ISALPHA(c) || ISDIGIT(c))

typedef struct mrc_string {
  char *ptr;
  size_t len;
  size_t capa;
} mrc_string;

static mrc_string*
mrc_str_new_capa(mrc_ccontext *c, size_t capa)
{
  mrc_string *s = (mrc_string *)mrc_malloc(c, sizeof(mrc_string));
  if (s) {
    s->ptr = (char *)mrc_calloc(c, 1, capa);
    if (s->ptr) {
      s->len = 0;
      s->capa = capa;
      return s;
    }
    mrc_free(c, s);
  }
  return NULL;
}

static mrc_string*
mrc_str_new(mrc_ccontext *c, const char *ptr, size_t len)
{
  mrc_string *s = mrc_str_new_capa(c, len+1);
  if (s) {
    memcpy(s->ptr, ptr, len);
    s->len = len;
    s->ptr[len] = '\0';
  }
  return s;
}

static mrc_string*
mrc_str_new_cstr(mrc_ccontext *c, const char *cstr)
{
  return mrc_str_new(c, cstr, strlen(cstr));
}

static void
mrc_str_cat_lit(mrc_ccontext *c, mrc_string *s, const char *lit)
{
  size_t len = strlen(lit);
  if (s->len+len+1 > s->capa) {
    s->capa = s->len+len+1;
    s->ptr = (char *)mrc_realloc(c, s->ptr, s->capa);
  }
  memcpy(s->ptr+s->len, lit, len);
  s->len += len;
  s->ptr[s->len] = '\0';
}

static void
mrc_str_cat_cstr(mrc_ccontext *c, mrc_string *s, const char *cstr)
{
  if (!cstr) return;
  mrc_str_cat_lit(c, s, cstr);
}

static void
mrc_str_cat_str(mrc_ccontext *c, mrc_string *s, mrc_string *s2)
{
  if (s->len+s2->len+1 > s->capa) {
    s->capa = s->len+s2->len+1;
    s->ptr = (char *)mrc_realloc(c, s->ptr, s->capa);
  }
  memcpy(s->ptr+s->len, s2->ptr, s2->len);
  s->len += s2->len;
  s->ptr[s->len] = '\0';
}

static void
mrc_str_free(mrc_ccontext *c, mrc_string *s)
{
  mrc_free(c, s->ptr);
  mrc_free(c, s);
}

static mrc_string*
mrc_str_escape(mrc_ccontext *c, mrc_string *s)
{
  mrc_string *s2 = mrc_str_new_capa(c, s->len*2+1);
  mrc_str_cat_lit(c, s2, "\"");
  if (s2) {
    for (size_t i=0; i<s->len; i++) {
      char ch[2] = {s->ptr[i], '\0'};
      if (ch[0] == '"' || ch[0] == '\\') {
        mrc_str_cat_lit(c, s2, "\\");
      }
      mrc_str_cat_lit(c, s2, ch);
    }
  }
  mrc_str_cat_lit(c, s2, "\"");
  return s2;
}

#define MRC_STRING_PTR(s) ((s)->ptr)
#define MRC_STRING_LEN(s) ((s)->len)

static int
cdump_pool(mrc_ccontext *c, const mrc_pool_value *p, FILE *fp)
{
  if (p->tt & IREP_TT_NFLAG) {  /* number */
    switch (p->tt) {
#ifdef MRC_64BIT
    case IREP_TT_INT64:
      if (p->u.i64 < INT32_MIN || INT32_MAX < p->u.i64) {
        fprintf(fp, "{IREP_TT_INT64, {.i64=%" PRId64 "}},\n", p->u.i64);
      }
      else {
        fprintf(fp, "{IREP_TT_INT32, {.i32=%" PRId32 "}},\n", (int32_t)p->u.i64);
      }
      break;
#endif
    case IREP_TT_INT32:
      fprintf(fp, "{IREP_TT_INT32, {.i32=%" PRId32 "}},\n", p->u.i32);
      break;
    case IREP_TT_FLOAT:
#ifndef MRC_NO_FLOAT
      fprintf(fp, "{IREP_TT_FLOAT, {.f=" MRC_FLOAT_FMT "}},\n", p->u.f);
#endif
      break;
    case IREP_TT_BIGINT:
      {
        const char *s = p->u.str;
        int len = s[0]+2;
        fputs("{IREP_TT_BIGINT, {\"", fp);
        for (int i=0; i<len; i++) {
          fprintf(fp, "\\x%02x", (int)s[i]&0xff);
        }
        fputs("\"}},\n", fp);
      }
      break;
    }
  }
  else {                        /* string */
    int i, len = p->tt>>2;
    const char *s = p->u.str;
    fprintf(fp, "{IREP_TT_STR|(%d<<2), {\"", len);
    for (i=0; i<len; i++) {
      fprintf(fp, "\\x%02x", (int)s[i]&0xff);
    }
    fputs("\"}},\n", fp);
  }
  return MRC_DUMP_OK;
}

static mrc_bool
sym_name_word_p(const char *name, mrc_int len)
{
  if (len == 0) return FALSE;
  if (name[0] != '_' && !ISALPHA(name[0])) return FALSE;
  for (int i = 1; i < len; i++) {
    if (name[i] != '_' && !ISALNUM(name[i])) return FALSE;
  }
  return TRUE;
}

static mrc_bool
sym_name_with_equal_p(const char *name, mrc_int len)
{
  return len >= 2 && name[len-1] == '=' && sym_name_word_p(name, len-1);
}

static mrc_bool
sym_name_with_question_mark_p(const char *name, mrc_int len)
{
  return len >= 2 && name[len-1] == '?' && sym_name_word_p(name, len-1);
}

static mrc_bool
sym_name_with_bang_p(const char *name, mrc_int len)
{
  return len >= 2 && name[len-1] == '!' && sym_name_word_p(name, len-1);
}

static mrc_bool
sym_name_ivar_p(const char *name, mrc_int len)
{
  return len >= 2 && name[0] == '@' && sym_name_word_p(name+1, len-1);
}

static mrc_bool
sym_name_cvar_p(const char *name, mrc_int len)
{
  return len >= 3 && name[0] == '@' && sym_name_ivar_p(name+1, len-1);
}

#define OPERATOR_SYMBOL(sym_name, name) {name, sym_name, sizeof(sym_name)-1}
struct operator_symbol {
  const char *name;
  const char *sym_name;
  uint16_t sym_name_len;
};
static const struct operator_symbol operator_table[] = {
  OPERATOR_SYMBOL("!", "not"),
  OPERATOR_SYMBOL("%", "mod"),
  OPERATOR_SYMBOL("&", "and"),
  OPERATOR_SYMBOL("*", "mul"),
  OPERATOR_SYMBOL("+", "add"),
  OPERATOR_SYMBOL("-", "sub"),
  OPERATOR_SYMBOL("/", "div"),
  OPERATOR_SYMBOL("<", "lt"),
  OPERATOR_SYMBOL(">", "gt"),
  OPERATOR_SYMBOL("^", "xor"),
  OPERATOR_SYMBOL("`", "tick"),
  OPERATOR_SYMBOL("|", "or"),
  OPERATOR_SYMBOL("~", "neg"),
  OPERATOR_SYMBOL("!=", "neq"),
  OPERATOR_SYMBOL("!~", "nmatch"),
  OPERATOR_SYMBOL("&&", "andand"),
  OPERATOR_SYMBOL("**", "pow"),
  OPERATOR_SYMBOL("+@", "plus"),
  OPERATOR_SYMBOL("-@", "minus"),
  OPERATOR_SYMBOL("<<", "lshift"),
  OPERATOR_SYMBOL("<=", "le"),
  OPERATOR_SYMBOL("==", "eq"),
  OPERATOR_SYMBOL("=~", "match"),
  OPERATOR_SYMBOL(">=", "ge"),
  OPERATOR_SYMBOL(">>", "rshift"),
  OPERATOR_SYMBOL("[]", "aref"),
  OPERATOR_SYMBOL("||", "oror"),
  OPERATOR_SYMBOL("<=>", "cmp"),
  OPERATOR_SYMBOL("===", "eqq"),
  OPERATOR_SYMBOL("[]=", "aset"),
};

static const char*
sym_operator_name(const char *sym_name, mrc_int len)
{
  mrc_sym table_size = sizeof(operator_table)/sizeof(struct operator_symbol);
  if (operator_table[table_size-1].sym_name_len < len) return NULL;

  for (mrc_sym start = 0; table_size != 0; table_size/=2) {
    mrc_sym idx = start+table_size/2;
    const struct operator_symbol *op_sym = &operator_table[idx];
    int cmp = (int)len-(int)op_sym->sym_name_len;
    if (cmp == 0) {
      cmp = memcmp(sym_name, op_sym->sym_name, len);
      if (cmp == 0) return op_sym->name;
    }
    if (0 < cmp) {
      start = ++idx;
      table_size--;
    }
  }
  return NULL;
}

static mrc_string*
sym_var_name_str(mrc_ccontext *c, const char *initname, const char *key, int n)
{
  char buf[32];
  mrc_string *s = mrc_str_new_cstr(c, initname);
  mrc_str_cat_lit(c, s, "_");
  mrc_str_cat_cstr(c, s, key);
  mrc_str_cat_lit(c, s, "_");
  snprintf(buf, sizeof(buf), "%d", n);
  mrc_str_cat_cstr(c, s, buf);
  return s;
}

static int
cdump_sym(mrc_ccontext *c, mrc_sym sym, const char *var_name, int idx, mrc_string *init_syms_code, FILE *fp)
{
  if (sym == 0) {
    fputs("0,", fp);
    return MRC_DUMP_OK;
  }

  const pm_constant_t *constant = pm_constant_pool_id_to_constant(&c->p->constant_pool, sym);
  mrc_string *name_obj = mrc_str_new(c, (const char *)constant->start, constant->length);
  const char *name = MRC_STRING_PTR(name_obj);
  const char *op_name;
  mrc_int len = constant->length;

  if (*name == '\0') {
    mrc_str_free(c, name_obj);
    return MRC_DUMP_INVALID_ARGUMENT;
  }
  if (sym_name_word_p(name, len)) {
    fprintf(fp, "MRB_SYM(%s)", name);
  }
  else if (sym_name_with_equal_p(name, len)) {
    fprintf(fp, "MRB_SYM_E(%.*s)", (int)(len-1), name);
  }
  else if (sym_name_with_question_mark_p(name, len)) {
    fprintf(fp, "MRB_SYM_Q(%.*s)", (int)(len-1), name);
  }
  else if (sym_name_with_bang_p(name, len)) {
    fprintf(fp, "MRB_SYM_B(%.*s)", (int)(len-1), name);
  }
  else if (sym_name_ivar_p(name, len)) {
    fprintf(fp, "MRB_IVSYM(%s)", name+1);
  }
  else if (sym_name_cvar_p(name, len)) {
    fprintf(fp, "MRB_CVSYM(%s)", name+2);
  }
  else if ((op_name = sym_operator_name(name, len))) {
    fprintf(fp, "MRB_OPSYM(%s)", op_name);
  }
  else {
    char buf[32];
    mrc_string *name_obj = mrc_str_new(c, name, len);
    mrc_str_cat_lit(c, init_syms_code, "  ");
    mrc_str_cat_cstr(c, init_syms_code, var_name);
    snprintf(buf, sizeof(buf), "[%d] = ", idx);
    mrc_str_cat_cstr(c, init_syms_code, buf);
    mrc_str_cat_lit(c, init_syms_code, "mrb_intern_lit(mrb, ");
    mrc_string *escaped = mrc_str_escape(c, name_obj);
    mrc_str_free(c, name_obj);
    mrc_str_cat_str(c, init_syms_code, escaped);
    mrc_str_free(c, escaped);
    mrc_str_cat_lit(c, init_syms_code, ");\n");
    fputs("0", fp);
  }
  fputs(", ", fp);
  mrc_str_free(c, name_obj);
  return MRC_DUMP_OK;
}

static int
cdump_syms(mrc_ccontext *c, const char *name, const char *key, int n, int syms_len, const mrc_sym *syms, mrc_string *init_syms_code, FILE *fp)
{
  int ai = mrc_gc_arena_save(c);
  size_t code_len = MRC_STRING_LEN(init_syms_code);
  mrc_string *var_name = sym_var_name_str(c, name, key, n);

  fprintf(fp, "mrb_DEFINE_SYMS_VAR(%s, %d, (", MRC_STRING_PTR(var_name), syms_len);
  int emitted = 0;
  for (int i=0; i<syms_len; i++) {
    if (cdump_sym(c, syms[i], MRC_STRING_PTR(var_name), i, init_syms_code, fp) == MRC_DUMP_OK) {
      emitted++;
    }
  }
  /* An empty inline list expands to `{}`, which ISO C rejects before C23 (older
     MSVC fails with C2059). Emit a single 0 so the array is validly
     zero-initialized; runtime-interned symbols are still filled by init code. */
  if (emitted == 0) fputs("0", fp);
  mrc_str_free(c, var_name);
  fputs("), ", fp);
  if (code_len == MRC_STRING_LEN(init_syms_code)) fputs("const", fp);
  fputs(");\n", fp);
  mrc_gc_arena_restore(c, ai);
  return MRC_DUMP_OK;
}

//Handle the simple/common case of debug_info:
// - 1 file associated with a single irep
// - mrc_debug_line_ary format only
static int
simple_debug_info(mrc_irep_debug_info *info)
{
  if (!info || info->flen != 1) {
    return 0;
  }
  return 1;
}

//Adds debug information to c-structs and
//adds filenames in init_syms_code block
static int
cdump_debug(mrc_ccontext *c, const char *name, int n, mrc_irep_debug_info *info,
            mrc_string *init_syms_code, FILE *fp)
{
  int ai = mrc_gc_arena_save(c);
  char buffer[256];
  const char *line_type = "mrb_debug_line_ary";

  if (!simple_debug_info(info))
    return MRC_DUMP_INVALID_IREP;

  int len = info->files[0]->line_entry_count;

  const pm_constant_t *fn_constant = pm_constant_pool_id_to_constant(&c->p->constant_pool, info->files[0]->filename_sym);
  const char *filename = (const char *)fn_constant->start;
  snprintf(buffer, sizeof(buffer), "  %s_debug_file_%d.filename_sym = mrb_intern_lit(mrb,", name, n);
  mrc_str_cat_cstr(c, init_syms_code, buffer);
  mrc_string *filename_str = mrc_str_new_cstr(c, filename);
  mrc_string *escaped = mrc_str_escape(c, filename_str);
  mrc_str_free(c, filename_str);
  mrc_str_cat_str(c, init_syms_code, escaped);
  mrc_str_free(c, escaped);
  mrc_str_cat_cstr(c, init_syms_code, ");\n");

  switch (info->files[0]->line_type) {
  case mrc_debug_line_ary:
    fprintf(fp, "static uint16_t %s_debug_lines_%d[%d] = {", name, n, len);
    for (int i=0; i<len; i++) {
      if (i%10 == 0) fputs("\n", fp);
      fprintf(fp, "0x%04x,", info->files[0]->lines.ary[i]);
    }
    fputs("};\n", fp);
    break;

  case mrc_debug_line_flat_map:
    line_type = "mrb_debug_line_flat_map";
    fprintf(fp, "static struct mrb_irep_debug_info_line %s_debug_lines_%d[%d] = {", name, n, len);
    for (int i=0; i<len; i++) {
      const mrc_irep_debug_info_line *fmap = &info->files[0]->lines.flat_map[i];
      fprintf(fp, "\t{.start_pos=0x%04x,.line=%d},\n", fmap->start_pos, fmap->line);
    }
    fputs("};\n", fp);
    break;

  case mrc_debug_line_packed_map:
    line_type = "mrb_debug_line_packed_map";
    fprintf(fp, "static const char %s_debug_lines_%d[] = \"", name, n);
    const uint8_t *pmap = info->files[0]->lines.packed_map;
    for (int i=0; i<len; i++) {
      fprintf(fp, "\\x%02x", pmap[i]&0xff);
    }
    fputs("\";\n", fp);
    break;
  }
  fprintf(fp, "static mrb_irep_debug_info_file %s_debug_file_%d = {\n", name, n);
  fprintf(fp, "%d, %d, %d, %s, {%s_debug_lines_%d}};\n",
          info->files[0]->start_pos,
          info->files[0]->filename_sym,
          info->files[0]->line_entry_count,
          line_type,
          name, n);
  fprintf(fp, "static mrb_irep_debug_info_file *%s_debug_file_%d_ = &%s_debug_file_%d;\n", name, n, name, n);

  fprintf(fp, "static mrb_irep_debug_info %s_debug_%d = {\n", name, n);
  fprintf(fp, "%d, %d, &%s_debug_file_%d_};\n", info->pc_count, info->flen, name, n);

  mrc_gc_arena_restore(c, ai);
  return MRC_DUMP_OK;
}


/* ---- Read-only method tables -------------------------------------------
   A method written in Ruby costs an RProc in the object heap for as long as
   the state lives, even though the irep behind it is dumped as `static const`
   right here. Where the build can say which class a `def` lands on, the proc
   and the method table entry can be `static const` too, and the class gets
   them from a read-only layer (mrb_mt_init_rom()) instead of from OP_TDEF.

   The build can say it when the `def` sits in the body of a class or module
   opened at the top level of the unit, under no outer constant, with a
   superclass that is a constant or absent, and the body holds no branch that
   could make the definition conditional. A body that also opens a nested
   scope keeps its own definitions; the nested one runs as it did.

   What is left in the body still runs: `include`, `alias`, a visibility call
   naming its methods, everything. The definitions it no longer performs are
   replaced in place by the value they left behind (OP_LOADSYM of the method
   name, padded to the same width), so nothing about the body's length, its
   jump targets or its result changes.

   A body calling `private` with no arguments is left alone entirely: that
   call sets the visibility of the definitions that follow it, which a table
   written before the body runs cannot know. */

/* instruction sizes, the way codegen.c builds them */
#undef Z
#undef S
#undef W
#define Z 1
#define S 3
#define W 4
#define OPCODE(_,x) x,
static const uint8_t rom_insn_size[] = {
#define B 2
#define BB 3
#define BBB 4
#define BS 4
#define BSS 6
#include "mrc_ops.h"
#undef B
#undef BB
#undef BBB
#undef BS
#undef BSS
};
static const uint8_t rom_insn_size1[] = {
#define B 3
#define BB 4
#define BBB 5
#define BS 5
#define BSS 7
#include "mrc_ops.h"
#undef B
#undef BB
#undef BBB
#undef BS
#undef BSS
};
static const uint8_t rom_insn_size2[] = {
#define B 2
#define BB 3
#define BBB 4
#define BS 4
#define BSS 6
#include "mrc_ops.h"
#undef B
#undef BB
#undef BBB
#undef BS
#undef BSS
};
static const uint8_t rom_insn_size3[] = {
#define B 3
#define BB 5
#define BBB 6
#define BS 5
#define BSS 7
#include "mrc_ops.h"
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

static uint32_t
rom_insn_len(const mrc_code *pc)
{
  switch (pc[0]) {
  case OP_EXT1: return (uint32_t)rom_insn_size1[pc[1]] + 1;
  case OP_EXT2: return (uint32_t)rom_insn_size2[pc[1]] + 1;
  case OP_EXT3: return (uint32_t)rom_insn_size3[pc[1]] + 1;
  default: return rom_insn_size[pc[0]];
  }
}

#define ROM_MAX_METHODS 192
#define ROM_MAX_GROUPS 96
#define ROM_SYM_EXPR_MAX 128

typedef struct rom_method {
  mrc_sym name;
  uint32_t pos;                 /* where the definition sits in the body */
  int rep;                      /* the body's rep holding the method */
  mrc_bool singleton;
} rom_method;

typedef struct rom_group {
  const mrc_irep *body;
  mrc_sym cname;
  mrc_sym super;                /* 0 where the source names none */
  mrc_bool module;
  int nmethods;
  rom_method methods[ROM_MAX_METHODS];
} rom_group;

typedef struct rom_plan {
  int ngroups;
  rom_group groups[ROM_MAX_GROUPS];
} rom_plan;

/* The symbol as a compile-time constant, or FALSE where it has no such
   spelling and the dump interns it at startup instead. A read-only table
   holds only the former. */
static mrc_bool
rom_sym_expr(mrc_ccontext *c, mrc_sym sym, char *buf, size_t size)
{
  const pm_constant_t *constant;
  const char *name, *op_name;
  mrc_int len;

  if (sym == 0) return FALSE;
  constant = pm_constant_pool_id_to_constant(&c->p->constant_pool, sym);
  name = (const char *)constant->start;
  len = (mrc_int)constant->length;
  if (len == 0 || (size_t)len + 16 > size) return FALSE;

  if (sym_name_word_p(name, len)) {
    snprintf(buf, size, "MRB_SYM(%.*s)", (int)len, name);
  }
  else if (sym_name_with_equal_p(name, len)) {
    snprintf(buf, size, "MRB_SYM_E(%.*s)", (int)(len-1), name);
  }
  else if (sym_name_with_question_mark_p(name, len)) {
    snprintf(buf, size, "MRB_SYM_Q(%.*s)", (int)(len-1), name);
  }
  else if (sym_name_with_bang_p(name, len)) {
    snprintf(buf, size, "MRB_SYM_B(%.*s)", (int)(len-1), name);
  }
  else if ((op_name = sym_operator_name(name, len)) != NULL) {
    snprintf(buf, size, "MRB_OPSYM(%s)", op_name);
  }
  else {
    return FALSE;
  }
  return TRUE;
}

static mrc_bool
rom_sym_named_p(mrc_ccontext *c, mrc_sym sym, const char *name)
{
  const pm_constant_t *constant;
  size_t len = strlen(name);

  if (sym == 0) return FALSE;
  constant = pm_constant_pool_id_to_constant(&c->p->constant_pool, sym);
  return constant->length == len &&
         memcmp(constant->start, name, len) == 0;
}

/* `private` and its neighbours with no argument: the call sets the visibility
   of what is defined after it. */
static mrc_bool
rom_visibility_scope_p(mrc_ccontext *c, const mrc_irep *irep, const mrc_code *pc)
{
  mrc_sym mid;

  switch (pc[0]) {
  case OP_SEND0: case OP_SSEND0:
    mid = irep->syms[pc[2]];
    break;
  case OP_SEND: case OP_SSEND:
    if (pc[3] != 0) return FALSE;
    mid = irep->syms[pc[2]];
    break;
  default:
    return FALSE;
  }
  return rom_sym_named_p(c, mid, "private") ||
         rom_sym_named_p(c, mid, "public") ||
         rom_sym_named_p(c, mid, "protected") ||
         rom_sym_named_p(c, mid, "module_function");
}

static mrc_bool
rom_branch_p(uint8_t op)
{
  switch (op) {
  case OP_JMP: case OP_JMPIF: case OP_JMPNOT: case OP_JMPNIL:
  case OP_JMPUW: case OP_EXCEPT: case OP_RESCUE:
    return TRUE;
  default:
    return FALSE;
  }
}

/* Collects the definitions a class or module body hands over, or FALSE when
   the body keeps them all. */
static mrc_bool
rom_scan_body(mrc_ccontext *c, const mrc_irep *body, rom_group *g)
{
  const mrc_code *pc = body->iseq, *end = body->iseq + body->ilen;
  const mrc_code *prev = NULL;
  char buf[ROM_SYM_EXPR_MAX];

  g->nmethods = 0;
  while (pc < end) {
    uint8_t op = *pc;

    if (rom_branch_p(op) || rom_visibility_scope_p(c, body, pc)) return FALSE;
    if (op == OP_DEF) return FALSE;   /* the receiver is a register, not self */
    if (op == OP_TDEF || op == OP_SDEF) {
      rom_method *m;
      int i;

      /* `def obj.name` puts the singleton of something else in the register */
      if (op == OP_SDEF && (prev == NULL || prev[0] != OP_LOADSELF || prev[1] != pc[1])) {
        return FALSE;
      }
      if (g->nmethods >= ROM_MAX_METHODS) return FALSE;
      m = &g->methods[g->nmethods];
      m->name = body->syms[pc[2]];
      m->rep = pc[3];
      m->singleton = (op == OP_SDEF);
      m->pos = (uint32_t)(pc - body->iseq);
      if (m->rep >= body->rlen) return FALSE;
      if (!rom_sym_expr(c, m->name, buf, sizeof(buf))) return FALSE;
      /* the same name twice would leave two entries in one table */
      for (i = 0; i < g->nmethods; i++) {
        if (g->methods[i].name == m->name && g->methods[i].singleton == m->singleton) {
          return FALSE;
        }
      }
      g->nmethods++;
    }
    prev = pc;
    pc += rom_insn_len(pc);
  }
  return g->nmethods > 0;
}

/* What a register holds, to the one precision the tests below need. */
enum { ROM_R_OTHER, ROM_R_NIL, ROM_R_CONST };

static void
rom_scan_top(mrc_ccontext *c, const mrc_irep *top, rom_plan *plan)
{
  uint8_t kind[256];
  mrc_sym cname[256];
  const mrc_code *pc = top->iseq, *end = top->iseq + top->ilen;
  mrc_sym pending_name = 0, pending_super = 0;
  mrc_bool pending_ok = FALSE, pending_module = FALSE;
  char buf[ROM_SYM_EXPR_MAX];

  memset(kind, ROM_R_OTHER, sizeof(kind));
  memset(cname, 0, sizeof(cname));
  plan->ngroups = 0;

  while (pc < end) {
    uint8_t op = *pc;

    switch (op) {
    case OP_LOADNIL:
      kind[pc[1]] = ROM_R_NIL;
      break;
    case OP_GETCONST:
      kind[pc[1]] = ROM_R_CONST;
      cname[pc[1]] = top->syms[pc[2]];
      break;
    case OP_CLASS:
    case OP_MODULE:
      pending_module = (op == OP_MODULE);
      pending_name = top->syms[pc[2]];
      pending_super = 0;
      /* only a body opened directly under the file's own scope: an outer
         constant would have to be resolved here to name the class */
      pending_ok = (kind[pc[1]] == ROM_R_NIL) &&
                   rom_sym_expr(c, pending_name, buf, sizeof(buf));
      if (pending_ok && !pending_module) {
        if (kind[pc[1]+1] == ROM_R_CONST) {
          pending_super = cname[pc[1]+1];
          pending_ok = rom_sym_expr(c, pending_super, buf, sizeof(buf));
        }
        else if (kind[pc[1]+1] != ROM_R_NIL) {
          pending_ok = FALSE;
        }
      }
      kind[pc[1]] = ROM_R_CONST;
      cname[pc[1]] = pending_name;
      break;
    case OP_EXEC:
      if (pending_ok && plan->ngroups < ROM_MAX_GROUPS && pc[2] < top->rlen) {
        rom_group *g = &plan->groups[plan->ngroups];
        g->body = top->reps[pc[2]];
        g->cname = pending_name;
        g->super = pending_super;
        g->module = pending_module;
        if (rom_scan_body(c, g->body, g)) plan->ngroups++;
      }
      pending_ok = FALSE;
      kind[pc[1]] = ROM_R_OTHER;
      break;
    default:
      if (op != OP_EXT1 && op != OP_EXT2 && op != OP_EXT3 && rom_insn_size[op] >= 2) {
        kind[pc[1]] = ROM_R_OTHER;
      }
      break;
    }
    pc += rom_insn_len(pc);
  }
}

static const rom_group*
rom_group_of(const rom_plan *plan, const mrc_irep *irep)
{
  int i;

  if (plan == NULL) return NULL;
  for (i = 0; i < plan->ngroups; i++) {
    if (plan->groups[i].body == irep) return &plan->groups[i];
  }
  return NULL;
}

static int
rom_group_index(const rom_plan *plan, const rom_group *g)
{
  return (int)(g - plan->groups);
}

/* The procs and the tables of one group, and the code that installs them. */
static void
rom_dump_group(mrc_ccontext *c, const rom_plan *plan, const rom_group *g,
               const char *name, int max, mrc_string *rom_code, FILE *fp)
{
  int gi = rom_group_index(plan, g);
  char buf[ROM_SYM_EXPR_MAX], line[512];
  int i, kinds;

  for (i = 0; i < g->nmethods; i++) {
    fprintf(fp, "mrb_alignas(8)\n"
                "static const struct RProc %s_romproc_%d_%d = {\n"
                "  NULL,MRB_TT_PROC,MRB_GC_RED,MRB_OBJ_IS_FROZEN,\n"
                "  MRB_PROC_SCOPE|MRB_PROC_STRICT,{&%s_irep_%d},NULL,{NULL}\n"
                "};\n",
            name, gi, i, name, max + g->methods[i].rep);
  }

  for (kinds = 0; kinds < 2; kinds++) {
    mrc_bool singleton = (kinds == 1);
    int count = 0;

    for (i = 0; i < g->nmethods; i++) {
      if (g->methods[i].singleton != singleton) continue;
      if (count == 0) {
        fprintf(fp, "static const mrb_mt_entry %s_rom%s_%d[] = {\n",
                name, singleton ? "s" : "", gi);
      }
      rom_sym_expr(c, g->methods[i].name, buf, sizeof(buf));
      fprintf(fp, "  MRB_MT_PROC_ENTRY(&%s_romproc_%d_%d, %s, MRB_METHOD_PUBLIC_FL),\n",
              name, gi, i, buf);
      count++;
    }
    if (count > 0) fputs("};\n", fp);
  }

  mrc_str_cat_lit(c, rom_code, "  {\n    struct RClass *c = ");
  rom_sym_expr(c, g->cname, buf, sizeof(buf));
  if (g->module) {
    snprintf(line, sizeof(line),
             "mrb_vm_define_module(mrb, mrb_obj_value(mrb->object_class), %s);\n", buf);
  }
  else if (g->super == 0) {
    snprintf(line, sizeof(line),
             "mrb_vm_define_class(mrb, mrb_obj_value(mrb->object_class), mrb_nil_value(), %s);\n",
             buf);
  }
  else {
    char sbuf[ROM_SYM_EXPR_MAX];
    rom_sym_expr(c, g->super, sbuf, sizeof(sbuf));
    snprintf(line, sizeof(line),
             "mrb_vm_define_class(mrb, mrb_obj_value(mrb->object_class),\n"
             "      mrb_obj_value(mrb_class_get_id(mrb, %s)), %s);\n", sbuf, buf);
  }
  mrc_str_cat_cstr(c, rom_code, line);

  for (kinds = 0; kinds < 2; kinds++) {
    mrc_bool singleton = (kinds == 1);
    int count = 0;

    for (i = 0; i < g->nmethods; i++) {
      if (g->methods[i].singleton == singleton) count++;
    }
    if (count == 0) continue;
    if (singleton) {
      snprintf(line, sizeof(line),
               "    mrb_mt_init_rom(mrb, mrb_singleton_class_ptr(mrb, mrb_obj_value(c)),\n"
               "                    %s_roms_%d, %d);\n", name, gi, count);
    }
    else {
      snprintf(line, sizeof(line),
               "    mrb_mt_init_rom(mrb, c, %s_rom_%d, %d);\n", name, gi, count);
    }
    mrc_str_cat_cstr(c, rom_code, line);
  }
  mrc_str_cat_lit(c, rom_code, "  }\n");
}

static int
cdump_irep_struct(mrc_ccontext *c, const mrc_irep *irep, uint8_t flags, FILE *fp, const char *name, int n, mrc_string *init_syms_code, int *mp, const rom_plan *plan, mrc_string *rom_code)
{
  int i, len;
  int max = *mp;
  int debug_available = 0;
  const rom_group *group = rom_group_of(plan, irep);

  /* dump reps */
  if (0 < irep->rlen) {
    for (i=0,len=irep->rlen; i<len; i++) {
      *mp += len;
      if (cdump_irep_struct(c, irep->reps[i], flags, fp, name, max+i, init_syms_code, mp, plan, rom_code) != MRC_DUMP_OK)
        return MRC_DUMP_INVALID_ARGUMENT;
    }
    /* `const` on the pointers as well as on what they point at: `mrb_irep`
       declares `reps` as a pointer to const pointers, and without the second
       one the array is writable data rather than read-only, which on a target
       that runs from flash is RAM it never needs. */
    fprintf(fp,   "static const mrb_irep *const %s_reps_%d[%d] = {\n", name, n, len);
    for (i=0,len=irep->rlen; i<len; i++) {
      fprintf(fp,   "  &%s_irep_%d,\n", name, max+i);
    }
    fputs("};\n", fp);
  }
  /* dump pool */
  if (0 < irep->plen) {
    len=irep->plen;
    fprintf(fp,   "static const mrb_irep_pool %s_pool_%d[%d] = {\n", name, n, len);
    for (i=0; i<len; i++) {
      if (cdump_pool(c, &irep->pool[i], fp) != MRC_DUMP_OK)
        return MRC_DUMP_INVALID_ARGUMENT;
    }
    fputs("};\n", fp);
  }
  /* dump syms */
  if (0 < irep->slen) {
    cdump_syms(c, name, "syms", n, irep->slen, irep->syms, init_syms_code, fp);
  }
  /* dump iseq */
  len=irep->ilen+sizeof(struct mrc_irep_catch_handler)*irep->clen;
  fprintf(fp,   "static const mrb_code %s_iseq_%d[%d] = {", name, n, len);
  for (i=0; i<len; i++) {
    mrc_code byte = irep->iseq[i];
    if (group) {
      /* OP_TDEF and OP_SDEF are four bytes and both leave the method name in
         R[a]; OP_LOADSYM writing the same name from the same symbol index is
         three, so one OP_NOP pads it back. The body keeps its length, its
         jump targets and its result, and performs no definition. */
      int k;
      for (k = 0; k < group->nmethods; k++) {
        uint32_t pos = group->methods[k].pos;
        if ((uint32_t)i == pos) byte = OP_LOADSYM;
        else if ((uint32_t)i == pos+3) byte = OP_NOP;
      }
    }
    if (i%20 == 0) fputs("\n", fp);
    fprintf(fp, "0x%02x,", byte);
  }
  fputs("};\n", fp);
  /* dump lv */
  if (irep->lv) {
    cdump_syms(c, name, "lv", n, irep->nlocals-1, irep->lv, init_syms_code, fp);
  }
  /* dump debug */
  if (flags & MRC_DUMP_DEBUG_INFO) {
    if (cdump_debug(c, name, n, irep->debug_info, init_syms_code, fp) == MRC_DUMP_OK) {
      debug_available = 1;
    }
  }


  /* dump irep */
  fprintf(fp, "static const mrb_irep %s_irep_%d = {\n", name, n);
  fprintf(fp,   "  %d,%d,%d,\n", irep->nlocals, irep->nregs, irep->clen);
  fprintf(fp,   "  MRB_IREP_STATIC,%s_iseq_%d,\n", name, n);
  if (0 < irep->plen) {
    fprintf(fp, "  %s_pool_%d,", name, n);
  }
  else {
    fputs(      "  NULL,", fp);
  }
  if (0 < irep->slen) {
    fprintf(fp, "%s_syms_%d,", name, n);
  }
  else {
    fputs(      "NULL,", fp);
  }
  if (0 < irep->rlen) {
    fprintf(fp, "%s_reps_%d,\n", name, n);
  }
  else {
    fputs(      "NULL,\n", fp);
  }
  if (irep->lv) {
    fprintf(fp, "  %s_lv_%d,\n", name, n);
  }
  else {
    fputs(      "  NULL,\t\t\t\t\t/* lv */\n", fp);
  }
  if (debug_available) {
    fprintf(fp, "  &%s_debug_%d,\n", name, n);
  }
  else {
    fputs("  NULL,\t\t\t\t\t/* debug_info */\n", fp);
  }
  fprintf(fp,   "  %d,%d,%d,%d,0\n};\n", irep->ilen, irep->plen, irep->slen, irep->rlen);

  if (group) {
    rom_dump_group(c, plan, group, name, max, rom_code, fp);
  }

  return MRC_DUMP_OK;
}

int
mrc_dump_irep_cstruct(mrc_ccontext *c, const mrc_irep *irep, uint8_t flags, FILE *fp, const char *initname)
{
  if (fp == NULL || initname == NULL || initname[0] == '\0') {
    return MRC_DUMP_INVALID_ARGUMENT;
  }
  if (fprintf(fp, "#include <mruby.h>\n"
                  "#include <mruby/irep.h>\n"
                  "#include <mruby/debug.h>\n"
                  "#include <mruby/proc.h>\n"
                  "#include <mruby/presym.h>\n"
                  "\n") < 0) {
    return MRC_DUMP_WRITE_FAULT;
  }
  fputs("#define mrb_BRACED(...) {__VA_ARGS__}\n", fp);
  fputs("#define mrb_DEFINE_SYMS_VAR(name, len, syms, qualifier) \\\n", fp);
  fputs("  static qualifier mrb_sym name[len] = mrb_BRACED syms\n", fp);
  fputs("\n", fp);

  /* Which definitions the classes here hand over to a read-only table. The
     headers those tables need are only pulled in where there are any. */
  rom_plan *plan = (rom_plan *)mrc_malloc(c, sizeof(rom_plan));
  if (plan == NULL) return MRC_DUMP_GENERAL_FAILURE;
  plan->ngroups = 0;
  if (flags & MRC_DUMP_ROM_METHODS) rom_scan_top(c, irep, plan);
  if (0 < plan->ngroups) {
    fputs("#include <mruby/class.h>\n"
          "#include <mruby/internal.h>\n"
          "\n", fp);
  }

  mrc_string *init_syms_code = mrc_str_new_capa(c, 1);
  mrc_string *rom_code = mrc_str_new_capa(c, 1);
  int max = 1;
  int n = cdump_irep_struct(c, irep, flags, fp, initname, 0, init_syms_code, &max, plan, rom_code);
  if (n != MRC_DUMP_OK) return n;
  fprintf(fp,
          "%s\n"
          "const struct RProc %s[] = {{\n",
          (flags & MRC_DUMP_STATIC) ? "static"
                                    : "#ifdef __cplusplus\n"
                                      "extern\n"
                                      "#endif",
          initname);
  fprintf(fp, "NULL,MRB_TT_PROC,MRB_GC_RED,MRB_OBJ_IS_FROZEN,0,{&%s_irep_0},NULL,{NULL},\n}};\n", initname);
  fputs("static void\n", fp);
  fprintf(fp, "%s_init_syms(mrb_state *mrb)\n", initname);
  fputs("{\n", fp);
  fputs(MRC_STRING_PTR(init_syms_code), fp);
  fputs("}\n", fp);
  if (flags & MRC_DUMP_ROM_METHODS) {
    /* Its own function because of when it has to run: after whatever defines
       the classes these bodies reopen, and before the proc, so that a body
       finds its own methods already there when it aliases one or names one in
       a visibility call. */
    fputs("static void\n", fp);
    fprintf(fp, "%s_init_rom(mrb_state *mrb)\n", initname);
    fputs("{\n", fp);
    fputs(MRC_STRING_PTR(rom_code), fp);
    fputs("}\n", fp);
  }
  mrc_str_free(c, init_syms_code);
  mrc_str_free(c, rom_code);
  mrc_free(c, plan);
  return MRC_DUMP_OK;
}
#endif /* MRC_NO_STDIO */
