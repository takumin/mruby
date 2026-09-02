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
| Process.spawn                     | o             | absent under `MRB_NO_PROCESS_SPAWN`      |
| Process.wait, .wait2              | o             | `.waitpid2` too; not `.waitall`          |
| Process.waitpid                   | o             | sets `$?`; see below                     |
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
| Process.detach                    |               | needs a waiter; mruby has no threads     |
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

Process.spawn({"LANG" => "C"}, "date")            # a leading Hash is env
Process.spawn("make", chdir: "/tmp/build")
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
be an executable image, as `execvp(3)` hands one over. The lookup is CRuby's
rather than `execvp(3)`'s: each `PATH` directory is asked, before any child
is made, whether it holds something of that name that is not a directory and
that this process may execute, and only what answers is tried. So a command
costs one exec wherever it sits on the `PATH`, where `execvp(3)` would try an
exec in every directory before it, and a name that stands for nothing
runnable anywhere on it is `Errno::ENOENT`, where `execvp(3)` would say
`EACCES` for the directories it stumbled over; `Process.spawn("..")` is the
case in point.

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

The command starts with no signal blocked and with `SIGPIPE` at its default
disposition, whatever this process has done with either, as it does under
CRuby. An exec keeps both, so a process that ignores `SIGPIPE`, as one that
talks to sockets commonly does, would otherwise hand that to every command it
runs, and `yes | head` would spin where it should stop. Any other signal this
process ignores stays ignored in the command, which is what an exec does and
what ignoring it meant; a handler never survives an exec at all.

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

An option this build does not act on is refused with `ArgumentError` rather
than dropped. `[cmdname, argv0]`, `pgroup`, `umask` and `rlimit_*` are not
supported.

## The child's environment and directory

A leading Hash is added to the child's environment, and a `nil` value removes
a variable rather than setting it. `:unsetenv_others` starts the child's
environment empty, and the deltas are still applied to it: they say what to
put in it, not what to change about the parent's.

```ruby
Process.spawn({"LANG" => "C", "TZ" => nil}, "date")
Process.spawn({"PATH" => "/usr/bin"}, "env", unsetenv_others: true)
```

What travels to the port is the deltas, not a whole environment, so nothing
here reads the parent's own and this gem needs no `ENV`. The port assembles
the child's environment before the fork and hands it to `execve()`: `setenv()`
and `unsetenv()` are not calls a child between `fork()` and `exec()` may make.
A `posix_spawn(3)` is handed the same assembled environment, which is the
argument it takes anyway.

A bare command name is looked up on the `PATH` the child is being given rather
than this process's, since that is the one it would have walked itself. So an
environment naming nowhere to look finds nothing, and a name that is a path is
not looked up at all.

`:chdir` runs the child in another directory. `chdir()` is one of the few
calls the child may make, so it happens there, after the redirections and
before the exec. Where the spawn is a `posix_spawn(3)`, it is a file action
appended after the redirections for the same reason, and a host whose
`posix_spawn(3)` has no such action takes the fork instead.

On Windows the child is created by `CreateProcessW()`, and the same argument
shapes reach it: a single String goes to `cmd.exe`, an argv is quoted back
into the one command line Win32 takes, and the environment is assembled into
the sorted UTF-16 block the API asks for. Only descriptors 0, 1 and 2 can be
redirected there, because `STARTUPINFO` has three slots and no more; anything
else is `Errno::ENOTSUP`.

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

- `Process.waitpid(pid)` waits by number, as `waitpid(2)` does, and the
  children it draws from are the running process's. A host application that
  forked a child of its own can be waited for through it, which is the point
  of keeping the two apart: an ownership model that narrowed `Process.waitpid`
  would leave that child waitable through `Process.wait(-1)` and unwaitable
  through its own pid. A pid that names no child of this process is
  `Errno::ECHILD`.
- Waiting by number for a child this interpreter did spawn goes through the
  record anyway, so the wait is given the child itself and not the number that
  labels it: a pid the platform has since handed to a stranger cannot be what
  is waited for.
- A record exists exactly while its child owes a reap, so a child waited for
  once is `Errno::ECHILD` the second time, as it is in CRuby. A wait event
  that is not the end of the child, such as a stop reported through
  `Process::WUNTRACED`, leaves the record where it was.
- At `mrb_close`, every child still owing a reap gets one non-blocking wait
  and is then let go of. A blocking wait there would let a child that never
  finishes hang the interpreter's close, which is worse than the zombie the
  child is left as until the host process exits.

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
       |               +---> mruby-struct  |
       |               |                   |
       v               v                   v
    io_hal         process_hal         signal_hal
       |               |                   |
   +---+---+       +---+---+           +---+---+
 posix   win     posix   win         posix   win
```

`IO.popen` is where the two meet, and it is a composition rather than a third
implementation: `mruby-io` writes it in `mrblib` as `IO.pipe` plus
`Process.spawn`, resolves the `Process` constant when the method is called, and
calls public methods on it. Nothing links across the gems. A build with
`mruby-io` and without `mruby-process` has no `IO.popen` at all, since the file
defining it is left out of that build.

Process creation and process reaping therefore live in one place. There is one
reaper in the build, and `mruby-io` is no longer it: waiting for a child is
`Process`'s, and `IO#close` asks for it through `Process.waitpid`, which is the
whole of what the two gems say to each other besides `Process.spawn`.

`mrbgem.rake` names `mruby-errno` and `mruby-metaprog` as _test_ dependencies:
a gem's tests run in a state holding its dependency closure and nothing else,
so naming an `Errno` class in an assertion, or asking an object for its
`instance_variables`, means asking for the gem that defines them.

`mruby-signal` and `mruby-struct` are the two real dependencies. `Process.kill`
takes a signal by name and `Process::Status#to_s` spells one out, and the
signal table both need is `mruby-signal`'s, reached through `signal_hal.h`;
`Process.times` answers a `Process::Tms`, which is the `Struct` `mruby-struct`
defines rather than a class written again here. Nothing runs the other way.
`mruby-time` is not a dependency: the two gems ask the host the same question
directly, so there is no table that could drift between them, and depending on
it would pull a `Time` class into every build that only asked for I/O.

### Windows and the wide API

Everything this gem hands Win32 goes through the wide entry points, and the
port is where the conversion happens. A mruby String is bytes, holding UTF-8
wherever it came from a Ruby literal, and the ANSI entry points would read one
in whatever code page the machine is set to: a command, a path, an argument or
an environment value spelled outside that code page would reach the child as
something else, or not at all. Nothing above the port knows about UTF-16.

The child is handed duplicates of exactly the handles it is meant to have,
named in a `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. Marking the originals
inheritable and letting `CreateProcess` take every inheritable handle would
leak one spawn's pipes into the next, and a pipe whose write end an unrelated
child still holds never reaches EOF.

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
- `mrb_hal_process_clock_gettime()` / `_clock_getres()` — read a clock, and
  the granularity that reading came out of.
- `mrb_hal_process_times()` — the four CPU time totals behind `Process.times`.

What a child _is_ stays behind the HAL. A pid labels a child, and the platform
may hand the same number to another process once the first has been reaped, so
spawn returns an opaque `mrb_hal_process_child` and every wait takes that
object rather than a number. The port keeps inside it whatever it really needs:
a pid on POSIX, a HANDLE on Windows.

Wait-one, wait-pid, wait-any and wait-group are one primitive because they are
one system call with a different argument, and because emulating any of them
from the others cannot be done honestly: polling live children with a
non-blocking wait in a loop is not a blocking wait, and it burns the CPU while
pretending otherwise. Those four are the sets a wait can draw from and there is
nothing else, because nothing else is expressible: POSIX `waitpid()` has no
form that takes an arbitrary subset.

Only the first of them names a child this interpreter holds. The other three
are the platform's own selectors, and the set they draw from is every child of
the _process_: an embedded interpreter shares that set with the application
that embedded it, which may have forked children the record table never heard
of. A port therefore reports which child answered only when it is one of its
own, and the common layer moves a record only then. What a port cannot do is
invent the missing half: Windows can wait only on a handle it holds, so a pid
it never spawned is `ECHILD` there.

The common sources under `src/` implement everything Ruby promises: the module
and class definitions, argument shapes and conversions, the child record table
and its rules, `Process.waitpid` return semantics, the teardown policy, `$?`
and `$$`, the `Process::WNOHANG` / `Process::WUNTRACED` constants, which units
a clock reading can be asked for in, the Floats a `Process::Tms` is built
from, and every `Process::Status` method.

No POSIX type or macro — `pid_t`, `WIFEXITED`, `WEXITSTATUS`, `SIGTERM`,
`WNOHANG`, `CLOCK_MONOTONIC` — appears above the HAL, and the HAL knows
nothing of `$?`, `$$`, blocks, `Process::Status`, `Process::Tms` or the record
table. What a signal is _called_ is `mruby-signal`'s to answer, and both
callers reach its HAL directly.

### A status is a snapshot

A wait status is decoded by the port that produced it, at the moment it was
produced, and a `Process::Status` keeps the result. It is not a question
re-asked of the platform later, because a status outlives the child it came
from, and by then the pid it was decoded for may belong to someone else.

That is also why there is no way to build one by hand: `Process::Status.new` is
undefined. A status comes from a wait.

`Process::Status#to_i` returns the platform value the status was decoded from.
Its layout is the platform's business; nothing above the port reads it.

### Design decisions

The full rationale for each decision lives as a comment beside the code it
constrains; this list is a map.

- A child is an opaque `mrb_hal_process_child` rather than a pid: the platform
  may hand the same number to another process once the first has been reaped
  (`include/process_hal.h`).
- Wait-one, wait-pid, wait-any and wait-group are one HAL primitive. They are
  one system call with a different argument, and none can be emulated from the
  others honestly (`mrb_hal_process_wait` in `include/process_hal.h`).
- Only wait-one draws from the record table; the other three are the
  platform's own selectors and reach every child of the running process,
  including one the embedding application forked (`src/child.c`).
- A record is reserved before the child exists and committed after it does, so
  the step that can fail is never the one after the OS has acted. A record is
  let go of at exactly one place, and only a terminal event reaches it
  (`src/child.c`).
- At `mrb_close` every child still owing a reap gets one non-blocking wait and
  is then let go of; a blocking wait there would let a child that never
  finishes hang the interpreter's close (`src/child.c`).
- A wait decodes its own status, and nothing else decodes one: the platform's
  bits are read once, in the port that reported them and at the moment it
  reported them (`src/status.c`).
- `raw_status` is permanent, not a compatibility detail: it is what `#to_i`
  returns and the only thing a status needs to store.
- Unsupported operations fail through `errno` (`ENOSYS`) rather than by the
  method being absent, so a program is told at the call site what this
  platform will not do. The one exception is process creation itself, which
  a port declares or does not in its `process_hal_features.h`, and whose
  absence leaves `Process.spawn` undefined (`include/process_hal.h`).
- `Process::Status.new` is undefined, as in CRuby, and nothing else builds one
  either: a status comes from a wait (`src/status.c`).
- A `Process::Status` is frozen once built (`mrb_process_status_new` in
  `src/status.c`).
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
- A pid or a signal too large for the platform is refused with `RangeError` in
  the common layer, where that can be said; `errno` has no spelling for it
  (`mrb_process_int_arg` in `src/process.c`).
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
- On a platform whose waits cannot be narrowed to a process group, the `pid`
  selectors 0 and below -1 raise `Errno::ENOSYS`. Windows is one: a process
  group there is what `GenerateConsoleCtrlEvent()` addresses, and nothing a
  wait can be filtered by.
- `Process.detach` is not implemented. What CRuby's returns is a Thread that
  performs the wait, so that the child is reaped whenever it finishes and no
  zombie is left; mruby has no threads to do that with, and a `detach` that
  only forgot the child would be the zombie it exists to prevent.
- On Windows only `KILL` and `TERM` can be delivered (as
  `TerminateProcess()`), signal 0 asks whether the process can be opened, and
  any other signal fails with `ENOSYS`.
- On Windows a wait status is the child's exit code and nothing more, so a
  status there always reads as exited — even for a process this gem terminated
  — and no child is ever reported as stopped.
- On Windows, `Process.waitpid` answers only for a child this interpreter
  spawned; any other pid is `Errno::ECHILD`. Win32 names a process to wait on
  by handle, and the only handles the port has are the ones it opened by
  spawning; a handle got by opening a process ID stands for any process the
  caller may open rather than for a child of it, and waiting on such a handle
  would report a stranger's exit code as a child's and publish it as `$?`.
- On Windows only descriptors 0, 1 and 2 can be redirected: `STARTUPINFO` has
  three slots and no more, and anything else fails with `Errno::ENOTSUP`. A
  blocking `Process.wait` over more than `MAXIMUM_WAIT_OBJECTS` live children
  fails with `Errno::EINVAL`, since `WaitForMultipleObjects()` takes no more
  than that at a time and running it in chunks would not be a blocking wait
  over the set; a non-blocking one is not limited.
- On Windows `Process::Tms#cutime` and `#cstime` always read `0.0`: Win32
  reports no reaped child's CPU time, and CRuby's Windows build answers the
  same way.

## Build configuration

Whether this build can create a process is the port's to say. The port
publishes it as `MRB_HAL_PROCESS_HAS_SPAWN` in a `process_hal_features.h`
beside its sources, which `process_hal.h` reads before it declares anything;
the POSIX port leaves the macro out on iOS, where a process may not spawn
another, so that platform needs no configuring to be told the truth.
`MRB_NO_PROCESS_SPAWN` is a build's veto: a configuration that defines it gets
the gem without process creation whatever the port could do. Either way
`Process.spawn` is then not defined at all, rather than defined and always
failing, so a program can ask `Process.respond_to?(:spawn)` before it commits
to a plan that needs a child. Everything else, `Process.pid` and signals and
`Process::Status`, is unaffected.

`IO.popen` follows: with no `Process.spawn` to build on, it raises
`NotImplementedError`. A build without this gem at all has no `IO.popen`
either, and mruby-io leaves the method undefined there rather than defining one
that cannot work.

## Adding a port

Create `ports/<name>/process_hal.c` implementing every function in
`include/process_hal.h`, and `ports/<name>/process_hal_features.h` saying
which of the guarded ones it implements; then build with `conf.ports :<name>,
:posix` so gems without a `<name>` port fall back. A capability the port does
not declare has no prototype, no implementation and no method, so a port that
cannot create a process at all leaves `MRB_HAL_PROCESS_HAS_SPAWN` out rather
than writing a `mrb_hal_process_spawn()` that refuses. A port that cannot do
something narrower should set `errno` to `ENOSYS` — or `ENOTSUP` for a
redirection it cannot express — and return the documented failure value rather
than pretending to succeed.

A port that creates a process by forking has one more rule to keep. Between
`fork()` and `exec()` a child may call only what is async-signal-safe, and
mruby needs no threads of its own for that to bite: it is embedded, so the
process it runs in may already have some, and a fork taken while another thread
holds a lock leaves the child holding a lock nobody will release. The bundled
POSIX port therefore assembles the child's environment, resolves the command
against the `PATH` it is being given, and reads `_SC_OPEN_MAX` before it forks,
so that what is left on the other side is `signal()`, `sigprocmask()`,
`dup2()`, `close()`, `chdir()` and `execve()`. It also blocks every signal
across the fork and has the child put each handler back to its default before
it unblocks them, so that a handler the embedding process installed never runs
in a child it was not written for.
