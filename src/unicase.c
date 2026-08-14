/*
** unicase.c - what case a Unicode character has
**
** The tables in unicase.h and the lookups over them. What a string does with
** the answers is string.c's business, and what a pattern does with them is
** mruby-regexp's; this file knows only about codepoints.
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include <mruby.h>

#ifdef MRB_UTF8_STRING

#include <mruby/internal.h>
#include "unicase.h"

mrb_static_assert(UNI_CASE_MAX_BYTES <= MRB_UNI_CASE_MAX_BYTES,
                  "a mapping outgrew the buffer its callers hand over");

static const struct case_table {
  const uni_case_run *runs;
  size_t run_count;
  const uni_case_multi *multi;
  size_t multi_count;
  uint32_t min, max;
} case_tables[] = {
  {UNI_LOWER_RUNS, UNI_LOWER_RUN_COUNT, UNI_LOWER_MULTI, UNI_LOWER_MULTI_COUNT,
   UNI_LOWER_MIN, UNI_LOWER_MAX},
  {UNI_UPPER_RUNS, UNI_UPPER_RUN_COUNT, UNI_UPPER_MULTI, UNI_UPPER_MULTI_COUNT,
   UNI_UPPER_MIN, UNI_UPPER_MAX},
  {UNI_TITLE_RUNS, UNI_TITLE_RUN_COUNT, UNI_TITLE_MULTI, UNI_TITLE_MULTI_COUNT,
   UNI_TITLE_MIN, UNI_TITLE_MAX},
  {UNI_SWAP_RUNS, UNI_SWAP_RUN_COUNT, UNI_SWAP_MULTI, UNI_SWAP_MULTI_COUNT,
   UNI_SWAP_MIN, UNI_SWAP_MAX},
  {UNI_FOLD_RUNS, UNI_FOLD_RUN_COUNT, UNI_FOLD_MULTI, UNI_FOLD_MULTI_COUNT,
   UNI_FOLD_MIN, UNI_FOLD_MAX},
};

/* Locate the run holding cp, or NULL. Runs are emitted in ascending source
   order and never overlap, so a binary search on start is enough; a run with
   stride 2 covers only every other codepoint in its span, which the modulo
   check rejects. */
static const uni_case_run*
case_run_for(const struct case_table *t, uint32_t cp)
{
  size_t lo = 0, hi = t->run_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const uni_case_run *r = &t->runs[mid];
    uint32_t last = r->start + (uint32_t)(r->count - 1) * r->stride;
    if (cp < r->start) hi = mid;
    else if (cp > last) lo = mid + 1;
    else return ((cp - r->start) % r->stride) == 0 ? r : NULL;
  }
  return NULL;
}

static const uni_case_multi*
case_multi_for(const struct case_table *t, uint32_t cp)
{
  size_t lo = 0, hi = t->multi_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < t->multi[mid].cp) hi = mid;
    else if (cp > t->multi[mid].cp) lo = mid + 1;
    else return &t->multi[mid];
  }
  return NULL;
}

/* Ask one table about cp, writing what it says into buf. Answers the byte
   count written, 0 for a character the table maps to itself, and -1 for one
   the table says nothing about. The two apart is what lets title case hand a
   character it says nothing about on to upper case while keeping the ones it
   deliberately maps to themselves. */
static mrb_int
case_map_one(enum mrb_case_kind kind, uint32_t cp, char *buf)
{
  const struct case_table *t = &case_tables[kind];
  if (cp < t->min || t->max < cp) return -1;

  const uni_case_multi *m = case_multi_for(t, cp);
  if (m) {
    memcpy(buf, uni_case_pool + m->off, m->len);
    return m->len;
  }
  const uni_case_run *r = case_run_for(t, cp);
  if (r == NULL) return -1;
  if (r->delta == 0) return 0;
  return mrb_utf8_to_buf(buf, (mrb_int)cp + r->delta);
}

mrb_int
mrb_uni_case_map(enum mrb_case_kind kind, uint32_t cp, char *buf)
{
  mrb_int n = case_map_one(kind, cp, buf);
  if (n >= 0) return n;

  switch (kind) {
  case MRB_CASE_KIND_TITLE:
    /* Title case is the difference from upper case, so a character the
       difference says nothing about takes the upper case answer. */
    n = case_map_one(MRB_CASE_KIND_UPPER, cp, buf);
    break;
  case MRB_CASE_KIND_SWAP:
    /* Swapping is the difference from this rule: a character with a lower
       case is an upper case one and swaps down, and one without swaps up. */
    n = case_map_one(MRB_CASE_KIND_LOWER, cp, buf);
    if (n < 0) n = case_map_one(MRB_CASE_KIND_UPPER, cp, buf);
    break;
  default:
    break;
  }
  return n < 0 ? 0 : n;
}

/* ------------------------------------------------------------- folding

   Simple case folding is the run table on its own: a source whose folding
   spells several characters (U+FB00 to "ff") is in the multi table and out of
   the runs, which is exactly the set simple folding leaves alone. So the two
   foldings share one table and differ in whether the multi half is read. */

uint32_t
mrb_uni_case_fold(uint32_t cp)
{
  /* ASCII is out of the table, being what the table is the rest of. */
  if (cp < 128) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;

  const struct case_table *t = &case_tables[MRB_CASE_KIND_FOLD];
  if (cp < t->min || t->max < cp) return cp;
  const uni_case_run *r = case_run_for(t, cp);
  return r ? (uint32_t)((int32_t)cp + r->delta) : cp;
}

int
mrb_uni_case_unfold(uint32_t cp, uint32_t *out, int max)
{
  const struct case_table *t = &case_tables[MRB_CASE_KIND_FOLD];
  int n = 0;
  uint32_t folded = mrb_uni_case_fold(cp);

  /* The folded form is itself a member of the class. */
  if (folded != cp && n < max) out[n++] = folded;

  /* ASCII sources are in no table, so the upper case letter that folds into a
     lower case one is added here. */
  if (folded >= 'a' && folded <= 'z' && folded - 32 != cp && n < max) {
    out[n++] = folded - 32;
  }

  /* A non-ASCII source folding into ASCII (U+017F into 's') is in the table
     and is found by this scan like any other. */
  for (size_t i = 0; i < t->run_count && n < max; i++) {
    const uni_case_run *r = &t->runs[i];
    int32_t src = (int32_t)folded - r->delta;
    if (src < (int32_t)r->start) continue;
    uint32_t off = (uint32_t)src - r->start;
    if (off % r->stride) continue;
    if (off / r->stride >= r->count) continue;
    if ((uint32_t)src == cp) continue;
    out[n++] = (uint32_t)src;
  }
  return n;
}

/* Both range walks read the table run by run, which keeps a wide range cheap:
   a run of stride 1 contributes one span whatever its length, and only the
   interleaved stride 2 runs are reported one codepoint at a time. */
void
mrb_uni_case_fold_range(uint32_t lo, uint32_t hi,
                        void (*add)(void *, uint32_t, uint32_t), void *user)
{
  const struct case_table *t = &case_tables[MRB_CASE_KIND_FOLD];
  for (size_t i = 0; i < t->run_count; i++) {
    const uni_case_run *r = &t->runs[i];
    uint32_t span = (uint32_t)(r->count - 1) * r->stride;

    uint32_t s_lo = r->start > lo ? r->start : lo;
    uint32_t s_hi = r->start + span < hi ? r->start + span : hi;
    if (s_lo > s_hi) continue;
    /* Round s_lo up and s_hi down to codepoints the run actually holds. */
    uint32_t off = s_lo - r->start;
    if (off % r->stride) s_lo += r->stride - (off % r->stride);
    s_hi -= (s_hi - r->start) % r->stride;
    if (s_lo > s_hi) continue;

    if (r->stride == 1) {
      add(user, (uint32_t)((int32_t)s_lo + r->delta), (uint32_t)((int32_t)s_hi + r->delta));
    }
    else {
      for (uint32_t cp = s_lo; cp <= s_hi; cp += r->stride) {
        uint32_t f = (uint32_t)((int32_t)cp + r->delta);
        add(user, f, f);
      }
    }
  }
}

void
mrb_uni_case_unfold_range(uint32_t lo, uint32_t hi,
                          void (*add)(void *, uint32_t, uint32_t), void *user)
{
  const struct case_table *t = &case_tables[MRB_CASE_KIND_FOLD];
  for (size_t i = 0; i < t->run_count; i++) {
    const uni_case_run *r = &t->runs[i];
    uint32_t span = (uint32_t)(r->count - 1) * r->stride;

    int32_t f_start = (int32_t)r->start + r->delta;
    if (f_start < 0) continue;
    int32_t f_end = f_start + (int32_t)span;
    uint32_t f_lo = (uint32_t)f_start > lo ? (uint32_t)f_start : lo;
    uint32_t f_hi = (uint32_t)f_end < hi ? (uint32_t)f_end : hi;
    if (f_lo > f_hi) continue;
    uint32_t off = f_lo - (uint32_t)f_start;
    if (off % r->stride) f_lo += r->stride - (off % r->stride);
    f_hi -= (f_hi - (uint32_t)f_start) % r->stride;
    if (f_lo > f_hi) continue;

    if (r->stride == 1) {
      add(user, (uint32_t)((int32_t)f_lo - r->delta), (uint32_t)((int32_t)f_hi - r->delta));
    }
    else {
      for (uint32_t cp = f_lo; cp <= f_hi; cp += r->stride) {
        uint32_t s = (uint32_t)((int32_t)cp - r->delta);
        add(user, s, s);
      }
    }
  }
}

#endif  /* MRB_UTF8_STRING */
