/*
** utf8.c - what a run of bytes spells, and what a string holds character by
**          character
**
** A build carrying this gem indexes its strings by character, and this is what
** does the indexing: the UTF-8 primitives, the walk that counts a string's
** characters and reads whether its bytes spell any, and the conversion between
** a character index and a byte offset. A build without the gem has one
** character per byte and carries none of this; the identity answers it gives
** instead are in core's string.c.
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include <mruby.h>
#include <mruby/string.h>
#include <mruby/internal.h>

#define utf8_islead(c) ((unsigned char)((c)&0xc0) != 0x80)

/* the byte length a lead byte claims, read only through mrb_utf8len() */
static const char mrb_utf8len_table[] = {
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 3, 3, 4, 0
};

mrb_int
mrb_utf8len(const char* p, const char* e)
{
  mrb_int len = mrb_utf8len_table[(unsigned char)p[0] >> 3];
  if (len > e - p) return 1;
  switch (len) {
  case 0:
    return 1;
  case 4:
    if (utf8_islead(p[3])) return 1;
  case 3:
    if (utf8_islead(p[2])) return 1;
  case 2:
    if (utf8_islead(p[1])) return 1;
  }
  /* Reject overlong sequences, UTF-16 surrogates, and code points above
     U+10FFFF (RFC 3629, Unicode D93b). */
  switch ((unsigned char)p[0]) {
  case 0xC0: case 0xC1:                       /* overlong (< U+0080) */
    return 1;
  case 0xE0:                                  /* overlong (< U+0800) */
    if ((unsigned char)p[1] < 0xA0) return 1;
    break;
  case 0xED:                                  /* surrogate (U+D800..U+DFFF) */
    if ((unsigned char)p[1] > 0x9F) return 1;
    break;
  case 0xF0:                                  /* overlong (< U+10000) */
    if ((unsigned char)p[1] < 0x90) return 1;
    break;
  case 0xF4:                                  /* above U+10FFFF */
    if ((unsigned char)p[1] > 0x8F) return 1;
    break;
  case 0xF5: case 0xF6: case 0xF7:            /* above U+10FFFF */
    return 1;
  }
  return len;
}

/* The byte the character covering `p` starts at, or `p` itself when `p` is
   already a character boundary. A continuation byte belongs to the character
   that reaches it; one that no lead byte reaches belongs to none and stands as
   a character of its own. Whether a lead byte reaches is mrb_utf8len()'s
   answer, so the boundaries found here are the ones the character count is
   taken over. Reading back three bytes covers it, since nothing longer than
   four bytes spells a character. */
const char*
mrb_utf8_char_head(const char *beg, const char *p, const char *end)
{
  if (p >= end || utf8_islead(p[0])) return p;
  for (mrb_int back = 1; back <= 3 && back <= p - beg; back++) {
    const char *lead = p - back;
    if (!utf8_islead(lead[0])) continue;  /* another continuation byte */
    return mrb_utf8len(lead, end) > back ? lead : p;
  }
  return p;
}

/* Decode a UTF-8 character and return its codepoint.
   *lenp is set to the byte length consumed. mrb_utf8len() answers 1 for every
   sequence it rejects, so those consume a single byte and come back as the
   lead byte itself. */
uint32_t
mrb_utf8_decode(const char *p, const char *e, mrb_int *lenp)
{
  uint8_t c = (uint8_t)p[0];
  uint32_t cp;
  mrb_int n = mrb_utf8len(p, e);

  *lenp = n;
  switch (n) {
  case 2:
    cp = (c & 0x1f) << 6;
    cp |= ((uint8_t)p[1] & 0x3f);
    return cp;
  case 3:
    cp = (c & 0x0f) << 12;
    cp |= ((uint8_t)p[1] & 0x3f) << 6;
    cp |= ((uint8_t)p[2] & 0x3f);
    return cp;
  case 4:
    cp = (c & 0x07) << 18;
    cp |= ((uint8_t)p[1] & 0x3f) << 12;
    cp |= ((uint8_t)p[2] & 0x3f) << 6;
    cp |= ((uint8_t)p[3] & 0x3f);
    return cp;
  default:
    return c;  /* ASCII, or invalid/truncated byte returned as-is */
  }
}

#ifdef SIMPLE_SEARCH_NONASCII
/* the naive implementation. define SIMPLE_SEARCH_NONASCII, */
/* if you need it for any constraint (e.g. code size).      */
static const char*
search_nonascii(const char* p, const char *e)
{
  for (; p < e; ++p) {
    if (NOASCII(*p)) return p;
  }
  return e;
}

#elif defined(__SSE2__)
# include <emmintrin.h>

static inline const char *
search_nonascii(const char *p, const char *e)
{
  if (sizeof(__m128i) < (size_t)(e - p)) {
    if (!_mm_movemask_epi8(_mm_loadu_si128((__m128i const*)p))) {
      const intptr_t lowbits = sizeof(__m128i) - 1;
      const __m128i *s, *t;
      s = (const __m128i*)(~lowbits & ((intptr_t)p + lowbits));
      t = (const __m128i*)(~lowbits & (intptr_t)e);
      for (; s < t; ++s) {
        if (_mm_movemask_epi8(_mm_load_si128(s))) break;
      }
      p = (const char *)s;
    }
  }
  /* One test per byte the range holds: entering at `case N` runs N of them,
     and `default` is only reached where the range holds at least 16, which is
     the first `_mm_loadu_si128()` having found a byte among those 16. A test
     more than the label promises reads the position the range ends at, which
     every mruby string happens to carry as its NUL sentinel and a bare buffer
     does not. */
  switch (e - p) {
  default:
  case 16: if (NOASCII(*p)) return p; ++p;
  case 15: if (NOASCII(*p)) return p; ++p;
  case 14: if (NOASCII(*p)) return p; ++p;
  case 13: if (NOASCII(*p)) return p; ++p;
  case 12: if (NOASCII(*p)) return p; ++p;
  case 11: if (NOASCII(*p)) return p; ++p;
  case 10: if (NOASCII(*p)) return p; ++p;
  case 9:  if (NOASCII(*p)) return p; ++p;
  case 8:  if (NOASCII(*p)) return p; ++p;
  case 7:  if (NOASCII(*p)) return p; ++p;
  case 6:  if (NOASCII(*p)) return p; ++p;
  case 5:  if (NOASCII(*p)) return p; ++p;
  case 4:  if (NOASCII(*p)) return p; ++p;
  case 3:  if (NOASCII(*p)) return p; ++p;
  case 2:  if (NOASCII(*p)) return p; ++p;
  case 1:  if (NOASCII(*p)) return p; ++p;
  case 0:  break;
  }
  return e;
}

#else

static const char*
search_nonascii(const char *p, const char *e)
{
  ptrdiff_t byte_len = e - p;

  const char *be = p + sizeof(bitint) * (byte_len / sizeof(bitint));
  for (; p < be; p+=sizeof(bitint)) {
    bitint t0;

    memcpy(&t0, p, sizeof(bitint));
    const bitint t1 = t0 & (MASK01*0x80);
    if (t1) {
      e = p + sizeof(bitint)-1;
      byte_len = sizeof(bitint)-1;
      break;
    }
  }

  switch (byte_len % sizeof(bitint)) {
#ifdef MRB_64BIT
  case 7: if (e[-7]&0x80) return e-7;
  case 6: if (e[-6]&0x80) return e-6;
  case 5: if (e[-5]&0x80) return e-5;
  case 4: if (e[-4]&0x80) return e-4;
#endif
  case 3: if (e[-3]&0x80) return e-3;
  case 2: if (e[-2]&0x80) return e-2;
  case 1: if (e[-1]&0x80) return e-1;
  }
  return e;
}

#endif  /* SIMPLE_SEARCH_NONASCII */

/* search_nonascii() for core, which asks it in two places of its own: what a
   splice left behind, and what a concatenation joined. Both sit in string.c
   behind the same define this file is compiled under. The scan itself stays
   static so that the walks above keep it inlined, since they ask it per
   string rather than twice per build. */
const char*
mrb_str_search_nonascii(const char *p, const char *e)
{
  return search_nonascii(p, e);
}

#if defined(__GNUC__) || __has_builtin(__builtin_popcount)
# ifdef MRB_64BIT
# define popcount(x) __builtin_popcountll(x)
# else
# define popcount(x) __builtin_popcountl(x)
# endif
#else
#define POPC_SHIFT (8 * sizeof(bitint) - 8)
static inline uint32_t popcount(bitint x)
{
  x = (x & (MASK01*0x55)) + ((x >>  1) & (MASK01*0x55));
  x = (x & (MASK01*0x33)) + ((x >>  2) & (MASK01*0x33));
  x = (x & (MASK01*0x0F)) + ((x >>  4) & (MASK01*0x0F));
  return (uint32_t)((x * MASK01) >> POPC_SHIFT);
}
#endif

/* Counts characters, and when `validp` is given also reports whether every
   sequence decoded as one character. The walk stops at the first broken
   sequence, so the returned count is a character count only while `*validp`
   stays TRUE. */
static mrb_int
utf8_strlen_check(const char *str, mrb_int byte_len, mrb_bool *validp)
{
  const char *p = str;
  const char *e = str + byte_len;
  mrb_int len = 0;

  while (p < e) {
    const char *np = search_nonascii(p, e);

    len += np - p;
    if (np == e) break;
    p = np;
    while (p < e && NOASCII(*p)) {
      mrb_int clen = mrb_utf8len(p, e);

      /* mrb_utf8len() answers 1 for a byte that leads no valid sequence. The
         byte here is known to be non-ASCII, so a length of 1 means the string
         carries a byte that stands for no character. */
      if (validp && clen == 1) {
        *validp = FALSE;
        return len;
      }
      p += clen;
      len++;
    }
  }
  return len;
}

mrb_int
mrb_utf8_strlen(const char *str, mrb_int byte_len)
{
  return utf8_strlen_check(str, byte_len, NULL);
}

/* count the characters of a string */
mrb_int
mrb_str_char_len(mrb_state *mrb, mrb_value str)
{
  (void)mrb;
  struct RString *s = mrb_str_ptr(str);
  mrb_int byte_len = RSTR_LEN(s);

  /* A single-byte string has one position per byte, which is what
     mrb_str_char_to_byte() and mrb_str_byte_to_char() already answer for it.
     Asked here only where the string stands, the same string was measured as
     UTF-8 and reported a length its own indexing did not agree with.

     Nothing is recorded on the way out. A string of nothing but ASCII carries
     that already, and a byte-read one returns here because of how it is read
     rather than because of what its bytes are: 7BIT would be a claim about
     bytes nothing has looked at, and force_encoding() can take the byte
     reading away again and leave the claim standing. */
  if (RSTR_SINGLE_BYTE_P(s)) {
    return byte_len;
  }
  else {
    const char *p = RSTR_PTR(s);
    const char *e = p + byte_len;
    const char *np = search_nonascii(p, e);

    /* Every character a non-ASCII byte begins spells two bytes or more, and a
       non-ASCII byte that begins none spells no character at all, so a string
       holds one character per byte exactly when every byte of it is ASCII.
       Counts that come out equal do not say that: a byte spelling no character
       is counted as one too, so a string of them set the flag as well, and the
       readers of it went on to hand those bytes back as characters. */
    if (np == e) {
      RSTR_CODERANGE_SET(s, MRB_STR_CODERANGE_7BIT);
      return byte_len;
    }
    mrb_int utf8_len = (mrb_int)(np - p) + mrb_utf8_strlen(np, (mrb_int)(e - np));
    mrb_assert(utf8_len <= byte_len);
    return utf8_len;
  }
}

/* whether a string's bytes read as the encoding it is taken to have */
mrb_bool
mrb_str_valid_encoding_p(mrb_state *mrb, mrb_value str)
{
  (void)mrb;
  struct RString *s = mrb_str_ptr(str);
  /* A byte-indexed string makes no such claim, so it is valid whatever its
     bytes are. */
  if (RSTR_BINARY_P(s)) return TRUE;
  /* The walk below reads the whole string to answer either way, so a string
     that has been walked already is answered off where it stands instead. A
     string of one character per byte is one of those: it holds nothing but
     ASCII, and ASCII reads as UTF-8 as it stands. This is what a string
     counted before it is asked about comes in carrying. */
  mrb_int cr = RSTR_CODERANGE(s);
  if (cr == MRB_STR_CODERANGE_7BIT || cr == MRB_STR_CODERANGE_VALID) return TRUE;
  if (cr == MRB_STR_CODERANGE_BROKEN) return FALSE;

  mrb_int byte_len = RSTR_LEN(s);
  mrb_bool valid = TRUE;
  mrb_int utf8_len = utf8_strlen_check(RSTR_PTR(s), byte_len, &valid);

  if (!valid) {
    RSTR_CODERANGE_SET(s, MRB_STR_CODERANGE_BROKEN);
    return FALSE;
  }
  RSTR_CODERANGE_SET(s, byte_len == utf8_len ? MRB_STR_CODERANGE_7BIT
                                             : MRB_STR_CODERANGE_VALID);
  return TRUE;
}

/* whether every byte of the string is ASCII. A walk that finds nothing else
   has made the statement 7BIT makes, so the answer is left on the string for
   the next asker to read off. */
mrb_bool
mrb_str_ascii_p(struct RString *s)
{
  if (RSTR_CODERANGE(s) == MRB_STR_CODERANGE_7BIT) return TRUE;

  const char *p = RSTR_PTR(s);
  const char *e = p + RSTR_LEN(s);
  if (search_nonascii(p, e) != e) return FALSE;
  RSTR_CODERANGE_SET(s, MRB_STR_CODERANGE_7BIT);
  return TRUE;
}

/* Whether a character index into this string is already a byte index, asking
   the bytes where the string does not say. RSTR_SINGLE_BYTE_P() reads what is
   recorded and answers no for a string nothing has read yet, which sends every
   later caller down the walking path however plain the bytes are. A string is
   walked whole at most once here: the walk records what it finds, and it is
   the same walk the character indexing would go on to do anyway. */
mrb_bool
mrb_str_single_byte_p(mrb_state *mrb, mrb_value str)
{
  struct RString *s = mrb_str_ptr(str);
  if (RSTR_CODERANGE(s) == MRB_STR_CODERANGE_UNKNOWN) {
    mrb_str_valid_encoding_p(mrb, str);
  }
  return RSTR_SINGLE_BYTE_P(s);
}

/* map character index to byte offset index */
mrb_int
mrb_str_char_to_byte(mrb_state *mrb, mrb_value str, mrb_int off, mrb_int idx)
{
  (void)mrb;
  struct RString *s = mrb_str_ptr(str);
  if (RSTR_SINGLE_BYTE_P(s)) {
    return idx;
  }

  const char *o = RSTR_PTR(s);
  const char *p0 = o + off;
  const char *p = p0;
  const char *e = o + RSTR_LEN(s);
  mrb_int i = 0;

  while (p<e && i<idx) {
    if ((*p & 0x80) == 0) {
      /* Every ASCII byte stands for a character of its own, so the run only
         has to be followed as far as the index asks for. Reading to the end of
         the string instead makes finding the character just past the head cost
         what finding the last one does. */
      const char *lim = (e - p) > (idx - i) ? p + (idx - i) : e;
      const char *np = search_nonascii(p, lim);
      i += np - p;
      p = np;
    }
    else {
      p += mrb_utf8len(p, e);
      i++;
    }
  }

  mrb_int len = (mrb_int)(p-p0);
  if (i<idx) len++;
  return len;
}

/* map byte offset to character index */
mrb_int
mrb_str_byte_to_char(mrb_state *mrb, mrb_value str, mrb_int bi)
{
  (void)mrb;
  struct RString *s = mrb_str_ptr(str);
  if (bi < 0 || RSTR_LEN(s) < bi) return -1;
  if (RSTR_SINGLE_BYTE_P(s)) {
    return bi;
  }

  const char *p = RSTR_PTR(s);
  const char *e = p + RSTR_LEN(s);
  const char *pivot = p + bi;
  mrb_int i = 0;

  while (p < pivot) {
    if ((*p & 0x80) == 0) {
      const char *np = search_nonascii(p, pivot);
      i += np - p;
      p = np;
    }
    else {
      p += mrb_utf8len(p, e);
      i++;
    }
  }
  if (p != pivot) return -1;
  return i;
}

mrb_int
mrb_str_index_str_by_char(mrb_state *mrb, mrb_value str, mrb_value sub, mrb_int pos)
{
  /* see str_index_str() */
  if (!mrb_str_valid_encoding_p(mrb, sub)) return -1;

  const char *ptr = RSTRING_PTR(sub);
  mrb_int len = RSTRING_LEN(sub);

  if (pos > 0) {
    pos = mrb_str_char_to_byte(mrb, str, 0, pos);
  }

  pos = mrb_str_index(mrb, str, ptr, len, pos);

  if (pos > 0) {
    pos = mrb_str_byte_to_char(mrb, str, pos);
  }
  return pos;
}
