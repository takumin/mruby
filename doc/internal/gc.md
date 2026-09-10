<!-- summary: Garbage Collector Internals -->

# Garbage Collector Internals

This document describes the internals of mruby's garbage collector
for developers working on `src/gc.c` and related code.

**Read this if you are:** modifying core data structures that hold
object references (and need to add write barriers), debugging
memory leaks or GC-related crashes, tuning GC performance for an
embedded target, or working on the GC code itself.

**For user-facing GC docs**, see
[gc-arena-howto.md](../guides/gc-arena-howto.md) (arena usage in C
extensions) and [memory.md](../guides/memory.md) (heap regions).

## Overview

mruby uses a **tri-color incremental mark-and-sweep** garbage collector
with an optional **generational mode**. The collector runs in small
incremental steps between VM instruction execution, avoiding long
pauses.

## Tri-Color Model

Every heap-allocated object has a color stored in
`RBasic::gc_color` (3 bits):

| Color          | Value  | Meaning                              |
| -------------- | ------ | ------------------------------------ |
| White (A or B) | 1 or 2 | Unmarked, candidate for collection   |
| Gray           | 0      | Marked, but children not yet scanned |
| Black          | 4      | Fully marked and scanned             |
| Red            | 7      | Static/ROM object, never collected   |

The GC uses two white types (A and B) in a flip-flop scheme. At the
start of each GC cycle, the meaning of "current white" is flipped by
XORing the white bits. This avoids recoloring all live objects at
cycle boundaries, which is an O(1) operation instead of O(n).

```c
#define is_dead(s, o) \
  (((o)->gc_color & other_white_part(s) & GC_WHITES) || \
   (o)->tt == MRB_TT_FREE)
```

An object is dead if it still carries the previous cycle's white color.

## Heap Structure

### Heap Pages

Objects are allocated from fixed-size heap pages:

```text
mrb_heap_page
+-- freelist        linked list of free slots
+-- next            next page in heap list
+-- free_next       next page with free slots
+-- old             old generation flag (generational mode)
+-- region          true if carved from a contiguous region
+-- objects[MRB_HEAP_PAGE_SIZE]   RVALUE array (default 1024)
```

Each page holds `MRB_HEAP_PAGE_SIZE` objects (default 1024). On
64-bit systems, a page is approximately 40 KB (40 bytes per slot).

### RVALUE Union

All mruby object types share the same slot size via a C union:

```text
RVALUE = union of {
  RBasic, RObject, RClass, RString, RArray, RHash,
  RRange, RData, RProc, REnv, RFiber, RException, ...
  struct { RBasic header; RVALUE *next; }  (free slot)
}
```

Free slots use the union space for a freelist pointer.

### Freelist

Each page maintains a singly-linked freelist of available slots.
Allocation pops from the freelist; deallocation during sweep
prepends to the freelist. The GC tracks pages with free slots in
`gc->free_heaps` for fast allocation.

### Heap Regions

For embedded systems with fixed memory banks, `mrb_gc_add_region()`
carves heap pages from a user-provided contiguous buffer:

```c
static uint8_t heap_buf[64 * 1024];
mrb_gc_add_region(mrb, heap_buf, sizeof(heap_buf));
```

Region pages are never freed by the GC (even if all objects die).
When region pages are exhausted, allocation falls back to `malloc()`.

## GC Phases

The GC operates as a three-state machine:

```text
GC_STATE_ROOT --> GC_STATE_MARK --> GC_STATE_SWEEP --> GC_STATE_ROOT
```

### Root Scan (GC_STATE_ROOT)

Marks objects directly reachable from the VM:

1. Global variables (`mrb_gc_mark_gv`)
2. GC arena (`gc->arena[0..arena_idx-1]`)
3. All built-in classes (Object, Class, Module, etc.)
4. Top-level self (`mrb->top_self`)
5. Current exception (`mrb->exc`)
6. Execution contexts (VM stacks, call info stacks, active fibers)
7. Task queues (if `MRB_USE_TASK_SCHEDULER` is defined)

After root scanning, the white color is flipped.

### Incremental Marking (GC_STATE_MARK)

Gray objects are popped from the gray stack and their children
marked. Each step processes a limited number of objects:

```text
limit = (GC_STEP_SIZE / 100) * step_ratio
```

With default `step_ratio = 200` and `GC_STEP_SIZE = 1024`, the
limit is 2048 objects per step. After each step, `gc_debt` is
decremented by the actual number of objects processed, so larger
steps repay more debt.

When the gray stack is exhausted, the final marking phase re-marks
the arena and global variables to catch objects created during
marking, then transitions to sweep.

### Sweep (GC_STATE_SWEEP)

Iterates through heap pages. For each object:

- If dead (previous cycle's white): call `obj_free()`, return
  slot to freelist
- If alive: paint with current white for the next cycle

Sweep is also incremental: `gc->sweeps` tracks the current page
position between steps.

## Gray Stack

The gray stack is a fixed-size array of object pointers:

```c
struct RBasic *gray_stack[MRB_GRAY_STACK_SIZE];  /* default 1024 */
size_t gray_stack_top;
mrb_bool gray_overflow;
```

When the stack overflows, `gray_overflow` is set to `TRUE`. During
marking, `gc_gray_rescan()` scans the entire heap to find any gray
objects that could not be pushed. This guarantees correctness at the
cost of a full heap scan.

## Write Barriers

During incremental marking, a black (fully marked) object storing
a reference to a white (unmarked) object creates a dangerous edge
that could lead to premature collection. Write barriers prevent this.

### Field Write Barrier

Used when assigning a specific field:

```c
mrb_field_write_barrier(mrb, parent, child);
```

If `parent` is black and `child` is white:

- During marking or generational mode: paint `child` gray (add to
  gray stack for scanning)
- During sweep: paint `parent` with current white (demote it for
  next cycle)

### General Write Barrier

Used when an object has been modified but the specific child is
not known:

```c
mrb_write_barrier(mrb, obj);
```

Paints `obj` gray and pushes it onto the gray stack for re-scanning.

## GC Arena

The arena protects newly created objects from collection before
they are stored in a reachable location. Every `mrb_obj_alloc()`
automatically pushes the new object onto the arena.

C extensions must save and restore the arena index when creating
many temporary objects to prevent arena overflow:

```c
int ai = mrb_gc_arena_save(mrb);
/* create temporary objects */
mrb_gc_arena_restore(mrb, ai);
```

### Fixed vs Dynamic Arena

- **Dynamic** (default): arena grows by 1.5x when full. Risk of
  unbounded growth if arena is not properly managed.
- **Fixed** (`MRB_GC_FIXED_ARENA`): raises an exception on overflow.
  Arena size is `MRB_GC_ARENA_SIZE` (default 100).

### Permanent Registration

For long-lived C objects that must survive indefinitely:

```c
mrb_gc_register(mrb, obj);    /* add to permanent root */
mrb_gc_unregister(mrb, obj);  /* remove from root */
```

These store objects in a global array that is always marked as
part of the root set.

See [gc-arena-howto.md](../guides/gc-arena-howto.md) for detailed
usage patterns.

## Generational Mode

When enabled (default, unless `MRB_GC_TURN_OFF_GENERATIONAL` is
defined), the GC classifies objects into young and old generations.

### Minor GC

Only processes young objects. Pages where all objects are old are
marked with `page->old = TRUE` and skipped entirely during sweep.
Minor GC always runs to completion in a single step.

### Major GC

A full mark-and-sweep cycle that processes all objects. Triggered
when `gc->live > gc->oldgen_threshold`. Major GC runs
incrementally, like the non-generational collector.

After a major GC completes, the collector reverts to minor GC mode.
The old-generation threshold is recalculated:

```text
oldgen_threshold = live_after_mark * MAJOR_GC_INC_RATIO / 100
```

With `MAJOR_GC_INC_RATIO = 120`, a major GC triggers when live
objects exceed 120% of the last major GC's survivors.

### Transitioning Between Modes

```c
mrb_gc_generational_mode_set(mrb, TRUE);   /* enable */
mrb_gc_generational_mode_set(mrb, FALSE);  /* disable */
```

From Ruby: `GC.generational_mode = true/false`.

## Object Allocation

`mrb_obj_alloc()` is the core allocation function:

1. If `MRB_GC_STRESS` is defined, run a full GC
2. Increment `gc->gc_debt`; if positive, run `mrb_incremental_gc()`
3. Ensure arena has space (`gc_arena_keep`)
4. Pop an object from the freelist of `gc->free_heaps`
5. If no free pages, allocate a new page (`add_heap`)
6. Initialize the object (zero fill, set type and class)
7. Paint with current white color
8. Push onto arena (`gc_protect`)
9. Increment `gc->live`

## Object Freeing

`obj_free()` performs type-specific cleanup:

- **Objects/Exceptions**: free instance variable tables
- **Classes**: free method tables and instance variable tables
- **Arrays**: free heap buffer (if not embedded/shared)
- **Hashes**: free hash table
- **Strings**: free heap buffer (if not embedded/shared)
- **Data objects**: call user-provided `dfree` callback
- **Procs**: decrement irep reference count
- **Fibers**: free context (stacks)

The object's type is set to `MRB_TT_FREE` after freeing.

## Weak Slots: the Frozen String Cache

`String#-@` answers with a frozen string shared between the callers
that ask about the same bytes, and the table it answers out of
(`struct mrb_fstr_cache` in `include/mruby/internal.h`, implemented in
`src/string.c`) is the collector's one **weak** client: a slot naming
a string is not a reason to keep that string alive.

Three paths fill it, all of them through `src/string.c`:

| Path                              | Entry point                             | What it does                                                                  |
| --------------------------------- | --------------------------------------- | ----------------------------------------------------------------------------- |
| `String#-@`                       | `mrb_str_fstring()`                     | answers with the string the cache holds for those bytes, or puts one there    |
| a `String` key stored in a `Hash` | `mrb_str_fstring()`, from `h_key_for()` | stores the shared frozen string as the key rather than a private copy         |
| a frozen string literal           | `mrb_str_fstring()`, from `__fstring`   | `"lit".freeze` is compiled as a call to `__fstring`, which asks for the bytes |

Each of the three is free to answer with a string other than the one it
was handed, since what they promise is a frozen string with those bytes
and nothing about which one. Freezing a string is not among them, as it
is not in CRuby: `freeze` answers with the string that was frozen, so
that string cannot be exchanged for the cache's, and putting it in the
cache would hand it to every later ask about those bytes. A frozen
literal reaches the cache through the first path rather than through
`freeze`: the compiler folds `"lit".freeze` into `__fstring("lit")`
(`gen_call()` in `mrbgems/mruby-compiler/src/codegen.c`), which it may
do because the string the literal makes is one nothing else can be
holding, so a literal frozen inside a loop hands back one string rather
than one per pass.

Three rules make that safe, and all three live in the collector:

1. **Nothing marks a slot.** `gc_mark_children()` never walks the
   table, so a string reachable only from the cache is unreached and
   is collected like any other unreachable string.
2. **A slot is emptied before its string is swept.**
   `sweep_fstr_cache()` runs at the end of `final_marking_phase()`,
   which is the one point where marking has settled and sweeping has
   not started. Every slot naming an unmarked string is emptied there,
   so no lookup between that point and the sweep can answer with a
   string the sweep is about to free.
3. **A string freed on any other path takes itself out.**
   `mrb_gc_free_str()` empties the slot of a string carrying
   `MRB_STR_FSTR`, before the bytes the slot would be found by are
   freed. Rule 2 leaves that flag clear on everything it empties, so
   this is a backstop rather than a second pass.

A lookup that finds a string puts it in the **arena**
(`mrb_gc_protect()`), exactly as a lookup that found nothing would
have put the string it allocated there. Without that, a string held
only by the C caller and by a slot would be unreached at the next
final marking, and rule 2 would drop it while the caller still held
it.

The table costs one pointer per slot and holds no string of its own,
so its whole cost is `MRB_FSTRING_CACHE_MAX * sizeof(void*)` bytes,
reached only by a program that fills it. It is allocated on the first
`String#-@`, doubles when a row fills, and stops at the bound; an
insertion into a full row past the bound drops one of the row's
entries, which costs deduplication and nothing else. `GC.stat` reports
`:fstring_count` and `:fstring_capa`.

### The interned literals

Beside the cache stands a table of the string literals of every irep
that has been loaded (`struct mrb_fstr_literals`, filled by
`mrb_fstr_intern_irep()` as `read_irep()` and `mrb_load_proc()` hand
code over). `mrb_str_fstring()` reads it before the cache, so a program
that asks about the bytes of a literal is answered with the literal,
which is what CRuby answers with: its compiler interns every literal it
compiles.

That table is **strong**, and is the one place these two differ.
`root_scan_phase()` marks every slot, nothing is ever dropped, and the
strings stand until the state is closed. It has to be: a weak slot
would let go of a literal as soon as the program stopped holding one,
and the source would stop answering for bytes it plainly still spells.
What it costs is one string to a distinct literal -- a header alone
where the bytes are read-only data, which is where code compiled into
the binary stands -- and `MRB_NO_FSTRING_LITERALS` builds it out for a
target that would rather not pay it. `GC.stat[:fstring_literal_count]`
says how many stand there; a default build interns about a hundred
before a line of the program runs.

## Triggering GC

### Debt Model

GC uses a **debt-based feedback model** to balance allocation
rate against collection work. The key field is `gc->gc_debt`
(signed integer):

- **Negative** = credit (GC is ahead, no collection needed)
- **Zero** = balanced
- **Positive** = debt (allocation outpacing collection, GC runs)

Each object allocation increments `gc_debt` by 1. When debt
goes positive, `mrb_incremental_gc()` runs. Each incremental
step decrements debt by `GC_STEP_SIZE` (1024), giving credit
for many future allocations.

When a GC cycle completes, credit is calculated from
`interval_ratio`:

```text
credit = (live_after_mark / 100) * interval_ratio - live_after_mark
minimum: GC_STEP_SIZE (1024)
gc_debt = -credit
```

With default `interval_ratio = 200` and 1000 live objects:
`credit = (1000/100)*200 - 1000 = 1000`, so approximately 1000
allocations can occur before the next GC cycle begins.

### Malloc Pressure

When `gc->malloc_threshold` is non-zero (it is `MRB_GC_MALLOC_THRESHOLD`
by default), the GC also tracks bytes allocated through
`mrb_realloc_simple()` in `gc->malloc_increase`. When
`malloc_increase` reaches `malloc_threshold`, the counter resets
and an incremental GC step runs. This captures memory pressure
from large buffers (e.g., long strings) that would otherwise be
invisible to the object-count-based debt model.

Only a fresh allocation may step the collector here; a `realloc`
has already freed the caller's old block and the caller has not
yet stored the new pointer, so marking in that window would walk
freed memory. Byte pressure from reallocs is not lost, it fires
at the next fresh allocation.

### Manual

```c
mrb_full_gc(mrb);          /* force complete GC cycle */
mrb_garbage_collect(mrb);  /* public API wrapper */
```

From Ruby: `GC.start`.

## Configuration

### Compile-Time

| Macro                          | Default | Description                                  |
| ------------------------------ | ------- | -------------------------------------------- |
| `MRB_HEAP_PAGE_SIZE`           | 1024    | Objects per heap page                        |
| `MRB_GRAY_STACK_SIZE`          | 1024    | Gray stack capacity                          |
| `MRB_GC_ARENA_SIZE`            | 100     | Arena size (fixed mode) or initial size      |
| `MRB_GC_FIXED_ARENA`           | off     | Use fixed-size arena                         |
| `MRB_GC_TURN_OFF_GENERATIONAL` | off     | Disable generational mode                    |
| `MRB_GC_STRESS`                | off     | Full GC on every allocation (debug)          |
| `MRB_GC_STATS`                 | off     | Enable GC statistics counters                |
| `MRB_USE_MALLOC_TRIM`          | off     | Call `malloc_trim()` after full GC           |
| `MRB_FSTRING_CACHE_MAX`        | 256     | Slots of the frozen string cache (0=off)     |
| `MRB_NO_FSTRING_LITERALS`      | off     | Leave the literals of loaded code uninterned |

### Runtime

From Ruby code:

```ruby
GC.interval_ratio = 200     # controls debt credit after GC cycle
GC.step_ratio = 200         # objects per incremental step
GC.step_limit = 0           # 0=unlimited, >0=absolute step cap
GC.malloc_threshold = 16777216 # 0=disabled, >0=bytes to trigger GC
GC.generational_mode = true
GC.start                     # force full GC
GC.enable                    # re-enable GC
GC.disable                   # disable GC
```

### GC Statistics

`GC.stat` returns a Hash with GC state and statistics:

```ruby
GC.stat
# => {
#   :live => 5432,              # live object count
#   :debt => -1024,             # GC debt (negative=credit, positive=behind)
#   :state => 0,                # 0=root, 1=marking, 2=sweeping
#   :generational => true,      # generational mode enabled
#   :full => false,             # major GC in progress
#   :step_limit => 0,           # current step limit setting
#   :malloc_increase => 8192,   # malloc bytes since last cycle
#   :malloc_threshold => 16777216, # current malloc threshold setting
#   :fstring_count => 12,       # strings the frozen string cache holds
#   :fstring_capa => 64,        # slots it holds them in
#   :fstring_literal_count => 105, # literals interned as code was loaded
# }
```

The last three keys are absent from a build made with
`MRB_FSTRING_CACHE_MAX=0`, which carries no cache, and the last of them
from one made with `MRB_NO_FSTRING_LITERALS`.

With `MRB_GC_STATS` enabled, additional keys are available:

```ruby
#   :total => 15,               # total GC invocations
#   :minor => 12,               # minor GC count
#   :major => 3,                # major GC count
```

### Tuning Guide

**`interval_ratio`** (default 200): Controls how many allocations
occur between GC cycles. Higher values reduce GC frequency but
increase peak memory. The debt credit after each cycle is
`(live_after_mark / 100) * interval_ratio - live_after_mark`.

**`step_ratio`** (default 200): Controls how much work each
incremental step performs. Higher values make each step larger,
reducing total GC overhead but increasing individual pause times.

**`step_limit`** (default 0, unlimited): Caps the maximum work
per incremental step regardless of `step_ratio`. Useful for
real-time applications that need bounded pause times. The
effective step size is `min(step_ratio calculation, step_limit)`.

**`malloc_threshold`** (default `MRB_GC_MALLOC_THRESHOLD`, 16MiB,
or `SIZE_MAX/4` where `size_t` is too narrow to hold that; 0
disables it): Triggers GC when cumulative `malloc`/`realloc`
bytes reach this threshold. This is what collects large buffers
(strings, data objects) that create memory pressure without a
proportional object count increase; without it a workload that
churns buffers has no debt to pay and the collector never runs.
Lower it to react sooner on a small memory budget.

### Practical Tuning Examples

**Allocation-heavy workloads** (many short-lived Procs, closures,
blocks): GC sweep dominates because of high object churn. Increase
`interval_ratio` to reduce GC frequency:

```ruby
GC.interval_ratio = 400  # ~12% faster than default (200)
```

Higher values (400-600) reduce sweep overhead at the cost of more
dead objects accumulating before collection. Values above 600 show
diminishing returns. Peak memory usage increases temporarily, but
live object count after GC remains the same.

**CPU-intensive workloads** (numeric computation, recursive methods
with no object allocation): GC parameters have negligible impact
because GC rarely runs. No tuning needed.

**Real-time or latency-sensitive** applications: Use `step_limit`
to bound pause times:

```ruby
GC.step_limit = 256  # cap incremental step to 256 objects
```

This makes GC pauses more predictable but increases total GC
overhead (more steps needed per cycle).

**Large buffer workloads** (reading files, building long strings):
`malloc_threshold` already collects these at its 16MiB default.
Lower it to react sooner, at the cost of more collections:

```ruby
GC.malloc_threshold = 1024 * 1024  # trigger GC per ~1MB allocated
```

**Small memory budgets** (a target with a few hundred KiB of RAM):
the default is inert there, because the counter is cleared at the end
of every collection cycle and a small heap collects often enough that
it never accumulates 16MiB. Measured high-water marks of
`malloc_increase` on a host build: 188KB over two million small
allocations, 710KB for `benchmark/bm_ao_render.rb`, and a small heap
collects more often than either. To make the byte axis fire on such a
target, set `MRB_GC_MALLOC_THRESHOLD` to something on the order of the
memory budget. On the two million allocation workload, `65536` leaves
the collection count unchanged and `32768` adds 3% more minor
collections.

### Diagnosing GC Overhead

Use `GC.stat` to monitor GC behavior at runtime:

```ruby
s = GC.stat
puts "live objects: #{s[:live]}"
puts "GC debt: #{s[:debt]}"     # positive = GC is behind
puts "GC state: #{s[:state]}"   # 0=idle, 1=marking, 2=sweeping
```

If `debt` is frequently positive during performance-critical
sections, increase `interval_ratio`. If memory usage is too high,
decrease it.

## Source Files

| File                 | Contents                          |
| -------------------- | --------------------------------- |
| `src/gc.c`           | GC implementation                 |
| `src/string.c`       | Frozen string cache (weak slots)  |
| `include/mruby/gc.h` | `mrb_gc` structure, public GC API |
| `include/mruby.h`    | Arena save/restore macros         |
