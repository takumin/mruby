/*
** re_utf8.c - case folding, character types and word characters for regexp engine
**
** See Copyright Notice in mruby.h
*/

#include "re_internal.h"
#include <string.h>

#ifdef RE_UNICODE_CTYPE

#include "re_ctype.h"

/* The table numbers its bits in the order re_internal.h names them, so that
   an entry read off it is a re_ctype value as it stands. */
mrb_static_assert(RE_CTYPE_TABLE_ALPHA == RE_CTYPE_ALPHA &&
                  RE_CTYPE_TABLE_UPPER == RE_CTYPE_UPPER &&
                  RE_CTYPE_TABLE_LOWER == RE_CTYPE_LOWER &&
                  RE_CTYPE_TABLE_DIGIT == RE_CTYPE_DIGIT &&
                  RE_CTYPE_TABLE_ALNUM == RE_CTYPE_ALNUM &&
                  RE_CTYPE_TABLE_WORD  == RE_CTYPE_WORD &&
                  RE_CTYPE_TABLE_PUNCT == RE_CTYPE_PUNCT &&
                  RE_CTYPE_TABLE_SPACE == RE_CTYPE_SPACE &&
                  RE_CTYPE_TABLE_BLANK == RE_CTYPE_BLANK &&
                  RE_CTYPE_TABLE_GRAPH == RE_CTYPE_GRAPH &&
                  RE_CTYPE_TABLE_PRINT == RE_CTYPE_PRINT &&
                  RE_CTYPE_CNTRL >= (1 << RE_CTYPE_MASK_BITS),
                  "re_ctype.h and re_internal.h number the types differently");

/* The types of a codepoint above ASCII: the set of the run it falls in, which
   is the last run starting at or below it, and cntrl from its range. */
uint16_t
mrb_re_ctype(uint32_t cp)
{
  if (cp < RE_CTYPE_MIN) return 0;
  size_t lo = 0, hi = RE_CTYPE_RUN_COUNT;
  while (hi - lo > 1) {
    size_t mid = lo + (hi - lo) / 2;
    if ((re_ctype_runs[mid] >> RE_CTYPE_MASK_BITS) <= cp) lo = mid;
    else hi = mid;
  }
  uint16_t t = (uint16_t)(re_ctype_runs[lo] & ((1u << RE_CTYPE_MASK_BITS) - 1));
  if (cp >= RE_CTYPE_CNTRL_LO && cp <= RE_CTYPE_CNTRL_HI) t |= RE_CTYPE_CNTRL;
  return t;
}

#ifdef RE_UNICODE_PROP

#include "re_prop.h"

/* A token holds the kind in the byte above the value, and the compiler's
   negation bit stands above both. */
mrb_static_assert(RE_PROP_KIND_GC < (RE_PROP_NEG >> 8) &&
                  RE_PROP_KIND_SCRIPT < (RE_PROP_NEG >> 8),
                  "a property kind reaches the bit the negation is in");

/* The entry of the last run starting at or below `cp`, which is the run it
   falls in: the tables start at U+0000 and every codepoint is inside one. */
static uint32_t
prop_run(const uint32_t *runs, size_t count, uint32_t cp, int bits)
{
  size_t lo = 0, hi = count;
  while (hi - lo > 1) {
    size_t mid = lo + (hi - lo) / 2;
    if ((runs[mid] >> bits) <= cp) lo = mid;
    else hi = mid;
  }
  return runs[lo] & ((1u << bits) - 1);
}

mrb_bool
mrb_re_prop_has(uint16_t prop, uint32_t cp)
{
  uint8_t id = (uint8_t)(prop & 0xff);
  if ((prop >> 8) == RE_PROP_KIND_SCRIPT) {
    return prop_run(re_prop_script_runs, RE_PROP_SCRIPT_RUN_COUNT, cp,
                    RE_PROP_SCRIPT_BITS) == id;
  }
  uint32_t cat = prop_run(re_prop_gc_runs, RE_PROP_GC_RUN_COUNT, cp,
                          RE_PROP_GC_BITS);
  return (re_prop_gc_masks[id] >> cat) & 1;
}

mrb_bool
mrb_re_prop_lookup(const char *name, size_t len, uint16_t *prop)
{
  size_t lo = 0, hi = RE_PROP_NAME_COUNT;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const re_prop_name *e = &re_prop_names[mid];
    size_t n = len < e->len ? len : e->len;
    int cmp = memcmp(name, re_prop_name_chars + e->off, n);
    if (cmp == 0) cmp = len < e->len ? -1 : (len > e->len ? 1 : 0);
    if (cmp == 0) {
      *prop = (uint16_t)((uint16_t)e->kind << 8 | e->id);
      return TRUE;
    }
    if (cmp < 0) hi = mid;
    else lo = mid + 1;
  }
  return FALSE;
}

/* Whether a class holds a codepoint through the property escapes in it: yes
   when it holds a property the codepoint has, or a negated one it lacks. A
   byte has no property, so it is in the class through a negated escape and
   not through a positive one, which is how a byte reads a bracket too.

   Under /i the question is put to the codepoint and to every character
   sharing its folding, so `any` and `all` gather their answers: a positive
   escape wants a property any of them has, a negated one a property any of
   them lacks. The ASCII ones are left out, as they are for a bracket below;
   see compile_charclass(). */
static mrb_bool
class_prop_match(const re_charclass *cc, uint32_t cp, mrb_bool byte)
{
  for (uint16_t i = 0; i < cc->num_props; i++) {
    uint16_t prop = cc->props[i] & (uint16_t)~RE_PROP_NEG;
    mrb_bool neg = (cc->props[i] & RE_PROP_NEG) != 0;
    if (byte) {
      if (neg) return TRUE;
      continue;
    }
    mrb_bool any = mrb_re_prop_has(prop, cp), all = any;
    if (cc->table_fold) {
      uint32_t alt[MRB_UNI_MAX_UNFOLD];
      int n = mrb_uni_case_unfold(cp, alt, MRB_UNI_MAX_UNFOLD);
      for (int j = 0; j < n; j++) {
        if (alt[j] < 128) continue;
        mrb_bool t = mrb_re_prop_has(prop, alt[j]);
        any = any || t;
        all = all && t;
      }
    }
    if (neg ? !all : any) return TRUE;
  }
  return FALSE;
}

#endif  /* RE_UNICODE_PROP */

/* Whether a class holds a codepoint above ASCII through the POSIX brackets in
   it, once its ranges have said nothing: yes when the codepoint's type has a
   bit of ctype_yes, or lacks a bit of ctype_no, and failing both whatever the
   property escapes and utf8_any say. A byte, tagged RE_CLASS_BYTE by the
   caller, has no type: it is in the class through a negated bracket and not
   through a positive one.

   Under /i a character is in the class when any character sharing its
   folding is, so the question is put to every one of them: a positive
   bracket wants a type any of them has, a negated one a type any of them
   lacks. The ASCII ones are left out, since what the class holds through an
   ASCII counterpart is in its ranges already; see compile_charclass(). */
mrb_bool
mrb_re_class_table_match(const re_charclass *cc, uint32_t cp)
{
  mrb_bool byte = (cp & RE_CLASS_BYTE) != 0;
#ifdef RE_UNICODE_PROP
  if (class_prop_match(cc, cp & ~RE_CLASS_BYTE, byte)) return TRUE;
#endif
  if (byte) return cc->ctype_no != 0 || cc->utf8_any;
  uint16_t any = mrb_re_ctype(cp), all = any;
  if (cc->table_fold) {
    uint32_t alt[MRB_UNI_MAX_UNFOLD];
    int n = mrb_uni_case_unfold(cp, alt, MRB_UNI_MAX_UNFOLD);
    for (int i = 0; i < n; i++) {
      if (alt[i] < 128) continue;
      uint16_t t = mrb_re_ctype(alt[i]);
      any |= t;
      all &= t;
    }
  }
  return (any & cc->ctype_yes) || (~all & cc->ctype_no) || cc->utf8_any;
}

#endif  /* RE_UNICODE_CTYPE */

/* Check if character is a "word" character (\w): [a-zA-Z0-9_] */
mrb_bool
mrb_re_is_word_char(uint32_t c)
{
  if (c >= 'a' && c <= 'z') return TRUE;
  if (c >= 'A' && c <= 'Z') return TRUE;
  if (c >= '0' && c <= '9') return TRUE;
  if (c == '_') return TRUE;
  return FALSE;
}

#ifndef RE_UNICODE_CASE

#include "re_cased.h"

mrb_bool
mrb_re_needs_case_data(uint32_t lo, uint32_t hi)
{
  if (hi < RE_CASED_MIN || lo > RE_CASED_MAX) return FALSE;
  for (size_t i = 0; i < RE_CASED_RANGE_COUNT; i++) {
    if (lo <= re_cased_ranges[i][1] && re_cased_ranges[i][0] <= hi) return TRUE;
  }
  return FALSE;
}

#endif  /* !RE_UNICODE_CASE */

uint32_t
mrb_re_case_fold(uint32_t cp)
{
#ifdef RE_UNICODE_CASE
  return mrb_uni_case_fold(cp);
#else
  /* Without the option there is no table to walk, and the compiler reaches
     the same two foldings directly, since there are only two. */
  if (cp < 128) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
  if (cp == RE_FOLD_LONG_S) return 's';
  if (cp == RE_FOLD_KELVIN) return 'k';
  return cp;
#endif
}
