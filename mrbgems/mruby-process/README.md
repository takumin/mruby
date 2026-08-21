# mruby-process

`Process` module, `Process::Child` and `Process::Status` for mruby.

## Installation

Add the line below to your build configuration.

```ruby
  conf.gem core: 'mruby-process'
```

It is part of the `stdlib-io` gembox, so `default.gembox` and `full-core.gembox`
already include it.

## Implemented methods

| method                     | mruby-process | memo                                     |
| -------------------------- | ------------- | ---------------------------------------- |
| Process.pid                | o             | also `$$`                                |
| Process.ppid               | o             |                                          |
| Process.kill               | o             | no process-group forms yet               |
| Process.spawn              | o             | absent under `MRB_NO_PROCESS_SPAWN`      |
| Process.waitpid            | o             | sets `$?`; also `Process.wait`           |
| Process.child              | o             | the child a pid names, while it is live  |
| Process.detach             | o             | returns nil, not a Thread                |
| Process::WNOHANG           | o             | mruby's own value, not the platform's    |
| Process::WUNTRACED         | o             | mruby's own value, not the platform's    |
| Process::Child#pid         | o             |                                          |
| Process::Child#live?       | o             |                                          |
| Process::Child#wait        | o             | idempotent; sets `$?`                    |
| Process::Child#detach      | o             |                                          |
| Process::Status#pid        | o             |                                          |
| Process::Status#to_i       | o             | also `#to_int`                           |
| Process::Status#exited?    | o             |                                          |
| Process::Status#exitstatus | o             |                                          |
| Process::Status#signaled?  | o             |                                          |
| Process::Status#termsig    | o             |                                          |
| Process::Status#stopped?   | o             |                                          |
| Process::Status#stopsig    | o             |                                          |
| Process::Status#coredump?  | o             |                                          |
| Process::Status#success?   | o             |                                          |
| Process::Status#to_s       | o             |                                          |
| Process::Status#inspect    | o             |                                          |
| Process::Status#==         | o             | against another status or an Integer     |
| Process.fork               |               | inherently non-portable; separate change |
| Process.exec               |               | separate change                          |
| Process.system             |               | spawn plus wait; separate change         |
| Process.exit, .exit!       |               | see mruby-exit                           |
| Process.wait2, .waitall    |               | separate change                          |
| Process.uid, .gid, ...     |               | separate change                          |
| Process.getpgrp, ...       |               | separate change                          |

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
sees what an earlier one did. As in CRuby, `err: :out` is *not* `2>&1`: a bare
`:out` names the parent's descriptor 1, and merging inside the child is
written `err: [:child, :out]`.

`[cmdname, argv0]`, `pgroup`, `umask` and `rlimit_*` are not supported.

## Who owes the wait

Every child this interpreter spawns owes one wait, and exactly one thing owes
it. That thing is a record, not a pid: a pid is a label the platform may hand
to another process once the first has been reaped, so waiting on a bare number
is waiting on whoever holds it now.

```ruby
pid   = Process.spawn("sleep 1")
child = Process.child(pid)   # the record, while the child is still live
child.wait                   # -> Process::Status, and $?
child.wait                   # -> the same status; no second wait happens
```

- `Process.waitpid(pid)` waits on a child this interpreter spawned and has not
  yet accounted for. Any other pid -- one already reaped, or one that was
  never this interpreter's -- raises `Errno::ECHILD` rather than waiting on
  whatever process holds the number now.
- `Process::Child#wait` is idempotent, which is what makes reaching the same
  child twice harmless: `IO.popen`'s stream and an explicit `Process.waitpid`
  can both "reap" it, and only one of them reaches the operating system.
- `Process.detach(pid)` gives up the obligation without waiting. On POSIX the
  child's status slot then stays until the host process exits.
- At `mrb_close`, every child still live gets one non-blocking wait and is
  then let go of. A blocking wait there would let a child that never finishes
  hang the interpreter's close, which is worse than the zombie a detached
  child can leave behind.

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
   mruby-io       mruby-process
       |               |
       v               v
    io_hal         process_hal
       |               |
   +---+---+       +---+---+
 posix   win     posix   win
```

`IO.popen` is where the two meet, and it is a composition rather than a third
implementation: `mruby-io` writes it in `mrblib` as `IO.pipe` plus
`Process.spawn`, resolves the `Process` constant when the method is called,
and calls public methods on it. Nothing links across the gems. A build with
`mruby-io` and without `mruby-process` still builds, and `IO.popen` raises
`NotImplementedError` at the call site.

Process creation and process reaping therefore live in one place. There is one
reaper in the build, and `mruby-io` is no longer it: waiting for a child is
`Process`'s, and `IO#close` reaps through the `Process::Child` its stream was
given.

### The HAL boundary

`include/process_hal.h` declares platform-neutral primitives. The port under
`ports/<name>/` implements them; a gem named `hal-process-<conf>` may supply
them instead, in which case the bundled ports are dropped from the build.

The HAL answers OS-level facts and performs OS-level operations:

- `mrb_hal_process_pid()`, `mrb_hal_process_ppid()` — the native process
  identity, widened to `mrb_int`.
- `mrb_hal_process_context_init()` / `_free()` — the context every child of
  this interpreter is spawned into. Empty on POSIX, where the kernel already
  knows what this process's children are; the live handles on Windows, where
  nothing else does.
- `mrb_hal_process_spawn()` — creates a child from a `mrb_process_spawn_params`
  and hands back an opaque `mrb_hal_process_child`.
- `mrb_hal_process_wait()` — waits on one child, or on every live child when
  the child argument is `NULL`, and reports an `mrb_process_event` carrying an
  already decoded `mrb_process_status`.
- `mrb_hal_process_child_release()` — lets go of a child, once.
- `mrb_hal_process_kill()` — delivers a signal.
- `mrb_hal_process_signal_number()` / `_signal_name()` — map between a bare
  name such as `TERM` and the number this platform gives it.

Wait-one and wait-any are one primitive because they are one system call with
a different argument, and because emulating either from the other cannot be
done honestly: polling live children with a non-blocking wait in a loop is not
a blocking wait, and it burns the CPU while pretending otherwise. The set a
wait draws from is one child or all of them, and nothing in between, because
nothing in between is expressible: POSIX `waitpid()` has no form that takes an
arbitrary subset.

The common sources under `src/` implement everything Ruby promises: the module
and class definitions, argument shapes and conversions, the child record table
and its rules, `Process.waitpid` return semantics, the teardown policy, `$?`
and `$$`, and every `Process::Status` method. `Process.spawn`'s argument
decomposition is in `mrblib/process.rb`, so that no Hash / Array / IO case
analysis accumulates in C: `src/spawn.c` receives flat integer and string
arrays.

No POSIX type or macro — `pid_t`, `WIFEXITED`, `WEXITSTATUS`, `SIGTERM`,
`WNOHANG` — appears above the HAL, and the HAL knows nothing of `$?`, `$$`,
blocks, `Process::Status` or the record table.

### A status is a snapshot

A wait status is decoded by the port that produced it, at the moment it was
produced, and a `Process::Status` keeps the result. It is not a question
re-asked of the platform later, because a status outlives the child it came
from, and by then the pid it was decoded for may belong to someone else.

That is also why there is no way to build one by hand: `Process::Status.new`
and `Process::Child.new` are undefined. A status comes from a wait, and a
child from a spawn.

`Process::Status#to_i` returns the platform value the status was decoded
from. Its layout is the platform's business; nothing above the port reads it.

## Deviations from CRuby

- `Process.kill` does not signal process groups yet. A negative signal number,
  or a name written with a leading `-`, raises `ArgumentError` rather than
  quietly signalling the process instead. `Process.waitpid` likewise raises
  `NotImplementedError` for a pid of 0 or a negative pid other than -1.
- `Process.detach` returns nil. CRuby returns a Thread that does the waiting;
  mruby has no threads.
- `Process::Status` cannot be constructed, and `Process::Status.new(pid, raw)`
  is not available.
- `Process::Status._signame` and `Process::Status._signal_description` are
  internal helpers `Process::Status#to_s` uses to spell a signal number out.
  They are not a general signal API; `Signal` and `Signal.signame` are not
  implemented.
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
