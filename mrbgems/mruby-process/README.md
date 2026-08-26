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
| Process.spawn                     | o             | no environment or chdir yet, see below   |
| Process.wait, .wait2              | o             | `.waitpid2` too; not `.waitall`          |
| Process.waitpid                   | o             | sets `$?`; POSIX only, see below         |
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
| Process.fork                      |               | inherently non-portable; separate change |
| Process.exec                      |               | separate change                          |
| Process.system                    |               | spawn plus wait; separate change         |
| Process.exit, .exit!              |               | see mruby-exit                           |
| Process.uid, .gid, ...            |               | separate change                          |
| Process.getpgrp, ...              |               | separate change                          |

## Creating a child

```ruby
pid = Process.spawn("sleep 1")   # no shell involved
Process.waitpid(pid)             # -> pid, and $? says how it finished
$?.success?                      # -> true

Process.spawn("a > b")           # through the shell: a redirection
Process.spawn("a", "> b")        # two or more arguments: an argument
```

A single String is a command line, and it reaches the system shell only when
there is something in it for a shell to do. That is one of the characters

```text
* ? { } [ ] < > ( ) ~ & | \ $ ; ' " ` #    and a newline
```

anywhere in the line, an `=` in its first word, or a first word that is one of
the POSIX shell's reserved words or special built-ins. A command line with
none of those is split on spaces and tabs here and the command run directly,
which is the rule CRuby follows and what makes
`Process.spawn("no-such-command")` raise `Errno::ENOENT` rather than leave a
shell to complain and exit 127.

Two or more arguments name the image and its arguments, and no shell is
involved whatever they hold. A bare name is looked up on `PATH`; a name that
is a path is run as written, and handed to the shell if it turns out not to
be an executable image, as `execvp(3)` hands one over. The lookup asks
`stat(2)` which directories have something of that name before any child is
made, so a command costs one exec wherever it sits on the `PATH`, where
`execvp(3)` would try an exec in every directory before it; whether what is
there can be run is still the exec's to say.

A command that cannot be run raises the `Errno` the attempt failed with,
rather than leaving a child that exited 127 to be told apart from a command
that exited 127 of its own accord. Where the host has a `posix_spawn(3)` that
answers with that `Errno`, the spawn is that one call, and there is no fork
here to be caught holding a lock another thread of the embedding process left
behind. Where it has not, a fork reports what went wrong down a close-on-exec
pipe, which reaches EOF with nothing in it when the exec succeeds.

Which of the two the POSIX port does is `MRB_PROCESS_HAVE_POSIX_SPAWN`, which
the port settles from the host: 1 where its `posix_spawn(3)` is known to
answer for a failed exec, 0 elsewhere, since POSIX lets the call create a
child that exits 127 and say nothing. A build defines it to say otherwise.
Defining it to 0 is also what a run wants where the host would answer but
what stands between the process and the host does not: under valgrind 3.27 a
`posix_spawn(3)` whose exec failed returns 0, and so does one under qemu's
user-mode emulation, which carries the `vfork()` glibc reports through out as
a `fork()`. The fork path reports through a pipe of its own and depends on
neither.

What a `Process.spawn` creates, the caller owes a wait for, through
`Process.waitpid`. Nothing reaps it at `mrb_close`, as nothing reaps it at
exit in CRuby.

## Redirecting the child's descriptors

A trailing Hash is options. `:in`, `:out`, `:err` and plain descriptor numbers
name the child's descriptors, and the value says what to put there:

```ruby
Process.spawn("cmd", out: io)                        # child's 1 -> io
Process.spawn("cmd", out: io, err: [:child, :out])   # ... and 2>&1
Process.spawn("cmd", out: "log.txt")                 # a file, opened here
Process.spawn("cmd", [1, 2] => "log.txt")            # one file for both
Process.spawn("cmd", 3 => :close)
```

The table is applied in the order it is written, so a later entry sees what an
earlier one did. As in CRuby, `err: :out` is _not_ `2>&1`: a bare `:out` names
the parent's descriptor 1, and merging inside the child is written
`err: [:child, :out]`.

A file named for more than one descriptor is opened once and shared by them,
the way `>log 2>&1` shares one open file; opening it once per descriptor would
give each its own offset and each would write over the other. A file is opened
in the parent, by `File.open`, so the HAL never grows a notion of a filename
and every other redirection form works in a build without mruby-io.

`:close_others` closes the descriptors above 2 that the table does not name.
What it closes is what nobody asked for, and an explicit redirection is
asking, so `3 => io, close_others: true` leaves 3 alone. Descriptors this
build opens are close-on-exec already, so it is rarely needed.

`:close_others` is carried by a fork even where a `posix_spawn(3)` carries the
rest, since what is open is what only the child can be asked, and so is a
`1 => 1`, which asks for a descriptor to be left open rather than moved and is
a file action not every host that has `posix_spawn(3)` defines.

An environment for the child, `:chdir` and `:unsetenv_others` are separate
changes. An option this build does not act on is refused with `ArgumentError`
rather than dropped. `[cmdname, argv0]`, `pgroup`, `umask` and `rlimit_*` are
not supported.

Windows has no `Process.spawn` at all: its port cannot create a process yet,
so `process_hal.h` sets `MRB_NO_PROCESS_SPAWN` there and the method is left
undefined rather than defined and always failing. `Process.respond_to?(:spawn)`
is therefore an answer. iOS is the same, and stays that way: a process may not
create another there.

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
- Unsupported operations fail through `errno` (`ENOSYS`) rather than by the
  method being absent, so a program is told at the call site what this
  platform will not do.
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
  are none (`src/process.c`).
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
  rather than the method disappearing (`process_times` in `src/process.c`).
- `Process::Tms` is the `Struct` CRuby's own is, with nothing left to decode
  once built, so `Tms.new` stays public where `Process::Status.new` is
  undefined (`mrb_mruby_process_gem_init` in `src/process.c`).
- Whether `<sys/resource.h>` exists is asked of the compiler by `mrbgem.rake`
  (`check_header`), not guessed inside the port; a target without it compiles
  the `times(2)` fallback.

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
  any other signal fails with `ENOSYS`. A wait status is the child's exit code
  and nothing more, so a status always reads as exited, and `Process.waitpid`
  fails for every process (`ENOSYS` for -1, `ECHILD` for a specific pid) until
  `Process.spawn` exists to make children; `ports/win/process_hal.c` says why.
- On Windows `Process::Tms#cutime` and `#cstime` always read `0.0`: Win32
  reports no reaped child's CPU time, and CRuby's Windows build answers the
  same way.

## Adding a port

Create `ports/<name>/process_hal.c` implementing every function in
`include/process_hal.h`, then build with `conf.ports :<name>, :posix` so gems
without a `<name>` port fall back. A port that cannot do something should set
`errno` to `ENOSYS` and return the documented failure value rather than
pretending to succeed.
