<!-- summary: About the Symbols -->

# Symbols

Symbols in `mruby` C source code is represented by `mrb_sym` which is alias of
`uint32_t`. Lower 30 bits are used for symbols so that higher 2 bits can be
used as flags, e.g. `struct mt_elem` in `class.c`.

```c
struct mt_elem {
  union mt_ptr ptr;
  size_t func_p:1;
  size_t noarg_p:1;
  mrb_sym key:sizeof(mrb_sym)*8-2;
};
```

## C API

We provide following C API for symbols.

### Generate Symbols

#### `mrb_sym mrb_intern(mrb_state*,const char*,size_t)`

Get a symbol from a string.

#### `mrb_sym mrb_intern_check_cstr(mrb_state*,const char*)`

Get a symbol from a NULL terminated (C) string.

#### `mrb_sym mrb_intern_str(mrb_state*,mrb_value)`

Get a symbol from a Ruby string object.

#### `mrb_intern_lit(mrb_state*,const char*)`

Get a symbol from a C string literal. The second argument should be a C string
literal, otherwise you will get a compilation error. It does not copy C string
given the fact it's a literal.

#### `mrb_sym mrb_intern_check(mrb_state*,const char*,size_t)`

Get a symbol from a string if the string has been already registered as a
symbol, otherwise return `0`. We also provide variants `mrb_intern_check_str()`
(from Ruby string) and `mrb_intern_check_cstr()` (from C string).

#### `const char *mrb_sym_name(mrb_state*,mrb_sym)`

Get a string representation of a symbol as a C string.

#### `const char *mrb_sym_name_len(mrb_state*,mrb_sym,mrb_int*)`

Get a string representation of a symbol, and its length.

## Preallocate Symbols

To save RAM, `mruby` can use compile-time allocation of some symbols. You can
use following macros to get preallocated symbols by including `mruby/presym.h`
header.

- `MRB_SYM(xor)` //=> xor (Word characters)
- `MRB_SYM_B(xor)` //=> xor! (Method with Bang)
- `MRB_SYM_Q(xor)` //=> xor? (Method with Question mark)
- `MRB_SYM_E(xor)` //=> xor= (Method with Equal)
- `MRB_GVSYM(xor)` //=> $xor (Global Variable)
- `MRB_CVSYM(xor)` //=> @@xor (Class Variable)
- `MRB_IVSYM(xor)` //=> @xor (Instance Variable)
- `MRB_OPSYM(xor)` //=> ^ (Operator)

For `MRB_OPSYM()`, specify the names corresponding to operators (see
`MRuby::Presym::OPERATORS` in `lib/mruby/presym.rb` for the names that
can be specified for it). Other than that, describe only word characters
excluding leading and ending punctuation.

These macros are converted to static symbol IDs at compile time.
The `_2` suffix variants (e.g., `MRB_SYM_2`) are kept for backward
compatibility only; they accept an explicit `mrb_state*` parameter
but ignore it. New code should use the standard macros above.

### How the IDs are picked

The build scans the sources for these macros and writes the IDs it found into
`build/<name>/include/mruby/presym/id.h`, which `mruby.h` includes. An ID is a
hash of the symbol's name folded into `MRB_PRESYM_BITS` bits, so it depends on
the name and on nothing else the scan turned up: adding a symbol adds one
`#define` and leaves every other symbol where it was. A translation unit that
names none of the new symbols preprocesses to the bytes it did before, which
is what lets `ccache` and `sccache` hand back the object they already have.

Two names can fold to the same ID. The generator gives the second one the next
free ID, and `presym_find()` in `src/symbol.c` walks the run of consecutive IDs
that a displaced name could have reached, so a clash costs a comparison rather
than a build error.

Preallocated IDs run over `[1, MRB_PRESYM_MAX)` and runtime symbols start at
`MRB_PRESYM_MAX`, so `sym < MRB_PRESYM_MAX` tells the two apart. Only a
fraction of that range is ever occupied; the rest is what leaves each name a
place of its own.

### Reading a name back

Scattered IDs cannot index a table the way dense ones could, but the set of
them is settled once the scan is done, so where each one belongs can be settled
then too. The generator places every ID in a slot of its own, by the CHD
construction: the IDs are bucketed by their high bits, the crowded buckets are
taken first, and each bucket is given the smallest displacement that drops its
IDs on slots still free. `presym_slot()` recomputes that placement with a
shift, a load, an xor, a mask and a conditional subtract, and the slot it
answers holds the ID or holds none. Nothing is searched.

The slot count is not rounded up to a power of two. It would cost six bytes
for every slot left empty, which a symbol count just past a power of two would
pay two thousand times over, so the generator measures the shapes instead and
keeps the one whose tables come out smallest: packing tight leaves fewer empty
slots but needs larger displacements to fill, and a displacement past 255
doubles the width of the table holding them. A count that is not a power of
two is what the conditional subtract is for, the masked value landing at most
one count past the end.

Nothing outside `src/symbol.c` should read an ID as an index: they are
scattered, and the slots outnumber them. `mrb_presym_count()` reports how many
symbols there are, and code that has to walk them steps `slot` from 0 to
`mrb_presym_slots()`, reading `mrb_presym_at(slot)` and skipping the empties.
