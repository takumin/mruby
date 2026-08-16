# mruby-bin-mtdump

Dumps the method tables of a freshly opened `mrb_state`, one line per method,
so that a `Class#method` name can be mapped to the C function the VM would
actually dispatch to.

This exists for `tools/method_uftrace.rb` and `tools/mruby_method_index.rb`.
It is a development tool: it is not in any gembox, and is enabled only from
`build_config/host-gprof.rb`.

## Why it is not just a source scan

`tools/mruby_method_index.rb` can also build the same mapping by reading the C
sources, and does so by default. That scanner has to guess three things this
tool knows:

- **which class owns a method.** Most core methods are registered through
  `MRB_MT_ENTRY` ROM tables, which name no class at the call site — the class
  is a local `struct RClass *`. Here the owning class is the table the method
  is in.
- **whether the method exists in this build.** `Kernel#p` is written in
  `src/kernel.c` behind `#ifndef HAVE_MRUBY_IO_GEM`, so a build that includes
  mruby-io does not have it. A text scanner indexes it anyway.
- **what an alias points at.** An alias is an entry in the table holding the
  proc it forwards to, so it is followed rather than matched up by name.

What the tables cannot report is where a registration was _written_: they hold
a function pointer, not a source line. The source scan supplies that, and
`MethodIndex.merge` puts the two halves together.

## Usage

```console
$ MRUBY_CONFIG=host-gprof rake
$ build/host/bin/mruby-mtdump | head -3
!mtdump  2
!anchor  mrb_open  0x55e2a1b7c440
m  Array  #  &  cfunc  0x55e2a1b8e310  rom  -
```

(The separator is a tab; it is shown here as spaces.)

Read it back through the index tool, which resolves the addresses to symbols:

```console
$ ruby tools/mruby_method_index.rb --runtime --class String
$ ruby tools/method_uftrace.rb --runtime --method 'String#slice' --expr '"abc".slice(1)'
```

## Format

A header line `!mtdump <version>`, an `!anchor <symbol> <address>` line, one
`m` line per method, and a trailing `!stats` line. Fields on an `m` line are
tab separated:

| field     |                                                                                                  |
| --------- | ------------------------------------------------------------------------------------------------ |
| `class`      | owning class, e.g. `String`, `Socket::Option`                                    |
| separator    | `#` for an instance method, `.` for a singleton method                           |
| `name`       | method name                                                                      |
| kind         | `cfunc`, `proc`, or `undef`                                                      |
| target       | runtime address for `cfunc`, `file:line` for `proc`, `-` otherwise               |
| origin       | `rom` if served from an `MRB_MT_ENTRY` table, else `heap`                        |
| aliased-from | the name this entry was aliased from, or `-`                                     |

An alias is reported as the body it resolves to, not as an indirection to
chase: `kind` and `target` describe what would actually run, and the name it
was aliased from is a column of its own. That resolution follows the alias
proc's `upper`, which is what the VM does before it runs the method, so it
works even when the method aliased from lives in another class —
`Enumerator::Lazy#force` is aliased from `entries`, which is `Enumerable`'s.

`kind` is `alias` only for an alias with no proc under it at all, which
nothing in a freshly opened state produces.

Addresses are runtime addresses, and mean nothing on their own under a
position-independent executable. The `!anchor` line gives the runtime address
of one named function, so the difference between the two, added to what `nm`
reports for the anchor, is the static address of the target — in _this_
binary. Resolve them against `bin/mruby-mtdump`, not against `bin/mruby`.

## Known gaps

- Classes with no `__classname__` are skipped, as are singleton tables whose
  attached object is not a class or module. There is no `Class#method`
  spelling to report those under. The `!stats` line counts them.
- Methods moved into an origin iclass by `Module#prepend` are not walked.
  Nothing in a freshly opened state prepends, and the count is reported on
  stderr if that ever changes.
