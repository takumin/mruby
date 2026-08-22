# mruby-alloc-fault

Drives a scenario with one of its allocations refused, so that what mruby
does when memory runs out is tested rather than assumed.

The gem is not part of any gembox.  A build asks for it, and
`build_config/ci/alloc-fault.rb` is the build that does, with the address and
undefined sanitizers on: a refusal that leaks, that leaves a dangling
pointer, or that walks off the end of an allocation is reported there, where
an ordinary run would swallow it.

## How the refusal is injected

Every allocation mruby makes goes through `mrb_basic_alloc_func()` -- from
`mrb_malloc()` and friends in `src/gc.c`, from the first allocation
`mrb_open_core()` makes in `src/state.c`, and from the parser pool in
`src/mempool.c`.  An application replaces the allocator by defining that
function itself (`doc/guides/memory.md`), and the driver does exactly that:
its definition is linked into the executable, so the linker never pulls
`src/allocf.o` out of libmruby.  Nothing else in the build is affected, and
there is no second definition to collide with.

The allocator counts the requests it is armed for and refuses the one it was
told to:

* `--fail-at N` refuses the N-th request and every one after it, which is
  what "there is no memory left" looks like.
* `--fail-once N` refuses the N-th request alone, so that the collection
  `mrb_realloc_simple()` runs on a refusal has room to hand back.  This is
  the recovery path, and the one where a scenario reaches the checks it
  makes after rescuing `NoMemoryError`.

A free is not a request and is not counted.  The retry `src/gc.c` makes
after a refusal is, because it is another call into the allocator.

## Running one scenario

```console
$ rake MRUBY_CONFIG=ci/alloc-fault
$ build/alloc-fault/bin/mruby-alloc-fault -f mrbgems/mruby-alloc-fault/scenarios/array.rb --count
allocations: 289
refusals: 0
outcome: ok
$ build/alloc-fault/bin/mruby-alloc-fault -f mrbgems/mruby-alloc-fault/scenarios/array.rb --fail-at 29
```

`-e CODE` runs a scenario given on the command line, `-f FILE` one from a
file, and `-c NAME` one of the two C scenarios, `open` and `open-core`,
which drive `mrb_open()` and `mrb_close()` themselves.  Those two are the
part no Ruby scenario can reach, because they are what runs the Ruby.

The outcomes it reports:

| outcome        | means                                                        |
|----------------|--------------------------------------------------------------|
| `ok`           | the scenario ran to its end                                   |
| `nomem`        | it stopped at a `NoMemoryError`, which is what a refusal raises |
| `open-failure` | `mrb_open()` answered a state that cannot be used             |
| `exception`    | something other than `NoMemoryError` came out                 |
| `broken`       | the state no longer worked once the refusals stopped          |

The first three are accepted (`open-failure` only while a refusal is being
injected) and exit 0.  The last two exit 1 and are findings.  A usage error
exits 2.

A Ruby scenario is compiled with the injection disarmed and only then run
with it armed, so that the index names an allocation of the scenario's
execution rather than one of its parse; `--compile-in-scope` puts the
compile in the armed region as well, and `--with-open` extends the region
over `mrb_open()`.

After the run, and before the state is closed, the driver asks the state to
build a few strings, arrays and hashes with the faults off.  A state that
raised `NoMemoryError` is expected to carry on, so a failure there is a
finding about what the unwinding left behind (`--no-recheck` turns it off).

## Driving the refusals from inside a scenario

The driver defines `AllocFault` for a scenario that wants to arm the faults
itself:

```ruby
n = AllocFault.count { "x" * 100_000 }      # allocations the block asks for
AllocFault.fail_at(n / 2) { "x" * 100_000 } # refuse from the middle on
AllocFault.fail_once(3) { [1, 2, 3] * 100 } # refuse one, let the retry work
AllocFault.armed?                           # true inside such a block
```

The module is absent under `--with-open`, where arming it would collide with
the refusal already running.

## The sweep

`sweep.rb` counts a scenario's allocations and then starts the driver once
per index, so that every allocation is, in one run or another, the one that
fails.  Runs are separate processes: a refusal leaves state the next run must
not inherit, and a crash has to be survivable.

```console
$ rake test:alloc-fault MRUBY_CONFIG=ci/alloc-fault
$ ruby mrbgems/mruby-alloc-fault/sweep.rb --bin build/alloc-fault/bin/mruby-alloc-fault \
    --only array,hash --mode sticky --log-dir /tmp/sweep
```

A run is a finding when the driver reports an outcome it does not accept,
when a sanitizer reports on it, when it dies of a signal, or when it stops
answering within `--timeout`.  `--log-dir` writes the output of each one,
with the command that produced it on the first line.

`--shard I/N` takes an even slice of the work, for a CI job that runs the
sweep in parallel across several runners, and `--max-index` stops each
scenario early.

### Known failures

Findings are grouped by signature rather than by allocation index: the
index moves whenever anything above it allocates one more time, and the code
that was holding the memory does not.  A signature is what went wrong and
where, as in `leak:ea_dup` or `ub:obj_free` -- the first frame that is
neither the allocator plumbing nor the sanitizer's own machinery.

`known_failures.txt` lists the signatures already understood, one
`scenario signature note` per line.  A finding that matches is reported as
`[known]` and does not fail the sweep; a signature that is not in the list
does, however many runs it appears in.  So the sweep stays a gate: what it
catches is a *new* way of mishandling a refusal, which is what a change
would introduce.

`--ignore-known` reports them all as findings, and `--emit-known` prints
what was found in the file's own format, which is how the list is written
and checked against what the tree does now.

The list is meant to shrink.  A signature that leaves it must not come
back.

### Determinism

The sweep needs the same scenario to ask for its allocations in the same
order every time, so that the index that counts and the index that refuses
name the same allocation.  That is why `build_config/ci/alloc-fault.rb`
holds back mruby-task: its POSIX HAL installs a periodic `SIGALRM`, and a
handler that runs on wall-clock time takes that away.

## Writing a scenario

A scenario is a plain Ruby file in `scenarios/`.  It should

* work the allocator rather than the object heap -- long strings, growing
  arrays and hashes, many symbols -- since most Ruby-level allocation is
  handed out of GC pages that are already there;
* rescue `NoMemoryError` around each part, and then check that what it was
  working on is still the value it was.  Under `--fail-once` those checks
  run, and an operation that gives up halfway is caught leaving a receiver
  behind that it changed but never finished changing;
* stay small.  The sweep runs one process per allocation, so a scenario that
  allocates ten thousand times costs ten thousand runs;
* keep to what the build has, and do no I/O.
