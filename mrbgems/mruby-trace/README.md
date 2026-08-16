# mruby-trace

A call tracer for mruby, in the shape of uftrace: it watches every method
call the VM makes and reports where the time went, as folded stacks.

## Building

```ruby
conf.gem :core => 'mruby-trace'
```

The gem defines `MRB_USE_CALL_HOOK` for the whole build, which is what
compiles the call frame hooks into the VM. Nothing else in mruby changes:
without the gem the hooks are not built at all, so a build that does not
ask for tracing pays nothing for it.

`build_config/host-trace.rb` is a ready-made configuration.

## Usage

```ruby
folded = Trace.record { work }
File.write('out.folded', folded)
```

`Trace.record` is the whole tool for most purposes: it clears whatever was
recorded, records the block, and returns the result even if the block
raises. The pieces are also available on their own:

| method                          | what it does                                 |
| ------------------------------- | -------------------------------------------- |
| `Trace.start`                   | begins recording; false if already recording |
| `Trace.stop`                    | stops recording, keeping the data            |
| `Trace.running?`                | whether recording is on                      |
| `Trace.clear`                   | throws the recorded data away                |
| `Trace.folded(unit = :ns)`      | the recorded stacks, as a string             |
| `Trace.record(unit = :ns) { }`  | clear, record the block, return `folded`     |
| `Trace.write(path, unit = :ns)` | writes `folded` to a file (needs mruby-io)   |
| `Trace.size`                    | how many distinct stacks were recorded       |
| `Trace.elapsed`                 | nanoseconds spent with recording on          |

`unit` is `:ns`, `:us`, or `:calls`.

## Output

One line per stack, frames separated by `;`, then the value — FlameGraph's
folded format:

```
<main>;Trace.record;<block> 3341
<main>;Trace.record;<block>;Demo#outer 881
<main>;Trace.record;<block>;Demo#outer;Integer#times 4112
<main>;Trace.record;<block>;Demo#outer;Integer#times;block in Demo#outer 1970
<main>;Trace.record;<block>;Demo#outer;Integer#times;block in Demo#outer;Demo#middle 10728
<main>;Trace.record;<block>;Demo#outer;Integer#times;block in Demo#outer;Demo#middle;Demo#leaf 1530
<main>;Trace.record;<block>;Demo#slot 285
<main>;Trace.record;<block>;Demo#slot= 655
<main>;Trace.record;<block>;Demo.helper 220
```

The number is the time spent in the innermost frame **itself**, children
excluded, so the numbers sum to the traced run rather than counting nested
time again and again. Lines are sorted, so two runs can be diffed.

Straight into a flame graph:

```sh
mruby prof.rb > out.folded
flamegraph.pl out.folded > out.svg
```

Frames are named the way you would say them out loud: `Foo#bar` for
instance methods, `Foo.bar` for singleton ones, `block in Foo#bar` for a
block written inside a method, `<class:Foo>` for a class or module body,
`<main>` and `<fiber>` for the root of a context. A name holding `;` or a
space (`define_method` accepts those) has them spelled as `_`.

## What is covered

Every frame the VM pushes: methods written in Ruby, methods written in C,
blocks, `class`/`module` bodies, `Kernel#send`, `method_missing`, and calls
made from C through `mrb_funcall()`. Attribute accessors are included even
though the VM answers them without a full call.

Each fiber gets its own stack, rooted at `<fiber>`, so its frames are never
mixed into the stack of whoever resumed it. Time is charged to a fiber
only while it runs.

Frames thrown away in bulk — a `Fiber` that is collected before it
finishes, or a state torn down mid-call — are not reported: mruby frees
those call frames without popping them one by one, and their timings die
with them. This costs a sample, never the shape of the stack: the tracer
indexes frames by their depth, so the next call re-establishes the truth.

## Cost

Two timestamps and a hash lookup per call, plus a name lookup the first
time a stack is seen. On a recursive `fib(24)`, which is nothing but call
overhead, tracing runs about 7× slower; code that does real work per call
pays proportionally less.

With tracing stopped the hooks are uninstalled, leaving one NULL check per
call frame. That same `fib(24)` takes 7.7 ms in a build without the gem
and 7.8 ms in one with it, tracing off — the hooks are free until you turn
them on.

Memory is proportional to the number of _distinct stacks_, not to the
number of calls, since samples are folded as they arrive. Deep recursion
therefore costs almost nothing to record, and a run can be traced for as
long as you like.

## Tracing the C side too

This gem traces Ruby-level calls. To see the VM's own C functions, build
with `-pg` or `-finstrument-functions` (see `build_config/host-gprof.rb`)
and run the binary under uftrace; the two views line up, since a Ruby frame
calling into C shows up in both.
