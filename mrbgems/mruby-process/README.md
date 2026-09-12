# mruby-process

`Process` module and `Process::Status` / `Process::Tms` classes for mruby.

## Installation

Add the line below to your build configuration.

```ruby
  conf.gem core: 'mruby-process'
```

It is part of the `stdlib-io` gembox, so `default.gembox` and `full-core.gembox`
already include it. `mruby-signal` and `mruby-struct` come with it: the first
owns the signal table `Process.kill` and `Process::Status#to_s` read names
from, the second the `Struct` `Process::Tms` is one of.

## Implemented methods

| method                            | mruby-process | memo                                     |
| --------------------------------- | ------------- | ---------------------------------------- |
| Process.pid                       | o             | also `$$`                                |
| Process.ppid                      | o             |                                          |
| Process.kill                      | o             | no negative-signal form yet, see below   |
| Process.wait, .wait2              | o             | if the port waits; not `.waitall`        |
| Process.waitpid, .waitpid2        | o             | sets `$?`; if the port waits, see below  |
| Process.clock_gettime             | o             | seven units; symbolic clock ids          |
| Process.clock_getres              | o             | takes `:hertz` too                       |
| Process.times                     | o             | needs a build with Float, see below      |
| Process.getrlimit                 | o             | if the port reads limits, see below      |
| Process.setrlimit                 | o             | if the port writes limits, see below     |
| Process::Tms                      | o             | a Struct, as in CRuby                    |
| Process::Tms#utime, #stime        | o             |                                          |
| Process::Tms#cutime, #cstime      | o             | reaped children only; 0 on Windows       |
| Process::WNOHANG                  | o             | mruby's own value, not the platform's    |
| Process::WUNTRACED                | o             | mruby's own value, not the platform's    |
| Process::CLOCK_REALTIME           | o             | mruby's own value, not the platform's    |
| Process::CLOCK_MONOTONIC          | o             | mruby's own value, not the platform's    |
| Process::CLOCK_PROCESS_CPUTIME_ID | o             | mruby's own value, not the platform's    |
| Process::CLOCK_THREAD_CPUTIME_ID  | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_AS                | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_CORE              | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_CPU               | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_DATA              | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_FSIZE             | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_MEMLOCK           | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_MSGQUEUE          | o             | mruby's own value; Linux                 |
| Process::RLIMIT_NICE              | o             | mruby's own value; Linux                 |
| Process::RLIMIT_NOFILE            | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_NPROC             | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_NPTS              | o             | mruby's own value; FreeBSD               |
| Process::RLIMIT_RSS               | o             | mruby's own value, not the platform's    |
| Process::RLIMIT_RTPRIO            | o             | mruby's own value; Linux                 |
| Process::RLIMIT_RTTIME            | o             | mruby's own value; Linux                 |
| Process::RLIMIT_SBSIZE            | o             | mruby's own value; FreeBSD and NetBSD    |
| Process::RLIMIT_SIGPENDING        | o             | mruby's own value; Linux                 |
| Process::RLIMIT_STACK             | o             | mruby's own value, not the platform's    |
| Process::RLIM_INFINITY            | o             | -1, not the platform's number, see below |
| Process::RLIM_SAVED_CUR           | o             | -2, and never equal to the two others    |
| Process::RLIM_SAVED_MAX           | o             | -3, and never equal to the two others    |
| Process::Status#pid               | o             |                                          |
| Process::Status#to_i              | o             | no `#to_int`; mruby converts nothing     |
| Process::Status#exited?           | o             |                                          |
| Process::Status#exitstatus        | o             |                                          |
| Process::Status#signaled?         | o             |                                          |
| Process::Status#termsig           | o             |                                          |
| Process::Status#stopped?          | o             |                                          |
| Process::Status#stopsig           | o             |                                          |
| Process::Status#coredump?         | o             |                                          |
| Process::Status#success?          | o             |                                          |
| Process::Status#to_s              | o             |                                          |
| Process::Status#inspect           | o             |                                          |
| Process::Status#==                | o             | the raw status decides, not the pid      |
| Process.fork                      |               | inherently non-portable; separate change |
| Process.spawn                     |               | separate change                          |
| Process.exec                      |               | separate change                          |
| Process.exit, .exit!              |               | see mruby-exit                           |
| Process.uid, .gid, ...            |               | separate change                          |
| Process.getpgrp, ...              |               | separate change                          |

## What the port declares

Whether a method exists is the port's to say, since the port is what a build
names and a `hal-process-<conf>` gem may stand in for the bundled ones. Each
port publishes a `process_hal_features.h` in its `include/`, which
`include/process_hal.h` reads before it declares anything. One macro there
guards the prototype, the port's implementation and the method definition, so
a capability the port does not declare is marked not implemented, as mruby-dir
and mruby-io mark theirs: `respond_to?` answers false for it and a call raises
`NotImplementedError`. A port that declares a capability it does not implement
fails to link.

| macro                           | methods                                           | posix | win |
| ------------------------------- | ------------------------------------------------- | ----- | --- |
| `MRB_HAL_PROCESS_HAS_WAIT`      | `Process.wait`, `.waitpid`, `.wait2`, `.waitpid2` | o     |     |
| `MRB_HAL_PROCESS_HAS_GETRLIMIT` | `Process.getrlimit`                               | o     |     |
| `MRB_HAL_PROCESS_HAS_SETRLIMIT` | `Process.setrlimit`                               | o     |     |

`Process::WNOHANG`, `Process::WUNTRACED`, `Process::RLIM_INFINITY` and the two
saved limits beside it are the shape of the call and are defined whether or not
the port waits or reads a limit. The `Process::RLIMIT_*` constants are not: one
is defined for each resource the port has, so `defined?(Process::RLIMIT_NPTS)`
answers what this platform limits, as it does in CRuby; a Linux build has
neither `Process::RLIMIT_NPTS` nor `Process::RLIMIT_SBSIZE`, and a FreeBSD one
has both. What a port has but cannot do for the arguments it was given, a
signal Windows cannot deliver or a pid selector it does not read, fails at the
call site through `errno` instead.

## Architecture

`mruby-process` and `mruby-io` are independent sibling gems. Neither needs the
other to provide its own feature set:

```text
             mruby
               |
       +-------+-------+
       |               |
       v               v
   mruby-io       mruby-process ----> mruby-signal
       |               |                   |
       |               +---> mruby-struct  |
       |               |                   |
       v               v                   v
    io_hal         process_hal         signal_hal
       |               |                   |
   +---+---+       +---+---+           +---+---+
 posix   win     posix   win         posix   win
```

`IO.popen` is the one place the two capabilities meet, and it is served by
`mruby-io`'s own private spawn/wait primitives rather than by anything here.
`mrbgem.rake` names `mruby-io`, `mruby-errno` and `mruby-metaprog` as _test_
dependencies only, for the reasons its comments give.

`mruby-signal` and `mruby-struct` are the two real dependencies. `Process.kill`
takes a signal by name and `Process::Status#to_s` spells one out, and the
signal table both need is `mruby-signal`'s, reached through `signal_hal.h`;
`Process.times` answers a `Process::Tms`, which is the `Struct` `mruby-struct`
defines rather than a class written again here. Nothing runs the other way.
`mruby-time` is not a dependency: the two gems ask the host the same question
directly, so there is no table that could drift between them, and depending on
it would pull a `Time` class into every build that only asked for I/O.

### The HAL boundary

`include/process_hal.h` declares platform-neutral primitives and documents
their contract in full; its comments are the reference for what each function
promises. The port under `ports/<name>/` implements them; a gem named
`hal-process-<conf>` may supply them instead, in which case the bundled ports
are dropped from the build.

The HAL answers OS-level facts and performs OS-level operations, nothing more.
No POSIX type or macro appears above it, and it knows nothing of `$?`, `$$`,
blocks, `Process::Status` or `Process::Tms`: everything Ruby promises lives in
the common sources under `src/`, including which units a clock reading can be
asked for in, the Floats a `Process::Tms` is built from and which resources a
limit can be set on. What a signal is _called_ is `mruby-signal`'s to answer,
and both callers reach its HAL directly.

### Process::Status and mruby-io

`mruby-io` sets `$?` after an `IO.popen` stream closes by building a status
through `mrb_obj_new()` when the class happens to be defined — the same
allocate-and-`#initialize` path `Process.waitpid` takes, since
`Process::Status.new` is undefined. A status stores only the pid and the raw
platform status and asks the HAL afresh for every question, so one `mruby-io`
built reads exactly like one this gem reaped; `src/status.c` describes the
seam.

The tests exercise that seam on POSIX only: on Windows `mruby-io` hands out a
process handle as `IO#pid` and never sets `$?`, both of which are `mruby-io`'s
to fix.

### Design decisions

The full rationale for each decision lives as a comment beside the code it
constrains; this list is a map.

- The wait returns a raw status, decoded separately, so a status that arrived
  from `mruby-io` decodes through the same path (`src/status.c`).
- `raw_status` is permanent, not a compatibility detail: it is what `#to_i`
  returns and the only thing a status needs to store.
- A port says in its `process_hal_features.h` what it does not implement at
  all, and the method is marked not implemented; an operation it has but
  cannot do for these arguments fails through `errno` (`ENOSYS`), so a program
  is told at the call site what this platform will not do.
- `Process::Status.new` is undefined, as in CRuby; the allocator is left
  alone so `mrb_obj_new()` keeps working (`src/status.c`).
- A `Process::Status` is frozen once built; a subclass instance is not
  (`status_initialize` in `src/status.c`).
- Wait flags and clock ids are mruby's own numbers, and a value naming none of
  them is refused in the common layer before a port sees it
  (`include/process_hal.h`).
- A clock reading crosses the HAL as `int64_t` seconds and nanoseconds, never
  as a Float and never as `mrb_int` (`mrb_process_clock_time` in
  `include/process_hal.h`).
- The unit is resolved entirely above the HAL: in a build without `Float` the
  float units raise `NotImplementedError` at the call site, and a reading too
  large for the build's Integer becomes a bigint, or `RangeError` where there
  are none (`src/clock.c`).
- `Process.clock_getres` arrives with `Process.clock_gettime` and answers the
  granularity of the way a clock is read, never a period the clock is promised
  to advance on (`mrb_hal_process_clock_getres` in `include/process_hal.h`).
- A pid, signal or raw status too large for the platform is refused with
  `RangeError` in the common layer, where that can be said; `errno` has no
  spelling for it (`mrb_process_int_arg` in `src/process.c`,
  `status_initialize` in `src/status.c`).
- `Process.times` crosses the HAL as four more clock readings, never as ticks
  or a Float, and the conversion to Float happens once above it
  (`mrb_process_times` in `include/process_hal.h`).
- `Process.times` needs a build with Float, whole: it takes no unit argument
  to name an Integer answer by, so `MRB_NO_FLOAT` raises `NotImplementedError`
  rather than the method disappearing (`process_times` in `src/clock.c`).
- `Process::Tms` is the `Struct` CRuby's own is, with nothing left to decode
  once built, so `Tms.new` stays public where `Process::Status.new` is
  undefined (`mrb_process_clock_init` in `src/clock.c`).
- Whether `<sys/resource.h>` exists is asked of the compiler by `mrbgem.rake`
  (`check_header`), not guessed inside the port; a target without it compiles
  the `times(2)` fallback.
- Whether the host has `getrlimit(2)` and `setrlimit(2)` is asked the same way
  (`check_func`), one call at a time; which resources it has is the
  preprocessor's to answer, as it is in CRuby's `process.c` (`mrbgem.rake`,
  `ports/posix/process_hal.c`).
- Resource ids are mruby's own numbers, as the clock ids are, and the names
  are CRuby's: the constant's spelling without the `RLIMIT_` prefix
  (`mrb_process_rlimit_id` in `include/process_hal.h`).
- A `Process::RLIMIT_*` constant is defined for each resource the port has and
  for no other: half of them are one operating system's alone, where all four
  clock constants are defined everywhere (`mrb_process_rlimit_init` in
  `src/rlimit.c`).
- A limit crosses the HAL as a `uint64_t` and the kind of answer it is, never
  as `rlim_t`: a port hands up no placeholder as though it were a limit
  (`mrb_process_rlimit_kind` in `include/process_hal.h`).
- The three answers that are not numbers are `-1`, `-2` and `-3` in Ruby,
  mruby's own values as the resource ids are, and stay distinct on a host that
  spells all three alike (`rlimit_answers` in `src/rlimit.c`).
- A negative number that names none of them is refused with `RangeError` in
  the common layer, where the size of a number can be said, before a port has
  to narrow it into an unsigned type (`rlimit_limit_arg` in `src/rlimit.c`).
- The POSIX port reads and writes through `getrlimit64(2)` where its own
  `rlim_t` is too narrow and the host has the wider calls; an illumos 32-bit
  build then answers a saved limit as the number it is (`mrbgem.rake`,
  `ports/posix/process_hal.c`).

## Deviations from CRuby

- `Process.kill` does not name a process group through the signal yet: a
  negative signal number, or a name written with a leading `-`, raises
  `ArgumentError` rather than quietly signalling the process. The `pid`
  selectors are untouched and reach the platform as written.
- A clock can be named by the Symbol its constant is named with, as in CRuby.
  Only the four portable clocks exist; CRuby's platform-specific clocks and
  emulation names are not here, and such a name raises `Errno::EINVAL` as any
  unknown name does.
- On Windows each clock is read by one Win32 call and its resolution is that
  call's granularity; on the rare POSIX host without `clock_gettime(2)` the
  wall clock reads through `gettimeofday(2)` at microsecond resolution. The
  port sources detail the calls.
- On Windows only `KILL` and `TERM` can be delivered (as
  `TerminateProcess()`), signal 0 asks whether the process can be opened, and
  any other signal fails with `ENOSYS`. A raw status is the child's exit code
  and nothing more, so a status always reads as exited. The port declares no
  wait until `Process.spawn` exists to make children;
  `ports/win/include/process_hal_features.h` says why.
- On Windows `Process::Tms#cutime` and `#cstime` always read `0.0`: Win32
  reports no reaped child's CPU time, and CRuby's Windows build answers the
  same way.
- `Process::RLIM_INFINITY` is `-1`, where CRuby answers with the platform's own
  number, of which there is no one spelling: Linux writes 18446744073709551615,
  the BSDs and macOS 9223372036854775807, illumos `(rlim_t)-3`. Linux's is past
  `mrb_int` in every build, and past every Integer of a build without
  `mruby-bigint`, so answering with it would leave `Process.getrlimit` raising
  `RangeError` for an ordinary machine's unlimited resources.
- `Process::RLIM_SAVED_CUR` and `Process::RLIM_SAVED_MAX` are always distinct
  from `Process::RLIM_INFINITY` and from each other, where CRuby defines all
  three as one number wherever the platform spells them alike. Linux, the BSDs
  and macOS do: a program comparing a limit against the saved ones reads there
  as though every unlimited resource had a limit held back. On illumos each has
  its own value, and it is the host these three are for.
- A name on the list that this platform has no resource for, `:NPTS` on Linux
  say, raises `Errno::EINVAL`: the errno a platform answers for a resource it
  does not have. CRuby knows only the names its own host has and raises
  `ArgumentError` for the rest. A name the list does not hold at all raises
  `ArgumentError` here too.
- A negative limit that names none of the three answers raises `RangeError`.
  CRuby reads one as the unsigned number of the same bits, so
  `Process.setrlimit(:CORE, -4)` sets a limit of 18446744073709551612 there
  wherever `rlim_t` is 64 bits wide.

## Adding a port

Create `ports/<name>/` with an `include/process_hal_features.h` declaring what
the port implements and sources implementing every function
`include/process_hal.h` declares under it, then build with
`conf.ports :<name>, :posix` so gems without a `<name>` port fall back. Every
`.c` under the directory is compiled; the bundled ports keep the clocks apart
in `clock_hal.c` and the rest in `process_hal.c`, which is a convenience rather
than a rule. A port that cannot do something for the arguments it was given
should set `errno` to `ENOSYS` and return the documented failure value rather
than pretending to succeed; something it cannot do at all it leaves
undeclared.
