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

### The symbol registry

The build scans every source for these macros and numbers the symbols it
finds. The order it numbers them in comes from `presym.list` at the root of
the tree, which is append-only: a symbol keeps the number it has for as long
as the tree still names it, and a symbol new to the tree is numbered after
every symbol already registered.

That order is what keeps a compiler cache useful. The numbers are written to
`build/<target>/include/mruby/presym/id.h`, which every source includes, so
a symbol that moved the numbers of the symbols around it would change what
the compiler is handed for the whole tree, and `ccache` or `sccache` would
answer none of it from cache.

Adding a symbol therefore needs `presym.list` extended. Run

```console
rake presym:update
```

with a configuration that builds every gem the symbol reaches, and commit the
result together with the code that introduced it. CI runs `rake presym:check`
on one Linux job, which reports the symbols the registry is missing and fails
rather than writing them.

The registry covers what that reference job can reproduce, plus the names of
the platform-gated constants the tree spells out for itself: every socket
constant and every errno the generated tables can define is registered, on
whatever platform it turns out to exist. Registering a symbol no build scans
costs nothing, since only the symbols a build finds reach its tables.

A symbol a platform reaches by some other route stays unregistered. It is
numbered after every registered symbol, so it costs the order of that tail
alone, and a maintainer on that platform can register it the same way.

Because a symbol's number is its position among the symbols of the build,
removing the last use of a symbol renumbers the ones registered after it, and
that build recompiles in full. Adding a symbol does not.
