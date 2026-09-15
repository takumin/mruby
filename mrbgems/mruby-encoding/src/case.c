/*
** case.c - what a string becomes when its case is converted by Unicode
**
** The walk over a string's characters, and what it builds beside it. The
** tables it reads are in unicase.c; the ASCII conversion each String method
** keeps for itself is in core's string.c, which is where a string this walk
** turns down goes back to.
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include <mruby.h>
#include <mruby/string.h>
#include <mruby/numeric.h>
#include <mruby/internal.h>

/* A build classifying its characters by ASCII carries no tables to walk them
   with, so it carries none of this either and every conversion is the ASCII
   one core keeps. */
#ifndef MRB_USE_ASCII_CTYPE

/* What the walk below makes of an ASCII character. Each method keeps its own
   loop over a string that holds nothing but ASCII, so this is reached only for
   the ASCII characters of a string that holds others beside them. */
static int
ascii_case_conv(int c, enum mrb_case_mode mode, mrb_bool first)
{
  switch (mode) {
  case MRB_CASE_UP:
    return TOUPPER(c);
  case MRB_CASE_CAPITALIZE:
    return first ? TOUPPER(c) : TOLOWER(c);
  case MRB_CASE_SWAP:
    return ISUPPER(c) ? TOLOWER(c) : TOUPPER(c);
  default:
    return TOLOWER(c);
  }
}

static enum mrb_case_kind
case_kind_of(enum mrb_case_mode mode, mrb_bool first)
{
  switch (mode) {
  case MRB_CASE_UP:
    return MRB_CASE_KIND_UPPER;
  case MRB_CASE_CAPITALIZE:
    return first ? MRB_CASE_KIND_TITLE : MRB_CASE_KIND_LOWER;
  case MRB_CASE_SWAP:
    return MRB_CASE_KIND_SWAP;
  case MRB_CASE_FOLD:
    return MRB_CASE_KIND_FOLD;
  default:
    return MRB_CASE_KIND_LOWER;
  }
}

/* Room in `o` for `need` more bytes past the `len` already written. The answer
   is built with its length held apart from the string, so this grows the
   buffer the way an append does without the questions an append from anywhere
   has to ask: what is written here is this walk's own bytes, and where they
   go is not somewhere the string can already be. */
static char*
case_out_room(mrb_state *mrb, struct RString *o, mrb_int len, mrb_int need)
{
  mrb_int capa = RSTR_CAPA(o);

  if (capa - len < need) {
    mrb_int want;
    if (mrb_int_add_overflow(len, need, &want)) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "string size too big");
    }
    while (capa < want) {
      if (mrb_int_mul_overflow(capa, 2, &capa)) {
        capa = want;
        break;
      }
    }
    /* Leaving the buffer takes the string's length with it, and what an
       embedded string carries over is that many bytes: told nothing, it would
       carry over none of what has been written so far. */
    RSTR_SET_LEN(o, len);
    mrb_str_resize_capa(mrb, o, capa);
  }
  return RSTR_PTR(o) + len;
}

/* Convert a string that holds characters the tables can speak about. A mapping
   changes how many bytes a character takes ("K" U+212A lower cases to the one
   byte of "k"), so the answer is built beside the string rather than over it,
   and the string takes the buffer's bytes at the end. */
static mrb_bool
str_case_convert_utf8(mrb_state *mrb, mrb_value str, enum mrb_case_mode mode)
{
  struct RString *s = mrb_str_ptr(str);
  const char *p = RSTR_PTR(s);
  const char *pend = p + RSTR_LEN(s);
  mrb_value out = mrb_str_new_capa(mrb, RSTR_LEN(s));
  struct RString *o = mrb_str_ptr(out);
  mrb_int dlen = 0;
  mrb_bool modify = FALSE;
  mrb_bool ascii_only = TRUE;
  mrb_bool first = TRUE;

  while (p < pend) {
    /* Room for whatever one character can map to, so neither branch below has
       to ask again for the character it is about to write. */
    char *d = case_out_room(mrb, o, dlen, MRB_UNI_CASE_MAX_BYTES);

    if ((unsigned char)*p < 0x80) {
      /* ASCII has no mapping to look up and takes one byte of the answer per
         byte of the source, so a run of it is converted where it stands.
         Reaching the tables for it, or the buffer through an append, is what
         made a string of ASCII with one character among it cost as much per
         byte as one made of characters. The run stops where the buffer does,
         and the turn of the loop after it is what grows the buffer. */
      const char *dend = RSTR_PTR(o) + RSTR_CAPA(o);
      do {
        int c = (unsigned char)*p++;
        int r = ascii_case_conv(c, mode, first);
        first = FALSE;
        if (r != c) modify = TRUE;
        *d++ = (char)r;
      } while (p < pend && (unsigned char)*p < 0x80 && d < dend);
      dlen = (mrb_int)(d - RSTR_PTR(o));
      continue;
    }

    const char *src = p;
    mrb_int clen;
    uint32_t cp = mrb_utf8_decode(p, pend, &clen);
    mrb_int n;

    /* A run of bytes that spells no character has no case to convert, and
       answering as though it were the byte it starts with would hand back a
       string neither its own reading nor the caller asked for. */
    if (clen == 1) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "input string invalid");
    }
    n = mrb_uni_case_map(case_kind_of(mode, first), cp, d);
    /* A character with no mapping stands as it is. */
    if (n == 0) {
      memcpy(d, src, (size_t)clen);
      n = clen;
    }
    p += clen;
    first = FALSE;

    if (n != clen || memcmp(d, src, (size_t)n) != 0) modify = TRUE;
    /* Only what a mapping wrote can be asked about here: a character maps to
       characters, and ASCII maps to ASCII, so the run above answers itself. */
    for (mrb_int i = 0; i < n; i++) {
      if ((unsigned char)d[i] & 0x80) ascii_only = FALSE;
    }
    dlen += n;
  }

  if (!modify) return FALSE;

  RSTR_SET_LEN(o, dlen);
  RSTR_PTR(o)[dlen] = '\0';

  /* Every byte of the source spelled a character, since the walk refuses one
     that does not, and every mapping spells characters, so what was written
     is sound. Nothing but ASCII is the stronger answer where it holds. */
  RSTR_CODERANGE_SET(o, ascii_only ? MRB_STR_CODERANGE_7BIT
                                   : MRB_STR_CODERANGE_VALID);
  mrb_str_replace_ptr(mrb, s, o);
  return TRUE;
}

int
mrb_str_case_convert_unicode(mrb_state *mrb, mrb_value str, enum mrb_case_mode mode)
{
  struct RString *s = mrb_str_ptr(str);

  /* A string of nothing but ASCII holds no character the tables speak about,
     and one read as bytes holds no characters at all. Neither is this walk's
     to make, so both go back to the caller's own loop, which converts the
     bytes where they stand. A string that has not been walked yet is walked
     for it: reading it through is what the loop below does anyway, and this
     way an ASCII one is spared the second string the walk builds beside it.
     The byte reading is asked about first, since a string read as bytes must
     not be recorded as holding one character per byte. */
  if (RSTR_BINARY_P(s) || mrb_str_ascii_p(s)) return -1;

  /* The walk only reads the string and builds its answer beside it, so a
     buffer the string shares is read where it is rather than copied first:
     the copy would go unwritten, and mrb_str_replace_ptr() lets go of a
     shared buffer by dropping the reference where it would free a private
     one. A string
     the walk leaves as it was keeps what it carried, coderange included, and
     one it changes takes the answer's along with the bytes, so nothing here
     is prepared for a write. What is still owed is the frozen check: a
     frozen receiver is turned away before anything is read, whether or not
     the walk would have changed it, which is what CRuby's bang forms do. */
  mrb_check_frozen(mrb, s);

  return str_case_convert_utf8(mrb, str, mode) ? 1 : 0;
}

#endif  /* !MRB_USE_ASCII_CTYPE */
