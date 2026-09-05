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

### How symbols are numbered

The build scans every source for these macros and numbers the symbols it
finds. The numbers are written to
`build/<target>/include/mruby/presym/id.h`, which every source includes, so a
symbol whose number moved would change what the compiler is handed for every
source that names it, and `ccache` or `sccache` would answer none of those
from cache.

To hold the numbers still, a build numbers by the order it last used, which
is the symbol list it wrote to `build/<target>/presym`. The symbols the scan
finds again keep their places, and the symbols new to the scan are numbered
after all of them. Adding a symbol therefore costs the compile of the source
that names it and nothing else. A build directory with no list yet numbers by
the sorted order, and every build from then on is numbered against that.

Removing the last use of a symbol is the case this cannot hold still: the
symbols numbered after it move down, and that build compiles in full once.

Nothing about this is kept in the tree, so there is nothing to update when a
symbol is added. What it does ask is that the build directory outlive the
compiler cache: a build that starts from an empty directory numbers from the
sorted order again, which a cache filled against a different order will miss.
Where a cache is carried between runs that start fresh, as CI does, carry
`build/*/presym` with it.
