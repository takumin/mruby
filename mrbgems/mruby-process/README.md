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
| Process::Tms                      | o             | a Struct, as in CRuby                    |
| Process::Tms#utime, #stime        | o             |                                          |
| Process::Tms#cutime, #cstime      | o             | reaped children only; 0 on Windows       |
| Process::WNOHANG                  | o             | mruby's own value, not the platform's    |
| Process::WUNTRACED                | o             | mruby's own value, not the platform's    |
| Process::CLOCK_REALTIME           | o             | mruby's own value, not the platform's    |
| Process::CLOCK_MONOTONIC          | o             | mruby's own value, not the platform's    |
| Process::CLOCK_PROCESS_CPUTIME_ID | o             | mruby's own value, not the platform's    |
| Process::CLOCK_THREAD_CPUTIME_ID  | o             | mruby's own value, not the platform's    |
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
| Process::Sys.getuid               | o             | `.geteuid`, `.getgid`, `.getegid` too    |
| Process::Sys.setuid               | o             | `.seteuid`, `.setgid`, `.setegid` too    |
| Process::Sys.setruid              | o             | `.setrgid` too; not everywhere, below    |
| Process::Sys.setreuid             | o             | `.setregid` too                          |
| Process::Sys.setresuid            | o             | `.setresgid` too; not everywhere, below  |
| Process::Sys.issetugid            | o             | not everywhere, see below                |
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

Every `Process::Sys` method is defined either way, since CRuby defines all
fifteen everywhere and marks the missing ones: one whose call the port does not
declare has `mrb_notimplement_m` for a body. A port that declares any call
taking an ID also says which numbers name one. Whether a name can stand for an
ID is two macros more, `MRB_HAL_PROCESS_HAS_UID_BY_NAME` and
`MRB_HAL_PROCESS_HAS_GID_BY_NAME`, which a port declares beside the calls for
each account table it has to read; a method whose table the port did not
declare takes numbers alone and a name is the `TypeError` anything but an
Integer is, as CRuby built without `<pwd.h>` answers. Two rather than one
because the tables are two, and CRuby asks about `<pwd.h>` and `<grp.h>`
separately as well.

The POSIX port asks rather than names: `mrbgem.rake` asks the compiler and
the linker about each call (`check_func`) and writes `HAVE_<CALL>` where the
host has it, and about `getpwnam_r(3)` and `getgrnam_r(3)` the same way, and
the port's feature header reads that. The one platform the header names is
NetBSD, where it takes `HAVE_SETRUID` and `HAVE_SETRGID` back as CRuby's
`process.c` does, the two calls being deprecated there. The posix column
below says where the macro comes out defined.

| macro                             | methods                                           | posix                              | win |
| --------------------------------- | ------------------------------------------------- | ---------------------------------- | --- |
| `MRB_HAL_PROCESS_HAS_WAIT`        | `Process.wait`, `.waitpid`, `.wait2`, `.waitpid2` | o                                  |     |
| `MRB_HAL_PROCESS_HAS_GETUID`      | `Process::Sys.getuid`                             | o                                  |     |
| `MRB_HAL_PROCESS_HAS_GETEUID`     | `Process::Sys.geteuid`                            | o                                  |     |
| `MRB_HAL_PROCESS_HAS_GETGID`      | `Process::Sys.getgid`                             | o                                  |     |
| `MRB_HAL_PROCESS_HAS_GETEGID`     | `Process::Sys.getegid`                            | o                                  |     |
| `MRB_HAL_PROCESS_HAS_SETUID`      | `Process::Sys.setuid`                             | o                                  |     |
| `MRB_HAL_PROCESS_HAS_SETEUID`     | `Process::Sys.seteuid`                            | o                                  |     |
| `MRB_HAL_PROCESS_HAS_SETRUID`     | `Process::Sys.setruid`                            | o on Darwin, FreeBSD and DragonFly |     |
| `MRB_HAL_PROCESS_HAS_SETGID`      | `Process::Sys.setgid`                             | o                                  |     |
| `MRB_HAL_PROCESS_HAS_SETEGID`     | `Process::Sys.setegid`                            | o                                  |     |
| `MRB_HAL_PROCESS_HAS_SETRGID`     | `Process::Sys.setrgid`                            | o on Darwin, FreeBSD and DragonFly |     |
| `MRB_HAL_PROCESS_HAS_SETREUID`    | `Process::Sys.setreuid`                           | o                                  |     |
| `MRB_HAL_PROCESS_HAS_SETREGID`    | `Process::Sys.setregid`                           | o                                  |     |
| `MRB_HAL_PROCESS_HAS_SETRESUID`   | `Process::Sys.setresuid`                          | o, not on macOS or NetBSD          |     |
| `MRB_HAL_PROCESS_HAS_SETRESGID`   | `Process::Sys.setresgid`                          | o, not on macOS or NetBSD          |     |
| `MRB_HAL_PROCESS_HAS_ISSETUGID`   | `Process::Sys.issetugid`                          | o, not on glibc, on musl           |     |
| `MRB_HAL_PROCESS_HAS_UID_BY_NAME` | a name in any of the setters that take a user ID  | o                                  |     |
| `MRB_HAL_PROCESS_HAS_GID_BY_NAME` | a name in any of the setters that take a group ID | o                                  |     |

`Process::WNOHANG` and `Process::WUNTRACED` are the shape of the call and are
defined whether or not the port waits. What a port has but cannot do for the
arguments it was given, a signal Windows cannot deliver or a pid selector it
does not read, fails at the call site through `errno` instead.

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
asked for in and the Floats a `Process::Tms` is built from. What a signal is
_called_ is `mruby-signal`'s to answer, and both callers reach its HAL
directly.

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
- Which credential calls a port has is declared in its
  `process_hal_features.h`, and `src/sys.c` reads the macros rather than the
  platform: the POSIX port reads what `mrbgem.rake` found on the host, and
  the Windows port declares none, since a Windows process carries an access
  token rather than a `uid_t` (see "What the port declares" above).
- `Process::Status.new` is undefined, as in CRuby; the allocator is left
  alone so `mrb_obj_new()` keeps working (`src/status.c`).
- A `Process::Status` is frozen once built; a subclass instance is not
  (`status_initialize` in `src/status.c`).
- Wait flags and clock ids are mruby's own numbers, and a value naming none of
  them is refused in the common layer before a port sees it
  (`include/process_hal.h`).
- A user or group ID crosses the HAL as `int64_t`, and which numbers name one
  is the port's to say through `mrb_hal_process_id_fits()`, since POSIX fixes
  neither width nor sign for `uid_t`: the POSIX port reads both off its types
  and answers with `mrb_process_id_fits_type()` from `include/process_hal.h`,
  which any port with integer IDs can answer from, and asserts at compile
  time that the types fit the transport. The common layer refuses the rest
  with `RangeError`, and an ID the build's Integer cannot hold is a bigint
  where the build has them (`src/sys.c`).
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
- Whether `<sys/resource.h>` exists, which of the fifteen credential calls
  `<unistd.h>` declares and the C library defines, and whether
  `getpwnam_r(3)` and `getgrnam_r(3)` are there to read a name with, is asked
  of the compiler and the linker by `mrbgem.rake` (`check_header`,
  `check_func`), not guessed inside the port from the name of a platform; a
  target without the header compiles the `times(2)` fallback, one without a
  call, glibc for `issetugid(2)`, leaves that `Process::Sys` method
  unimplemented, and one without a lookup takes the ID that lookup would have
  read by number alone.
- A name the account table answers it has no record of raises `ArgumentError`,
  and a lookup that failed before it could answer raises the `Errno::*` it
  failed with, naming the account it was looking for. Which of the two an
  unknown name is is the C library's to decide and is not arranged for here,
  as CRuby does not arrange for it either: `getpwnam_r(3)` answers 0 with no
  record on musl and `ENOENT` on glibc, so an unknown account is an
  `ArgumentError` on the one and an `Errno::ENOENT` on the other.

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
