/*
** rom_proc.c - helpers for test/t/rom_proc.rb
**
** A method whose proc is built into the binary reads the class it was defined
** in through the callinfo, because a `const` proc has nowhere to carry it (see
** mrb_vm_frame_class() in src/vm.c). Only a handful of core methods are built
** that way today, and none of them asks for that class, so the classes here
** are the ones that do: `super`, a lexical constant, a class variable, and
** what `defined?` answers about the last two.
**
** The procs stand for what a build-time transformation over `mrblib` would
** emit. The Ruby they were compiled from is written beside each one; the
** bytecode is small enough to keep here rather than generate.
**
** The symbol tables are filled at init rather than written as presymbols: the
** presymbol scan reads the objects libmruby is built from, and a file that
** only the test binary compiles is not among them. What the test is about is
** the proc, and the proc is `const` either way.
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/irep.h>
#include <mruby/opcode.h>
#include <mruby/proc.h>
#include <mruby/variable.h>

void mrb_init_test_rom_proc(mrb_state *mrb);


/* RomProcBase#greet -> "base" */
static const mrb_irep_pool base_greet_pool[1] = {
  {IREP_TT_STR|(4<<2), {"base"}},
};
static const mrb_code base_greet_iseq[] = {
  OP_ENTER, 0x00, 0x00, 0x00,    /* 000 ENTER  0:0:0:0:0:0:0:0 */
  OP_STRING, 2, 0,               /* 004 STRING R2  L[0]        */
  OP_RETURN, 2                   /* 007 RETURN R2              */
};
static const mrb_sym base_greet_lv[1] = {0};
static const mrb_irep base_greet_irep = {
  2, 3, 0, MRB_IREP_STATIC, base_greet_iseq,
  base_greet_pool, NULL, NULL, base_greet_lv, NULL,
  sizeof(base_greet_iseq), 1, 0, 0, 0
};

/* RomProcChild#greet -> "child->" + super */
static const mrb_irep_pool child_greet_pool[1] = {
  {IREP_TT_STR|(7<<2), {"child->"}},
};
static const mrb_code child_greet_iseq[] = {
  OP_ENTER, 0x00, 0x00, 0x00,    /* 000 ENTER  0:0:0:0:0:0:0:0 */
  OP_STRING, 2, 0,               /* 004 STRING R2  L[0]        */
  OP_MOVE, 4, 1,                 /* 007 MOVE   R4  R1          */
  OP_SUPER, 3, 0,                /* 010 SUPER  R3  n=0         */
  OP_ADD, 2,                     /* 013 ADD    R2  (R3)        */
  OP_RETURN, 2                   /* 015 RETURN R2              */
};
static const mrb_sym child_greet_lv[1] = {0};
static const mrb_irep child_greet_irep = {
  2, 5, 0, MRB_IREP_STATIC, child_greet_iseq,
  child_greet_pool, NULL, NULL, child_greet_lv, NULL,
  sizeof(child_greet_iseq), 1, 0, 0, 0
};

/* RomProcChild#lexical -> ROM_PROC_CONST */
static mrb_sym lexical_syms[1];  /* :ROM_PROC_CONST */
static const mrb_code lexical_iseq[] = {
  OP_ENTER, 0x00, 0x00, 0x00,    /* 000 ENTER    0:0:0:0:0:0:0:0     */
  OP_GETCONST, 2, 0,             /* 004 GETCONST R2  ROM_PROC_CONST  */
  OP_RETURN, 2                   /* 007 RETURN   R2                  */
};
static const mrb_sym lexical_lv[1] = {0};
static const mrb_irep lexical_irep = {
  2, 3, 0, MRB_IREP_STATIC, lexical_iseq,
  NULL, lexical_syms, NULL, lexical_lv, NULL,
  sizeof(lexical_iseq), 0, 1, 0, 0
};

/* RomProcChild#cvar_bump -> @@rom_proc_count += 1 */
static mrb_sym cvar_syms[1];     /* :@@rom_proc_count */
static const mrb_code cvar_iseq[] = {
  OP_ENTER, 0x00, 0x00, 0x00,    /* 000 ENTER  0:0:0:0:0:0:0:0        */
  OP_GETCV, 2, 0,                /* 004 GETCV  R2  @@rom_proc_count   */
  OP_ADDI, 2, 1,                 /* 007 ADDI   R2  1                  */
  OP_SETCV, 2, 0,                /* 010 SETCV  @@rom_proc_count  R2   */
  OP_RETURN, 2                   /* 013 RETURN R2                     */
};
static const mrb_sym cvar_lv[1] = {0};
static const mrb_irep cvar_irep = {
  2, 5, 0, MRB_IREP_STATIC, cvar_iseq,
  NULL, cvar_syms, NULL, cvar_lv, NULL,
  sizeof(cvar_iseq), 0, 1, 0, 0
};

/* RomProcChild#defined_const -> defined?(ROM_PROC_CONST) */
static mrb_sym dconst_syms[2];   /* :ROM_PROC_CONST, :__defined_const? */
static const mrb_code dconst_iseq[] = {
  OP_ENTER, 0x00, 0x00, 0x00,    /* 000 ENTER    0:0:0:0:0:0:0:0            */
  OP_LOADSELF, 2,                /* 004 LOADSELF R2  (R0)                   */
  OP_LOADSYM, 3, 0,              /* 006 LOADSYM  R3  :ROM_PROC_CONST        */
  OP_SSEND, 2, 1, 1,             /* 009 SSEND    R2  :__defined_const?  n=1 */
  OP_RETURN, 2                   /* 013 RETURN   R2                         */
};
static const mrb_sym dconst_lv[1] = {0};
static const mrb_irep dconst_irep = {
  2, 5, 0, MRB_IREP_STATIC, dconst_iseq,
  NULL, dconst_syms, NULL, dconst_lv, NULL,
  sizeof(dconst_iseq), 0, 2, 0, 0
};

/* RomProcChild#defined_cvar -> defined?(@@rom_proc_count) */
static mrb_sym dcvar_syms[2];    /* :@@rom_proc_count, :__defined_cvar? */
static const mrb_code dcvar_iseq[] = {
  OP_ENTER, 0x00, 0x00, 0x00,    /* 000 ENTER    0:0:0:0:0:0:0:0           */
  OP_LOADSELF, 2,                /* 004 LOADSELF R2  (R0)                  */
  OP_LOADSYM, 3, 0,              /* 006 LOADSYM  R3  :@@rom_proc_count     */
  OP_SSEND, 2, 1, 1,             /* 009 SSEND    R2  :__defined_cvar?  n=1 */
  OP_RETURN, 2                   /* 013 RETURN   R2                        */
};
static const mrb_sym dcvar_lv[1] = {0};
static const mrb_irep dcvar_irep = {
  2, 5, 0, MRB_IREP_STATIC, dcvar_iseq,
  NULL, dcvar_syms, NULL, dcvar_lv, NULL,
  sizeof(dcvar_iseq), 0, 2, 0, 0
};

#define ROM_PROC(name, irep)                                            \
  mrb_alignas(8)                                                        \
  static const struct RProc name = {                                    \
    NULL, MRB_TT_PROC, MRB_GC_RED, MRB_OBJ_IS_FROZEN,                   \
    MRB_PROC_SCOPE | MRB_PROC_STRICT,                                   \
    { &irep }, NULL, { NULL }                                           \
  }

/* The `val` member a C function goes in comes first, so a proc entry names the
   member it means. The key is filled at init for the reason given at the top:
   the name is not a presymbol here. */
#define ROM_PROC_ENTRY(pr) { { .proc = &(pr) }, 0, MRB_METHOD_PUBLIC_FL }

ROM_PROC(base_greet_proc, base_greet_irep);
ROM_PROC(child_greet_proc, child_greet_irep);
ROM_PROC(lexical_proc, lexical_irep);
ROM_PROC(cvar_proc, cvar_irep);
ROM_PROC(dconst_proc, dconst_irep);
ROM_PROC(dcvar_proc, dcvar_irep);
/* the same body again, installed on the singleton class: `def self.name`
   reads the constants of the class the singleton is attached to */
ROM_PROC(s_lexical_proc, lexical_irep);

static mrb_mt_entry base_entries[] = {
  ROM_PROC_ENTRY(base_greet_proc),      /* greet */
};

static mrb_mt_entry child_s_entries[] = {
  ROM_PROC_ENTRY(s_lexical_proc),       /* lexical */
};

static mrb_mt_entry child_entries[] = {
  ROM_PROC_ENTRY(child_greet_proc),     /* greet */
  ROM_PROC_ENTRY(lexical_proc),         /* lexical */
  ROM_PROC_ENTRY(cvar_proc),            /* cvar_bump */
  ROM_PROC_ENTRY(dconst_proc),          /* defined_const */
  ROM_PROC_ENTRY(dcvar_proc),           /* defined_cvar */
};

static const char *const child_names[] = {
  "greet", "lexical", "cvar_bump", "defined_const", "defined_cvar",
};

void
mrb_init_test_rom_proc(mrb_state *mrb)
{
  struct RClass *base, *child;

  lexical_syms[0] = mrb_intern_lit(mrb, "ROM_PROC_CONST");
  cvar_syms[0] = mrb_intern_lit(mrb, "@@rom_proc_count");
  dconst_syms[0] = lexical_syms[0];
  dconst_syms[1] = mrb_intern_lit(mrb, "__defined_const?");
  dcvar_syms[0] = cvar_syms[0];
  dcvar_syms[1] = mrb_intern_lit(mrb, "__defined_cvar?");

  base_entries[0].key = mrb_intern_lit(mrb, "greet");
  child_s_entries[0].key = mrb_intern_lit(mrb, "lexical");
  for (int i = 0; i < (int)(sizeof(child_entries)/sizeof(child_entries[0])); i++) {
    child_entries[i].key = mrb_intern_cstr(mrb, child_names[i]);
  }

  base = mrb_define_class(mrb, "RomProcBase", mrb->object_class);
  MRB_MT_INIT_ROM(mrb, base, base_entries);

  child = mrb_define_class(mrb, "RomProcChild", base);
  MRB_MT_INIT_ROM(mrb, child, child_entries);
  MRB_MT_INIT_ROM(mrb, mrb_singleton_class_ptr(mrb, mrb_obj_value(child)),
                  child_s_entries);

  /* what the class body would have run beside the definitions */
  mrb_define_const_id(mrb, child, lexical_syms[0],
                      mrb_str_new_lit(mrb, "lexical"));
  mrb_mod_cv_set(mrb, child, cvar_syms[0], mrb_fixnum_value(0));
}
