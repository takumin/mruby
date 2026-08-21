# mruby-process

`Process` module and `Process::Status` for mruby.

## Installation

Add the line below to your build configuration.

```ruby
  conf.gem core: 'mruby-process'
```

It is part of the `stdlib-io` gembox, so `default.gembox` and `full-core.gembox`
already include it.

## Implemented methods

| method                            | mruby-process | memo                                     |
| --------------------------------- | ------------- | ---------------------------------------- |
| Process.pid                       | o             | also `$$`                                |
| Process.ppid                      | o             |                                          |
| Process.kill                      | o             | no negative-signal form yet, see below   |
| Process.spawn                     | o             | absent under `MRB_NO_PROCESS_SPAWN`      |
| Process.wait, .wait2              | o             | `.waitpid2` too; not `.waitall`          |
| Process.waitpid                   | o             | sets `$?`                                |
| Process.detach                    | o             | returns nil, not a Thread                |
| Process.clock_gettime             | o             | seven units; symbolic clock ids          |
| Process.clock_getres              | o             | takes `:hertz` too                       |
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

## Creating and waiting for a child

```ruby
pid = Process.spawn("sleep 1")   # through the shell
Process.waitpid(pid)             # -> pid, and $? says how it finished
$?.success?                      # -> true

Process.spawn("echo", "hello")   # two or more arguments: no shell involved
Process.wait                     # -> the pid of whichever child finished
```

`Process.spawn` takes CRuby's argument shape: an optional environment Hash
first, the command, and an optional options Hash last.

```ruby
Process.spawn({"LANG" => "C", "TZ" => nil}, "date")  # a nil value unsets
Process.spawn("cmd", out: io, err: [:child, :out])   # ... 2>&1
Process.spawn("cmd", out: "log.txt", chdir: "/tmp")
```

The redirection table is applied in the order it is written, so a later entry
sees what an earlier one did. As in CRuby, `err: :out` is _not_ `2>&1`: a bare
`:out` names the parent's descriptor 1, and merging inside the child is
written `err: [:child, :out]`.

`[cmdname, argv0]`, `pgroup`, `umask` and `rlimit_*` are not supported.

## Who owes the wait

Every child this interpreter spawns owes one wait, and what owes it is a
record rather than a pid. A pid is a label the platform may hand to another
process once the first has been reaped, so what a wait is given internally is
the child itself: a pid on POSIX, a process HANDLE on Windows. Nothing of that
reaches Ruby, where a pid is what CRuby takes and what is taken here.

```ruby
pid = Process.spawn("sleep 1")
Process.waitpid(pid)         # -> pid, and $? says how it finished
```

- `Process.waitpid(pid)` looks the number up among the children this
  interpreter spawned and has not yet accounted for, and waits by identity, so
  a pid the platform has since handed to a stranger cannot be what is waited
  for. Any other pid, one already reaped or one that was never this
  interpreter's, raises `Errno::ECHILD`.
- A record exists exactly while its child owes a reap, so a child waited for
  once is `Errno::ECHILD` the second time, as it is in CRuby.
- `Process.detach(pid)` gives up the obligation without waiting. On POSIX the
  child's status slot then stays until the host process exits.
- At `mrb_close`, every child still owing a reap gets one non-blocking wait
  and is then let go of. A blocking wait there would let a child that never
  finishes hang the interpreter's close, which is worse than the zombie the
  child is left as.

## Architecture

`mruby-process` and `mruby-io` are sibling gems. Neither includes the other's
headers, neither declares a build dependency on the other, and neither is
needed for the other's core feature set:

```text
             mruby
               |
       +-------+-------+
       |               |
       v               v
   mruby-io       mruby-process ----> mruby-signal
       |               |                   |
       v               v                   v
    io_hal         process_hal         signal_hal
       |               |                   |
   +---+---+       +---+---+           +---+---+
 posix   win     posix   win         posix   win
```

`IO.popen` is where the two meet, and it is a composition rather than a third
implementation: `mruby-io` writes it in `mrblib` as `IO.pipe` plus
`Process.spawn`, resolves the `Process` constant when the method is called,
and calls public methods on it. Nothing links across the gems. A build with
`mruby-io` and without `mruby-process` still builds, and `IO.popen` raises
`NotImplementedError` at the call site.

Process creation and process reaping therefore live in one place. There is one
reaper in the build, and `mruby-io` is no longer it: waiting for a child is
`Process`'s, and `IO#close` asks for it through `Process.waitpid`, which is
the whole of what the two gems say to each other besides `Process.spawn`.

`mruby-signal` is a real dependency: `Process.kill` takes a signal by name and
`Process::Status#to_s` spells one out, and the signal table both need is
`mruby-signal`'s, reached through `signal_hal.h`. Nothing runs the other way.
`mruby-time` is not a dependency: the two gems ask the host the same question
directly, so there is no table that could drift between them, and depending on
it would pull a `Time` class into every build that only asked for I/O.

`mrbgem.rake` names `mruby-errno` as a _test_ dependency: a gem's tests run in
a state holding its dependency closure and nothing else, so naming an `Errno`
class in an assertion means asking for the gem that defines them.

### The HAL boundary

`include/process_hal.h` declares platform-neutral primitives and documents
their contract in full; its comments are the reference for what each function
promises. The port under `ports/<name>/` implements them; a gem named
`hal-process-<conf>` may supply them instead, in which case the bundled ports
are dropped from the build.

The HAL answers OS-level facts and performs OS-level operations:

- `mrb_hal_process_pid()`, `mrb_hal_process_ppid()` — the native process
  identity, widened to `mrb_int`.
- `mrb_hal_process_context_init()` / `_free()` — the context every child of
  this interpreter is spawned into. Empty on POSIX, where the kernel already
  knows what this process's children are; the live handles on Windows, where
  nothing else does.
- `mrb_hal_process_spawn()` — creates a child from a `mrb_process_spawn_params`
  and hands back an opaque `mrb_hal_process_child`.
- `mrb_hal_process_wait()` — waits over the set of children an
  `mrb_process_wait_target` names, and reports an `mrb_process_event` carrying
  an already decoded `mrb_process_status`.
- `mrb_hal_process_child_release()` — lets go of a child, once.
- `mrb_hal_process_kill()` — delivers a signal.
- `mrb_hal_process_clock_gettime()` / `_getres()` — read a clock, and the
  granularity the way of reading it can tell two moments apart by.

Wait-one, wait-any and wait-group are one primitive because they are one
system call with a different argument, and because emulating any of them from
the others cannot be done honestly: polling live children with a non-blocking
wait in a loop is not a blocking wait, and it burns the CPU while pretending
otherwise. Those three are the sets a wait can draw from and there is nothing
else, because nothing else is expressible: POSIX `waitpid()` has no form that
takes an arbitrary subset.

Every one of the three is a set of this interpreter's own children. A group
scope narrows among them; it does not reach a process this one did not create,
which is why owning children and honouring the group selectors are not in
tension. `waitpid(0)` in a populated process group with no child of the
caller's in it answers `ECHILD`, not somebody else's status.

The common sources under `src/` implement everything Ruby promises: the module
and class definitions, argument shapes and conversions, the child record table
and its rules, `Process.waitpid` return semantics, the teardown policy, `$?`
and `$$`, every `Process::Status` method, and which units a clock reading can
be asked for in. `Process.spawn`'s argument decomposition is in
`mrblib/process.rb`, so that no Hash / Array / IO case analysis accumulates in
C: `src/spawn.c` receives flat integer and string arrays. What a signal is
_called_ is `mruby-signal`'s to answer, and both callers reach its HAL
directly.

No POSIX type or macro — `pid_t`, `WIFEXITED`, `WEXITSTATUS`, `SIGTERM`,
`WNOHANG` — appears above the HAL, and the HAL knows nothing of `$?`, `$$`,
blocks, `Process::Status` or the record table.

A pid or a signal too large for the platform is refused above the line as
well. What is wrong with such a value is its size, and size is not something
a port can report: the HAL answers with an `errno`, which has no spelling for
"that was never a pid" and would have to borrow one that means something else,
leaving `Errno::ESRCH` to stand both for "no process there" and for "that was
never a process id". So the value is refused where `RangeError` can be said,
which is what Ruby raises for it. The ports keep their own range guards, so
that each is correct on its own.

A wait flag no port stands for is refused above the line rather than in the
ports: `MRB_PROCESS_WAIT_FLAGS` names every bit a wait may carry, and anything
else is `EINVAL` before a port is asked. A port answers for the bits it knows
and has no name for the rest, so leaving the check to the ports would leave
the answer to each of them.

### A status is a snapshot

A wait status is decoded by the port that produced it, at the moment it was
produced, and a `Process::Status` keeps the result. It is not a question
re-asked of the platform later, because a status outlives the child it came
from, and by then the pid it was decoded for may belong to someone else.

That is also why there is no way to build one by hand: `Process::Status.new`
is undefined. A status comes from a wait, and is frozen once built, as CRuby
freezes the one it leaves in `$?`.

`Process::Status#to_i` returns the platform value the status was decoded
from. Its layout is the platform's business; nothing above the port reads it.

### Design decisions

The full rationale for each decision lives as a comment beside the code it
constrains; this list is a map.

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

## Deviations from CRuby

- `Process.kill` does not name a process group through the signal yet: a
  negative signal number, or a name written with a leading `-`, raises
  `ArgumentError` rather than quietly signalling the process. The `pid`
  selectors are untouched and reach the platform as written.
- On a platform whose waits cannot be narrowed to a process group, the `pid`
  selectors 0 and below -1 raise `Errno::ENOSYS`. Windows is one: a process
  group there is what `GenerateConsoleCtrlEvent()` addresses, and nothing a
  wait can be filtered by.
- `Process.detach` returns nil. CRuby returns a Thread that does the waiting;
  mruby has no threads.
- `Process::Status` cannot be constructed, and `Process::Status.new(pid, raw)`
  is not available.
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
  any other signal fails with `ENOSYS`.
- On Windows a wait status is the child's exit code and nothing more, so a
  status there always reads as exited — even for a process this gem
  terminated — and no child is ever reported as stopped.
- On Windows only descriptors 0, 1 and 2 can be redirected: `STARTUPINFO` has
  three slots and no more, and anything else fails with `ENOTSUP`. A blocking
  wait-any over more than `MAXIMUM_WAIT_OBJECTS` live children fails with
  `EINVAL`; a non-blocking one is not limited.

## Build configuration

`MRB_NO_PROCESS_SPAWN` builds the gem without process creation. `Process.spawn`
is then not defined at all, rather than defined and always failing, so a
program can ask `Process.respond_to?(:spawn)` before it commits to a plan that
needs a child. It is set automatically for iOS, where a process may not spawn
another. Everything else — `Process.pid`, signals, `Process::Status` — is
unaffected.

`IO.popen` follows: with no `Process.spawn` to build on, it raises
`NotImplementedError`.

## Adding a port

Create `ports/<name>/process_hal.c` implementing every function in
`include/process_hal.h`, then build with `conf.ports :<name>, :posix` so gems
without a `<name>` port fall back. A port that cannot do something should set
`errno` to `ENOSYS` — or `ENOTSUP` for a redirection it cannot express — and
return the documented failure value rather than pretending to succeed.
