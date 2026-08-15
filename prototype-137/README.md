# Measurement harness for fork issue #137

**Not for upstream.** This directory exists so the measurement behind the
previous commit survives an ephemeral container. Drop it before cutting a
branch for submission — nothing fork-local goes upstream.

## What was measured

Once the index opcodes stop bypassing the method table, mruby-regexp's
`String#[]` override stops being free. Four designs were built and run, each
on top of `index-opcodes-honor-core-redefinition`, on the default host build,
1M iterations x 5, empty-loop control subtracted:

| design                                           | `s[i]`   | vs before | `s.slice(i)` | `s[re]` | diff     | core change |
| ------------------------------------------------ | -------- | --------- | ------------ | ------- | -------- | ----------- |
| before (`03e66d9`, bypass active)                | 31.7 ns  | 1.00x     | 180.1 ns     | 1147 ns | —        | —           |
| the override as it stands (Ruby, `*args`)        | 181.5 ns | 5.72x     | 179.5 ns     | 1168 ns | —        | —           |
| **A** Ruby override without the splat            | 133.0 ns | 4.19x     | 133.8 ns     | 1069 ns | 6 lines  | none        |
| **B** the override as a gem cfunc                | 60.0 ns  | 1.89x     | 59.5 ns      | 1370 ns | 31 lines | none        |
| **C** core dispatch hook (the commit below this) | 32.4 ns  | 1.04x     | 57.5 ns      | 1349 ns | 42 lines | 19 lines    |
| control: builtin aliased back over the override  | 32.8 ns  | 1.03x     | —            | —       | —        | —           |

Every build: `rake test` 2089 total, 2042 OK, 0 KO, 0 Crash, 0 Warning,
47 Skip.

A fifth design — having the opcode call a cfunc override directly instead of
falling back to a generic send — was ruled out without building it.
`bench_send_cost.rb` measures the floor of a send at ~20 ns (`s.bytesize`, no
arguments, trivial body) against a 28 ns gap between B and C, and a cfunc
needs a callinfo and `mrb_get_args()` whatever the caller does. It could
recover 8-9 ns of the 28, so it cannot reach parity.

## Running it

```sh
rake
./build/host/bin/mruby prototype-137/bench_fast_path.rb
./build/host/bin/mruby prototype-137/bench_fast_path.rb alias
```

- `bench_fast_path.rb` — `OP_GETIDX` and `OP_GETIDX0`, with Array and Hash
  alongside as controls that nothing in the tree redefines. The `alias`
  argument aliases the builtin back over the override, which re-arms the same
  slot; that is the control row above. Ends in behaviour assertions, so a fast
  binary that answers wrongly is not read as a win.
- `bench_send_cost.rb` — the decomposition. `__aref` is the gem's alias of the
  builtin, so `s.__aref(i)` is an ordinary send to the same C function the
  opcode inlines; the gap between it and `s[i]` is the send machinery.
- `bench_regexp_path.rb` — the Regexp path, so a fast-path win is not being
  paid for there unnoticed.

All three report the best of five runs.

## The two designs that lost

Kept as source rather than as patch files, because a diff's blank context
lines are a single trailing space and the repository's `trailing-whitespace`
hook rewrites them. Both apply to `index-opcodes-honor-core-redefinition`.

### A — the Ruby override without the splat

`mrbgems/mruby-regexp/mrblib/string_regexp.rb` only. The default-value
expression is what replaces the sentinel: it assigns a local when the caller
gave no length, so `str[i, nil]` stays distinguishable from `str[i]`.

```ruby
def [](a1, a2 = (__no_alen = true; nil))
  unless Regexp === a1
    return __no_alen ? __aref(a1) : __aref(a1, a2)
  end
  md = Regexp.__search(a1, self)
  return nil unless md
  md[__no_alen ? 0 : a2]
end
```

Removes both splat arrays and keeps the Ruby frame. 181.5 -> 133.0 ns, so the
two arrays are about a third of the cost and the frame is the rest.

### B — the override as a gem cfunc

`mrbgems/mruby-regexp/src/regexp.c`, with `mrb_str_aref()` reached through
`mruby/internal.h`, which the file already includes:

```c
static mrb_value
str_aref_c(mrb_state *mrb, mrb_value self)
{
  mrb_value a1, a2;
  mrb_int argc = mrb_get_args(mrb, "o|o", &a1, &a2);

  switch (mrb_type(a1)) {
  case MRB_TT_INTEGER:
  case MRB_TT_STRING:
  case MRB_TT_RANGE:
    return mrb_str_aref(mrb, self, a1, argc == 1 ? mrb_undef_value() : a2);
  default:
    break;
  }
  if (mrb_obj_is_kind_of(mrb, a1, mrb_class_get(mrb, "Regexp"))) {
    mrb_value argv[2];
    argv[0] = a1; argv[1] = a2;
    return mrb_funcall_argv(mrb, self, mrb_intern_cstr(mrb, "__aref_re"), argc, argv);
  }
  return mrb_str_aref(mrb, self, a1, argc == 1 ? mrb_undef_value() : a2);
}
```

registered in `mrb_mruby_regexp_gem_init()` under a private name, because
mrblib runs after gem init and would otherwise capture this function as
`__aref`:

```c
mrb_define_method(mrb, mrb->string_class, "__aref_c", str_aref_c, MRB_ARGS_ARG(1,1));
```

and in the mrblib, `def [](*args)` becomes `def __aref_re(*args)` while
`alias slice []` becomes:

```ruby
alias [] __aref_c
alias slice __aref_c
```

60.0 ns, and `s[i]` lands on the same number as `s.__aref(i)` in the same
binary (58.6 ns), which is what says B is paying for the send and nothing
else.
