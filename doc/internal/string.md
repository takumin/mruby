<!-- summary: How bytes, characters, and the binary flag fit together -->

# String Indexing Model

This document describes how mruby strings answer "where" and "how
long": what `MRB_UTF8_STRING` does and does not change, what the
per-string binary flag means, and how the regexp engine and
mruby-encoding fit into the same model.

**Read this if you are:** modifying indexing code in `src/string.c`,
offset reporting in mruby-regexp, or mruby-encoding; or deciding
whether a new String API should count bytes or characters.

**For user-facing docs**, see [mrbconf.md](../guides/mrbconf.md) for
the build option, and the READMEs of mruby-regexp and mruby-encoding
for the gem-level APIs.

## The model at a glance

Storage is always a byte array. What varies is the *unit* used to
index it. With `s = "éx"` (`é` is the two bytes `C3 A9`):

|                          | byte build (default) | UTF-8 build (`MRB_UTF8_STRING`) |
| ------------------------ | -------------------- | ------------------------------- |
| `s.length`               | 3                    | 2                               |
| `s[0]`                   | `"\xC3"`             | `"é"`                           |
| `s.scan(/./)`            | `["é", "x"]`         | `["é", "x"]`                    |
| `s =~ /x/`               | 2                    | 1                               |
| `s.b.length`, `s.b =~ /x/` | 3, 2               | 3, 2                            |

Two things are worth noticing. The regexp engine matched `é` as one
character in both builds: the build option did not change *matching*,
only the *reported position*. And the reported position always agrees
with the String API of the same build, so `s[md.begin(0)]` lands on
the match either way.

## The ingredients

### Bytes, always

`RSTR_PTR`/`RSTR_LEN` are bytes in every build. Lower-level state
(capture offsets inside the regexp engine, IO, pack) works in bytes;
units are converted at the public API boundary, not below it.

### `MRB_UTF8_STRING`: the public index unit

The build option chooses the unit of the character-oriented String
API: `length`, `[]`, `index`, `each_char`, regexp match positions and
so on count characters with it and bytes without it. That is the whole
of its meaning. It is a cost switch — character indexing pays an O(n)
walk on multibyte strings — not a statement about which strings may
exist.

A byte build corresponds to using CRuby's byte-oriented API surface
(`byteindex`, `byteslice`, `getbyte`, ...) for everything; a UTF-8
build corresponds to CRuby's ordinary character API.

### `MRB_STR_BINARY`: per-string byte semantics

The flag (set by `String#b`, or by `force_encoding` from
mruby-encoding) marks one string as byte-indexed regardless of build:
`length`/`[]` count its bytes, and the regexp engine matches it byte
by byte. It is mruby's equivalent of an ASCII-8BIT string in CRuby.
The flag is defined unconditionally, means the same thing in both
builds, and travels with the bytes: a copy of a binary string is
binary.

### `MRB_STR_SINGLE_BYTE`: a cache, not a fact source

Under `MRB_UTF8_STRING`, this flag records "known to contain only
single-byte characters" so indexing can skip the UTF-8 walk. It is an
optimization: code may set it only when the contents prove it, and
leaving it unset may cost time but must never change an answer. In
byte builds it is constant-true.

## The invariants

1. **One definition of a character.** Every subsystem that decodes
   UTF-8 must agree on where characters begin and end, including on
   invalid input. (Currently violated; see *Known deviations*.)
2. **`MRB_UTF8_STRING` changes only the public index unit.** It never
   changes what a character is, and never changes what the regexp
   engine matches.
3. **`MRB_STR_BINARY` means byte semantics for that string,
   everywhere.** Same meaning in both builds; the flag follows the
   bytes through copies.
4. **Reported offsets follow the build's String API unit**, so
   `str[md.begin(0)]` addresses the match in every build, and offsets
   into a binary string are byte offsets in every build.
5. **`MRB_STR_SINGLE_BYTE` is only a cache.** Wrongly absent: slower.
   Wrongly present: a bug.

## How mruby-regexp fits

Patterns are always interpreted as UTF-8, in every build; a subject is
matched byte by byte when its binary flag is set and as UTF-8
characters otherwise. The engine contains no `MRB_UTF8_STRING`
conditionals; the only build-dependent code is the conversion of byte
offsets to character offsets (and back) at the API boundary, which
invariant 4 requires. One engine, one behavior, both builds.

See mruby-regexp's README for its deliberate differences from CRuby
(no pattern encodings, byte escapes above `0x7F`, and others).

## How mruby-encoding fits

mruby-encoding gives Ruby-level names to the model: `String#encoding`
reports `"UTF-8"` or `"ASCII-8BIT"` from the binary flag,
`force_encoding` sets and clears it, `valid_encoding?` scans the
bytes. Encodings are name strings rather than Encoding objects, and
only these two exist.

Its mrbgem.rake force-defines `MRB_UTF8_STRING`: adding the gem
selects the UTF-8 build. The contract is "an encoding-aware build is a
UTF-8 build" — the gem names the distinction between character strings
and byte strings, and that distinction only fully exists there.

## What mruby deliberately does not do

- **No Encoding objects, no other encodings.** The model is UTF-8 plus
  binary, chosen per string by one flag.
- **No coderange cache.** CRuby remembers per string whether its bytes
  are valid; mruby has only the single-byte cache flag, so anything
  that needs validity must scan.
- **Broken strings are answered, not raised.** CRuby raises
  `ArgumentError` when matching a string with invalid bytes. Without a
  coderange cache that check would cost a scan per match, so mruby
  treats each invalid byte as a single character instead.

## Known deviations (each fix should delete its bullet)

1. **Three definitions of "one character".** `mrb_utf8len()` in
   `src/string.c` accepts UTF-8-encoded surrogates and overlong
   sequences; `mrb_re_utf8_charlen()` in mruby-regexp rejects them
   (RFC 3629), so the same invalid string has two different character
   counts; and mruby-regexp's offset conversion counts lead bytes, a
   third answer. Observable in a UTF-8 build:

   ```ruby
   s = "\xED\xA0\x80"     # a surrogate, UTF-8-encoded
   s.length               #=> 1  (mrb_utf8len)
   s.scan(/./).size       #=> 3  (mrb_re_utf8_charlen)
   s.valid_encoding?      #=> true (CRuby: false)

   t = "\x80x"            # lone continuation byte, then "x"
   t =~ /x/               #=> 0  (lead-byte counting)
   t.index("x")           #=> 1  (mrb_utf8len)
   ```

   The target is one strict shared definition hoisted into core: count
   each errant byte as one character, which is also CRuby's counting
   (`"\xED\xA0\x80".length == 3` there), so converging removes the
   internal drift and a CRuby difference at once.

2. **Derived strings drop the binary flag.** `dup`/`clone` and
   in-place mutation keep it (invariant 3), but substrings do not yet:
   regexp captures, `pre_match`/`post_match`, `byteslice`, `[]` and
   `+` return non-binary strings from binary input, where CRuby keeps
   ASCII-8BIT.
