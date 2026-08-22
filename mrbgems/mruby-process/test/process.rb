##
# Process ISO Test

module ProcessTestUtil
  # These tests run in an interpreter holding this gem and its dependencies
  # and nothing else, which is the point: mruby-process creates its own
  # children and waits for them without mruby-io anywhere in the build.  So
  # nothing here reads a child's output through a pipe; what a redirection
  # did is asked of the child itself, through the status it exits with.
  def self.spawn?
    Process.respond_to?(:spawn)
  end

  # Whether this is Windows.  These tests run without mruby-io, so where
  # File is absent the question cannot be answered and is answered no; what
  # hangs off it is only the Windows-specific coverage.
  def self.windows?
    Object.const_defined?(:File) && !File::ALT_SEPARATOR.nil?
  end

  # Whether a single-String command reaches a POSIX shell.  There is no
  # `File::ALT_SEPARATOR` to consult here, since this gem's tests run without
  # mruby-io, so the shell is asked to do something only a POSIX one does.
  #
  # Running a command is not enough to ask it with: a Windows machine can
  # have a POSIX `test` on its PATH, which `cmd.exe` then finds and runs, so
  # `test 1 = 1` succeeds there while none of the shell syntax these tests
  # rely on works.  Arithmetic expansion reaching the exit status is the
  # shell's own doing, and nothing on the PATH can stand in for it.
  def self.posix_shell?
    return @posix_shell unless @posix_shell.nil?
    @posix_shell = spawn? && run("exit $((6 * 7))").exitstatus == 42
  rescue StandardError
    @posix_shell = false
  end

  # A pid above every platform's maximum, so that kill(2) fails to find a
  # process rather than signalling one.  Linux caps pid_max at 2**22 and the
  # BSDs far lower.
  NO_SUCH_PID = 1 << 30

  # Whether the platform has the signal +name+ stands for.  Process.kill
  # needs a process to name, so it is given one that cannot exist: an
  # unsupported name is refused before the pid is looked at, and a supported
  # one gets as far as not finding it.
  def self.signal?(name)
    Process.kill(name, NO_SUCH_PID)
    true
  rescue ArgumentError
    false
  rescue StandardError
    true
  end

  # Why a test is skipped, or nil when it can run.
  def self.child_reason
    return "this build cannot create processes" unless spawn?
    nil
  end

  # Whether Process.kill turned +pid+ down rather than passing it on.  What a
  # non-positive pid selects is the platform's to say, so its answer cannot be
  # asserted here; that it was asked at all can be.
  def self.kill_refused?(pid)
    Process.kill(0, pid)
    false
  rescue ArgumentError
    true
  rescue StandardError
    false
  end

  # The four clocks Process names.  The ids are 0 to 3, so `clocks.size` is
  # the first number that names none of them.
  def self.clocks
    [Process::CLOCK_REALTIME, Process::CLOCK_MONOTONIC,
     Process::CLOCK_PROCESS_CPUTIME_ID, Process::CLOCK_THREAD_CPUTIME_ID]
  end

  # Whether this platform has the clock +id+ names.  Every constant is
  # defined everywhere, and a port without the clock behind one refuses to
  # read it, so a test that means to exercise a reading asks first.  Asked in
  # seconds, which every build's Integer can carry.
  def self.clock?(id)
    Process.clock_gettime(id, :second)
    true
  rescue Errno::EINVAL
    false
  end

  # Whether a reading of +id+ in +unit+ is one this build can answer with.
  # An Integer of 32 bits carries a clock in seconds and little finer, and a
  # build without bigints has nothing wider to put such a reading in, so a
  # test that reads nanoseconds asks rather than pinning a build's width.
  def self.fits?(id, unit)
    Process.clock_gettime(id, unit)
    true
  rescue RangeError
    false
  end

  # The reading of +sec+ seconds and +nsec+ nanoseconds answered in +unit+,
  # as the decimal the Integer of it spells, or nil where this build's
  # Integer is too narrow to hold it and the conversion says so.  Both the
  # seconds handed in and the answer read back are decimal Strings: the
  # values worth asking about here are the ones a build may have no way to
  # write as a literal.
  def self.convert(sec, nsec, unit)
    ProcessClockTest.convert(sec, nsec, unit).to_s
  rescue RangeError
    nil
  end

  # Whether this build has a Float for the float units to answer in.
  def self.float?
    Object.const_defined?(:Float)
  end

  # Tests that ask the child something through a POSIX shell.
  def self.posix_reason
    return child_reason if child_reason
    return "the shell here is not a POSIX one" unless posix_shell?
    nil
  end

  # Tests that need to stop, continue and kill a child.  Windows has none of
  # that, and the parts of this gem they cover are not what it has.
  def self.signal_reason
    return posix_reason if posix_reason
    unless signal?(:KILL) && signal?(:STOP) && signal?(:CONT)
      return "the platform has no job-control signals"
    end
    nil
  end

  # Run +command+ and return how it finished.
  def self.run(*command)
    Process.waitpid(Process.spawn(*command))
    $?
  end
end

assert('Process.pid') do
  pid = Process.pid
  assert_kind_of Integer, pid
  assert_true pid > 0
  assert_equal pid, Process.pid
  assert_equal pid, $$
end

assert('Process.ppid') do
  ppid = Process.ppid
  assert_kind_of Integer, ppid
  assert_true ppid >= 0
end

assert('Process::WNOHANG, Process::WUNTRACED') do
  # Portable bits of mruby's own, not the host's, so they are simply two
  # distinct flags that can be combined.
  assert_kind_of Integer, Process::WNOHANG
  assert_kind_of Integer, Process::WUNTRACED
  assert_not_equal Process::WNOHANG, Process::WUNTRACED
  assert_equal 0, Process::WNOHANG & Process::WUNTRACED
end

assert('Process.waitpid with a flag it does not define') do
  # A port answers for the bits it is given and says nothing about the rest,
  # so a bit that stands for nothing is refused before it reaches one.  A
  # negative value is refused for the same reason: read as the unsigned value
  # a port takes, it would turn both of the known bits on.
  [4, -1, Process::WNOHANG | Process::WUNTRACED | 4].each do |flags|
    assert_raise(Errno::EINVAL) { Process.waitpid(-1, flags) }
  end
end

assert('Process.waitpid reports the error by itself') do
  # What a SystemCallError message carries after the error is the object the
  # call was working on, the way `File.open` names the path it could not open.
  # A wait has no such object and CRuby names nothing here.  Compared against
  # the text this platform gives that errno rather than against a literal, so
  # the wording itself is not pinned.
  e = assert_raise(Errno::EINVAL) { Process.waitpid(-1, 4) }
  assert_equal SystemCallError.new(e.errno).message, e.message
end

assert('Process.kill with signal 0') do
  # Signal 0 sends nothing; it only asks whether the process can be signalled.
  assert_equal 1, Process.kill(0, Process.pid)
  assert_equal 2, Process.kill(0, Process.pid, Process.pid)
end

assert('Process.kill does not take "EXIT" for signal 0') do
  # "EXIT" names signal 0 only where a handler is being set, which is not
  # something this gem does; as a signal to send, Ruby refuses the name and
  # leaves the number as the portable way to ask for the null signal.
  assert_raise(ArgumentError) { Process.kill("EXIT", Process.pid) }
  assert_raise(ArgumentError) { Process.kill(:EXIT, Process.pid) }
  assert_raise(ArgumentError) { Process.kill("SIGEXIT", Process.pid) }
end

assert('Process.kill with no process to signal') do
  # A signal on its own names nothing to send it to, which CRuby reports as
  # a missing argument rather than as a call that signalled nobody.
  assert_raise(ArgumentError) { Process.kill(0) }
  assert_raise(ArgumentError) { Process.kill(:TERM) }
end

assert('Process.kill with an unknown signal name') do
  # The name is reported with the "SIG" prefix put back on, whether or not it
  # was written with one, which is how Ruby reports it.
  assert_raise_with_message(ArgumentError, "unsupported signal 'SIGNO_SUCH_SIGNAL'") do
    Process.kill("NO_SUCH_SIGNAL", Process.pid)
  end
  assert_raise_with_message(ArgumentError, "unsupported signal 'SIGNO_SUCH_SIGNAL'") do
    Process.kill(:NO_SUCH_SIGNAL, Process.pid)
  end
  assert_raise_with_message(ArgumentError, "unsupported signal 'SIGNO_SUCH'") do
    Process.kill("SIGNO_SUCH", Process.pid)
  end
end

assert('Process.kill with a signal name too long for any signal') do
  # A name past the lookup buffer's width is still an unsupported signal, not
  # a different kind of error; the name is reported in full rather than
  # replaced by a generic message.
  long_name = "A" * 40
  assert_raise_with_message(ArgumentError, "unsupported signal 'SIG#{long_name}'") do
    Process.kill(long_name, Process.pid)
  end
  assert_raise_with_message(ArgumentError, "unsupported signal 'SIG#{long_name}'") do
    Process.kill(long_name.to_sym, Process.pid)
  end
end

assert('Process.kill with a name that is nothing but the prefix') do
  # "SIG" loses the prefix like any longer name and leaves nothing behind, and
  # a name that was empty to begin with reaches the same place.  Neither is an
  # error of its own; both are reported as the signal that "SIG" alone names,
  # which is none.
  ["SIG", ""].each do |name|
    assert_raise_with_message(ArgumentError, "unsupported signal 'SIG'") do
      Process.kill(name, Process.pid)
    end
    assert_raise_with_message(ArgumentError, "unsupported signal 'SIG'") do
      Process.kill(name.to_sym, Process.pid)
    end
  end
end

assert('Process.kill with a signal of no signal type') do
  # What cannot be a signal at all is refused by its class, which Ruby
  # reports as ArgumentError: the call is not converting the argument, it is
  # naming the kinds it takes.
  # Under MRB_NO_FLOAT the literal below is Integer 0, which is a signal
  # number `kill` takes rather than a class it refuses.
  if Object.const_defined?(:Float)
    assert_raise_with_message(ArgumentError, "bad signal type Float") do
      Process.kill(15.0, Process.pid)
    end
  end
  assert_raise_with_message(ArgumentError, "bad signal type NilClass") do
    Process.kill(nil, Process.pid)
  end
  assert_raise_with_message(ArgumentError, "bad signal type Array") do
    Process.kill([], Process.pid)
  end

  # A big integer is an Integer, but not the Integer the signal branch reads,
  # so it is refused the same way, as CRuby refuses it.  Worked out rather
  # than written down: a literal this wide would drop the whole file from a
  # build that cannot parse it.
  huge = ((2**35) * (2**35) rescue nil)
  if huge
    assert_raise_with_message(ArgumentError, "bad signal type Integer") do
      Process.kill(huge, Process.pid)
    end
  end
end

assert('Process.kill with a signal name holding a NUL') do
  # The name reaches the port as a C string, so a NUL in it would name the
  # part before it.  "TERM\0suffix" must not be a way to spell TERM.
  assert_raise(ArgumentError) { Process.kill("TERM\0suffix", Process.pid) }
  assert_raise(ArgumentError) { Process.kill(:"TERM\0suffix", Process.pid) }
end

assert('Process.kill rejects the process-group signal forms') do
  # Naming a process group through the signal is out of this gem's scope for
  # now, and saying so beats signalling the process instead.
  assert_raise(ArgumentError) { Process.kill(-15, Process.pid) }
  assert_raise(ArgumentError) { Process.kill("-TERM", Process.pid) }
end

assert('Process.kill passes the pid selectors on') do
  # A pid selects processes the way kill(2) reads it, and reading it is the
  # platform's job: POSIX takes 0 for the caller's process group, -1 for every
  # process the caller may signal, and a number below -1 for the group whose
  # ID is -pid, while Windows has no such selectors and answers ESRCH.  Both
  # are answers rather than refusals, and it is the refusal that is pinned
  # here, since what the selectors reach depends on the host: whether the test
  # runner leads its own process group, and whether there is any other process
  # it may signal.  Signal 0 throughout, so nothing is signalled.
  assert_false ProcessTestUtil.kill_refused?(0), "a pid of 0"
  assert_false ProcessTestUtil.kill_refused?(-1), "a pid of -1"
  # Not -Process.pid: where this runs as process 1, that is -1 again and the
  # third selector would never be asked about.
  assert_false ProcessTestUtil.kill_refused?(-(Process.pid + 1)), "a pid below -1"

  # The caller's own process group is one the caller is always in, so where
  # the platform reads the selectors as POSIX does, this one selects.  These
  # tests no longer have mruby-io to ask for File::ALT_SEPARATOR, so a POSIX
  # host is recognised the way the rest of this file recognises one.
  assert_equal 1, Process.kill(0, 0) unless ProcessTestUtil.posix_reason
end

assert('a pid or a signal number too large for the platform') do
  # What is wrong with these is their size, so RangeError is the answer, not
  # the errno a port would have to borrow to report one.
  #
  # A build whose own Integer cannot hold this and has no big integer to
  # promote it to raises while working the value out, so the value is asked
  # for rather than assumed.  `is_a?(Integer)` would not do: a big integer is
  # an Integer, so a build that has them answers yes and says nothing about
  # the width in question.
  big = (2**31 rescue nil)
  skip "this build cannot name a number wider than a pid" unless big

  assert_raise(RangeError) { Process.kill(0, big) }
  assert_raise(RangeError) { Process.waitpid(big) }

  # As a signal, its size is only what is wrong with it where the build's own
  # Integer carries it: a build that promoted it to a big integer refuses it
  # as no signal type instead, tested above.  The builds are told apart by
  # identity, which every Integer has and no big integer object does.
  if big.equal?(2**31)
    assert_raise(RangeError) { Process.kill(big, Process.pid) }
  end
end

assert('Process.spawn') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  pid = Process.spawn("exit 3")
  assert_kind_of Integer, pid
  assert_true pid > 0
  assert_equal pid, Process.waitpid(pid)

  # waitpid publishes what it reaped through $?
  assert_kind_of Process::Status, $?
  assert_true $?.frozen?
  assert_equal pid, $?.pid
  assert_true $?.exited?
  assert_equal 3, $?.exitstatus
  assert_false $?.success?
  assert_false $?.signaled?
  assert_nil $?.termsig
  assert_false $?.stopped?
  assert_nil $?.stopsig
  assert_false $?.coredump?
end

assert('Process.spawn without a shell') do
  skip ProcessTestUtil.posix_reason if ProcessTestUtil.posix_reason

  # Two or more arguments are the command and its arguments, so nothing
  # expands `*` on the way.
  assert_equal 0, ProcessTestUtil.run("sh", "-c", "exit 0").exitstatus
  assert_equal 4, ProcessTestUtil.run("sh", "-c", "exit 4").exitstatus
end

assert('Process.spawn with a command that does not exist') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  # The failure to execute is reported to the caller rather than left as an
  # exit status to be guessed at.
  assert_raise(StandardError) { Process.spawn("mruby-no-such-command", "arg") }
end

assert('Process.spawn with a redirection') do
  skip ProcessTestUtil.posix_reason if ProcessTestUtil.posix_reason

  # What a redirection did is asked of the child: with its standard output
  # closed there is nowhere for `echo` to write, and it says so by failing.
  assert_true ProcessTestUtil.run("echo hello").success?
  assert_false ProcessTestUtil.run("echo hello", out: :close).success?

  # `err: [:child, :out]` is 2>&1: the child's own descriptor 1, as the table
  # has left it by then, and not the parent's.  So the order of the table is
  # the order it is written in: naming err first copies the descriptor 1
  # still has, and closing 1 afterwards does not take that copy back.
  assert_true ProcessTestUtil.run("echo oops 1>&2").success?
  assert_true ProcessTestUtil.run("echo oops 1>&2", err: [:child, :out], out: :close).success?

  # The other way round asks for a copy of a descriptor that is closed by the
  # time it is asked for, and the child reports that rather than running.
  assert_raise(StandardError) do
    ProcessTestUtil.run("echo oops 1>&2", out: :close, err: [:child, :out])
  end

  # A descriptor of the parent's, named as a number rather than as an IO.
  assert_true ProcessTestUtil.run("echo hello", out: 2).success?
end

assert('Process.spawn with an environment') do
  skip ProcessTestUtil.posix_reason if ProcessTestUtil.posix_reason

  assert_true ProcessTestUtil.run({"MRUBY_SPAWN_TEST" => "bar"},
                                  'test "$MRUBY_SPAWN_TEST" = bar').success?
  # A nil value removes a variable rather than setting it.
  assert_true ProcessTestUtil.run({"MRUBY_SPAWN_TEST" => nil},
                                  'test -z "$MRUBY_SPAWN_TEST"').success?
  # An empty environment still takes the deltas: they are what to put in it,
  # not what to change about the parent's.
  assert_true ProcessTestUtil.run({"MRUBY_SPAWN_TEST" => "bar"},
                                  'test "$MRUBY_SPAWN_TEST" = bar',
                                  unsetenv_others: true).success?
end

assert('Process.spawn with chdir') do
  skip ProcessTestUtil.posix_reason if ProcessTestUtil.posix_reason

  assert_true ProcessTestUtil.run('test "`pwd`" = /', chdir: "/").success?
end

assert('Process.waitpid with Process::WNOHANG') do
  skip ProcessTestUtil.signal_reason if ProcessTestUtil.signal_reason

  # The argv form, so that no shell stands between this and the process it
  # signals: a single String is run under /bin/sh, and a shell that forks the
  # command rather than replacing itself with it would be what this kills.
  pid = Process.spawn("sleep", "30")
  # Nothing has finished, so the wait returns at once with nothing to report.
  assert_nil Process.waitpid(pid, Process::WNOHANG)
  assert_nil $?

  Process.kill(:KILL, pid)
  assert_equal pid, Process.waitpid(pid)
  assert_true $?.signaled?
  assert_false $?.exited?
  assert_nil $?.exitstatus
  assert_nil $?.success?
  assert_equal "KILL", Signal.signame($?.termsig)
end

assert('Process.waitpid with Process::WUNTRACED') do
  skip ProcessTestUtil.signal_reason if ProcessTestUtil.signal_reason

  pid = Process.spawn("sleep", "30")
  Process.kill(:STOP, pid)
  assert_equal pid, Process.waitpid(pid, Process::WUNTRACED)
  assert_true $?.stopped?
  assert_false $?.exited?
  assert_equal "STOP", Signal.signame($?.stopsig)

  # A stop is news about the child, not the end of it: it is still this
  # interpreter's to wait for, so a wait that reports nothing is nil rather
  # than Errno::ECHILD.
  assert_nil Process.waitpid(pid, Process::WNOHANG)
  Process.kill(:CONT, pid)
  Process.kill(:KILL, pid)
  assert_equal pid, Process.waitpid(pid)
  assert_true $?.signaled?
end

assert('Process.waitpid with no child to wait for') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  # A pid reaped once is gone; waiting on it again has nothing to find, and
  # the number may by then belong to someone else's process.
  pid = Process.spawn("exit 0")
  Process.waitpid(pid)
  assert_raise(Errno::ECHILD) { Process.waitpid(pid) }
  assert_raise(Errno::ECHILD) { Process.waitpid(pid + 1000000) }
end

assert('Process.waitpid by number accounts for a child of this interpreter') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  # Waiting by number is the platform's own selector, but a number this
  # interpreter spawned is also a record it holds, and the wait goes through
  # that record so that the two cannot come apart.
  pid = Process.spawn("exit 7")
  assert_equal pid, Process.waitpid(pid)
  assert_equal 7, $?.exitstatus

  # ... and the record went with the wait, so nothing here still owes a reap
  # for that number and the platform is not asked about it again.
  assert_raise(Errno::ECHILD) { Process.waitpid(pid) }
end

assert('Process.waitpid for a process this one did not spawn') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  # A pid reaches the platform as written, and what it answers for is the
  # children the running process has.  Its own parent is not one of them, so
  # this is ECHILD wherever a parent can be named at all.
  ppid = begin
           Process.ppid
         rescue StandardError
           nil
         end
  skip "this platform cannot name a parent process" unless ppid

  assert_raise(Errno::ECHILD) { Process.waitpid(ppid) }
end

assert('Process.waitpid over a process group') do
  skip ProcessTestUtil.posix_reason if ProcessTestUtil.posix_reason

  # A child inherits this process's group, so 0, the caller's own group,
  # reaches it.  A pid below -1 is the same call with the group named
  # outright, which would need a getpgrp this gem does not have.
  pid = Process.spawn("exit 0")
  assert_equal pid, Process.waitpid(0)
  assert_true $?.exited?
  assert_equal 0, $?.exitstatus

  # The record the wait landed on is found from the pid it reported, so a
  # group wait accounts for the child exactly as a wait that named it would:
  # nothing is left owing a reap for it.
  assert_raise(Errno::ECHILD) { Process.waitpid(pid) }
end

assert('Process.waitpid over a process group with no child in it') do
  skip ProcessTestUtil.posix_reason if ProcessTestUtil.posix_reason

  # A group holds whatever it holds; what a wait draws from it is the running
  # process's children, so a group with none of them in it has nothing to
  # report even where the group itself is populated.
  assert_raise(Errno::ECHILD) { Process.waitpid(0) }
end

assert('Process.kill reports the error by itself') do
  # Signalling names no object either, so its message is the error alone.
  # The pid is one no platform hands out, so the failure is reached without
  # naming a process that might belong to someone else.
  e = assert_raise(Errno::ESRCH) { Process.kill(0, ProcessTestUtil::NO_SUCH_PID) }
  assert_equal SystemCallError.new(e.errno).message, e.message
end

assert('Process.wait') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  # The same wait under Ruby's other name for it.
  pid = Process.spawn("exit 4")
  assert_equal pid, Process.wait(pid)
  assert_kind_of Process::Status, $?
  assert_equal pid, $?.pid
  assert_equal 4, $?.exitstatus

  pid = Process.spawn("exit 0")
  # -1 is "whichever child finishes first", which need not be the one just
  # started: another test may have left one behind.
  reaped = Process.wait
  reaped = Process.wait while reaped != pid
  assert_equal pid, reaped
  assert_true $?.success?
end

assert('Process.waitpid2, Process.wait2') do
  # The pid and the status of one wait, returned together.  $? is set to the
  # same status, so the pair is a second way to reach it and not a second
  # wait: asking twice would find nothing to wait for the second time.
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  pid = Process.spawn("exit 4")
  result = Process.waitpid2(pid)
  assert_kind_of Array, result
  assert_equal 2, result.size
  assert_equal pid, result[0]
  assert_kind_of Process::Status, result[1]
  assert_equal 4, result[1].exitstatus
  assert_equal pid, result[1].pid
  # The same object, not merely a status that reads the same: one wait
  # happened, and both ways of reaching it reach that one.
  assert_true result[1].equal?($?)
end

assert('Process.wait2 with Process::WNOHANG') do
  skip ProcessTestUtil.signal_reason if ProcessTestUtil.signal_reason

  # The argv form, so that the pid this holds is the one that sleeps; see
  # Process.waitpid with Process::WNOHANG above.
  pid = Process.spawn("sleep", "30")
  # Nothing has finished, so there is no pair to hand back.
  assert_nil Process.wait2(pid, Process::WNOHANG)
  assert_nil $?

  Process.kill(:KILL, pid)
  reaped, status = Process.wait2(pid)
  assert_equal pid, reaped
  assert_true status.signaled?
  assert_nil status.exitstatus
  assert_equal "KILL", Signal.signame(status.termsig)
end

assert('Process::Status.new') do
  # A status stands for a wait this interpreter performed, and cannot be
  # conjured out of numbers.
  assert_raise(NoMethodError) { Process::Status.new(1234, 0) }
end

assert('Process::Status#==') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  status = ProcessTestUtil.run("exit 0")
  assert_operator status, :==, status
  assert_operator status, :==, status.to_i
  assert_not_operator status, :==, status.to_i + 1
  assert_not_operator status, :==, "0"
  assert_not_operator status, :==, ProcessTestUtil.run("exit 1")

  # The raw status alone decides, so two children that left the same way are
  # equal although no two live children share a pid.
  other = ProcessTestUtil.run("exit 0")
  assert_not_equal status.pid, other.pid
  assert_operator status, :==, other
end

assert('Process::Status#to_s, #inspect') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  status = ProcessTestUtil.run("exit 0")
  assert_equal "pid #{status.pid} exit 0", status.to_s
  assert_equal "#<Process::Status: pid #{status.pid} exit 0>", status.inspect
end


assert('Process::CLOCK_REALTIME and the rest') do
  # Four distinct ids, all defined on every platform, so that a program
  # naming one is naming the same clock wherever it runs.
  clocks = ProcessTestUtil.clocks
  seen = {}
  clocks.each do |id|
    assert_kind_of Integer, id
    seen[id] = true
  end
  assert_equal clocks.size, seen.size
end

assert('Process.clock_gettime') do
  ProcessTestUtil.clocks.each do |id|
    next unless ProcessTestUtil.clock?(id)

    if ProcessTestUtil.float?
      assert_kind_of Float, Process.clock_gettime(id)
    end
    assert_kind_of Integer, Process.clock_gettime(id, :second)
    if ProcessTestUtil.fits?(id, :nanosecond)
      assert_kind_of Integer, Process.clock_gettime(id, :nanosecond)
    end
  end
end

assert('Process.clock_gettime with CLOCK_MONOTONIC') do
  # What a monotonic clock promises is that a later reading is not a smaller
  # one.  Where its origin is, and so what any single reading says, is the
  # platform's to choose and nothing to assert on.
  skip "no monotonic clock on this platform" unless ProcessTestUtil.clock?(Process::CLOCK_MONOTONIC)
  skip "no Integer for a reading in nanoseconds" unless ProcessTestUtil.fits?(Process::CLOCK_MONOTONIC, :nanosecond)

  first = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)
  second = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)
  assert_operator second, :>=, first
end

assert('Process.clock_gettime in each unit') do
  # The units are scalings of one reading, so a later reading in a smaller
  # unit is worth at least what an earlier one in a bigger unit was, and the
  # two are no further apart than the moment between them.  Read from the
  # monotonic clock, which cannot step between the two readings.
  skip "no monotonic clock on this platform" unless ProcessTestUtil.clock?(Process::CLOCK_MONOTONIC)
  skip "no Integer for a reading in nanoseconds" unless ProcessTestUtil.fits?(Process::CLOCK_MONOTONIC, :nanosecond)

  sec = Process.clock_gettime(Process::CLOCK_MONOTONIC, :second)
  msec = Process.clock_gettime(Process::CLOCK_MONOTONIC, :millisecond)
  usec = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
  nsec = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)

  [sec, msec, usec, nsec].each { |v| assert_kind_of Integer, v }
  # A whole unit is what the reading had reached, never the one it was about
  # to reach, so scaling a bigger unit up never overtakes a later reading.
  assert_operator sec * 1000, :<=, msec
  assert_operator msec * 1000, :<=, usec
  assert_operator usec * 1000, :<=, nsec
  # The four were read moments apart (a scheduler pause on a loaded CI runner
  # costs seconds, not the minute asserted here), while a reading the HAL
  # built wrong, the way a swapped field or the wrong clock would, is off by
  # an amount this catches easily.
  assert_operator nsec - sec * 1_000_000_000, :<, 60 * 1_000_000_000
end

assert('Process.clock_gettime in a float unit') do
  skip "this build has no Float" unless ProcessTestUtil.float?
  skip "no monotonic clock on this platform" unless ProcessTestUtil.clock?(Process::CLOCK_MONOTONIC)

  sec = Process.clock_gettime(Process::CLOCK_MONOTONIC, :float_second)
  msec = Process.clock_gettime(Process::CLOCK_MONOTONIC, :float_millisecond)
  usec = Process.clock_gettime(Process::CLOCK_MONOTONIC, :float_microsecond)

  [sec, msec, usec].each { |v| assert_kind_of Float, v }
  # One reading in three scalings, taken in this order moments apart, so each
  # is worth at least the one before it and the three say the same second.
  # Every unit is scaled and rounded on its own, so comparing two of them
  # rounds a second time, and that alone can read a live pair backwards once
  # the clock's magnitude eats into the mantissa: hence the epsilon.  What a
  # rounding costs is a share of the value rounded, not a fixed amount, so
  # the epsilon is a share of the reading's magnitude too, the origin being
  # the platform's to choose.  An absolute epsilon wide enough for a
  # monotonic clock that has been running a while is either far too wide on
  # a `double` or far too narrow on an `MRB_USE_FLOAT32` build, where the
  # two roundings are worth two milliseconds after a day of uptime.  A
  # millionth is many times the pair of them even there, and far short of
  # what an ordering bug would show.  The minute is the same slack against a
  # scheduler pause on a loaded CI runner that the integer test above uses.
  epsilon = sec.abs * 1.0e-6
  assert_operator msec / 1000, :>=, sec - epsilon
  assert_operator usec / 1000000, :>=, msec / 1000 - epsilon
  assert_operator usec / 1000000 - sec, :<, 60
  # :float_second is what a caller who names no unit gets.
  assert_operator Process.clock_gettime(Process::CLOCK_MONOTONIC), :>=, sec - epsilon
end

assert('Process.clock_gettime with a nil unit') do
  # Naming no unit is what nil says, which is what leaving the argument out
  # says: CRuby cannot tell the two apart at all, an omitted unit arriving
  # there as nil, so neither is answered differently from the other here.
  skip "this build has no Float" unless ProcessTestUtil.float?
  skip "no monotonic clock on this platform" unless ProcessTestUtil.clock?(Process::CLOCK_MONOTONIC)

  assert_kind_of Float, Process.clock_gettime(Process::CLOCK_MONOTONIC, nil)
  assert_kind_of Float, Process.clock_getres(Process::CLOCK_MONOTONIC, nil)
end

assert('Process.clock_gettime in a float unit without a Float') do
  # A build without Float cannot answer in one, and the method is not made
  # to disappear over it: the integer units still answer, and asking for a
  # float one is told so where it is asked.
  skip "this build has a Float" if ProcessTestUtil.float?

  assert_kind_of Integer, Process.clock_gettime(Process::CLOCK_REALTIME, :second)
  [:float_second, :float_millisecond, :float_microsecond].each do |unit|
    assert_raise(NotImplementedError) { Process.clock_gettime(Process::CLOCK_REALTIME, unit) }
  end
  # A resolution in hertz is a Float too, so it goes the same way, and it is
  # still not a unit a reading has, which ArgumentError says first.
  assert_raise(NotImplementedError) { Process.clock_getres(Process::CLOCK_REALTIME, :hertz) }
  assert_raise(ArgumentError) { Process.clock_gettime(Process::CLOCK_REALTIME, :hertz) }
  # Including the one a caller gets by not naming a unit at all, which is the
  # one nil asks for as well.
  assert_raise(NotImplementedError) { Process.clock_gettime(Process::CLOCK_REALTIME) }
  assert_raise(NotImplementedError) { Process.clock_gettime(Process::CLOCK_REALTIME, nil) }
  assert_raise(NotImplementedError) { Process.clock_getres(Process::CLOCK_REALTIME, nil) }
end

assert('Process.clock_gettime with a number naming no clock') do
  # An id outside the list is refused before a port sees it, with the errno
  # a platform's own call gives for a clock it does not have: nothing is
  # wrong with the size of the number, it simply names nothing.
  [-1, ProcessTestUtil.clocks.size, 99].each do |id|
    assert_raise(Errno::EINVAL) { Process.clock_gettime(id) }
    assert_raise(Errno::EINVAL) { Process.clock_getres(id) }
  end
end

assert('Process.clock_gettime with a clock named by a Symbol') do
  # A clock can be named as well as numbered, as it can in CRuby, and the
  # name is the constant's, so a program need not depend on the number.
  {
    CLOCK_REALTIME: Process::CLOCK_REALTIME,
    CLOCK_MONOTONIC: Process::CLOCK_MONOTONIC,
    CLOCK_PROCESS_CPUTIME_ID: Process::CLOCK_PROCESS_CPUTIME_ID,
    CLOCK_THREAD_CPUTIME_ID: Process::CLOCK_THREAD_CPUTIME_ID,
  }.each do |name, id|
    next unless ProcessTestUtil.clock?(id)

    assert_kind_of Integer, Process.clock_gettime(name, :second)
    assert_kind_of Integer, Process.clock_getres(name, :nanosecond)
    assert_equal Process.clock_getres(id, :nanosecond),
                 Process.clock_getres(name, :nanosecond)
  end
end

assert('Process.clock_gettime with a clock_id that names nothing') do
  # CRuby knows further names: the clocks only some platforms have, and the
  # ways it emulates one the host lacks.  A port here either has one of the
  # four or says it has not, so those pick nothing out, and are refused the
  # way a number naming no clock is, which is also what CRuby answers for a
  # name it does not know.
  [:NOPE, :CLOCK_MONOTONIC_RAW, :GETTIMEOFDAY_BASED_CLOCK_REALTIME].each do |name|
    assert_raise(Errno::EINVAL) { Process.clock_gettime(name, :second) }
    assert_raise(Errno::EINVAL) { Process.clock_getres(name, :second) }
  end
  # A String is not a name: it is refused for its type, as CRuby refuses it,
  # rather than read for a clock name it might spell.  nil is refused the
  # same way: naming no clock is not the default a nil unit is.
  assert_raise(TypeError) { Process.clock_gettime("CLOCK_MONOTONIC") }
  assert_raise(TypeError) { Process.clock_gettime(nil) }
end

assert('Process.clock_gettime names the clock it failed on') do
  # The failure says which call was made and which clock it was asked for,
  # the way CRuby says it, so a caller who named a clock is shown the name
  # back rather than a number never written.
  begin
    Process.clock_gettime(:NOPE, :second)
    flunk "no error raised"
  rescue Errno::EINVAL => e
    assert_include e.message, "clock_gettime(:NOPE)"
  end

  begin
    Process.clock_getres(99, :second)
    flunk "no error raised"
  rescue Errno::EINVAL => e
    assert_include e.message, "clock_getres(99)"
  end
end

assert('Process.clock_gettime with a unit it does not know') do
  # A unit is a Symbol, as it is in CRuby, which takes nothing else: a String
  # naming the same thing is not one of the units.
  [:minute, :float_nanosecond, "second", 1].each do |unit|
    assert_raise(ArgumentError) { Process.clock_gettime(Process::CLOCK_REALTIME, unit) }
    assert_raise(ArgumentError) { Process.clock_getres(Process::CLOCK_REALTIME, unit) }
  end
  # :hertz is a resolution's unit alone: there is no rate at which a moment
  # happened.  CRuby refuses it for a reading in the same words.
  assert_raise(ArgumentError) { Process.clock_gettime(Process::CLOCK_REALTIME, :hertz) }
end

assert('Process.clock_gettime with a reading this build cannot carry') do
  # A 32-bit Integer holds a wall clock in seconds and not in nanoseconds.
  # What is wrong with such a reading is its size, so it is refused the way
  # an oversized pid is, unless the build has bigints, which are what CRuby
  # answers with here and are wide enough for any of these clocks.
  skip "this build carries a wall clock in nanoseconds" if ProcessTestUtil.fits?(Process::CLOCK_REALTIME, :nanosecond)

  assert_raise(RangeError) { Process.clock_gettime(Process::CLOCK_REALTIME, :nanosecond) }
  # The same reading in seconds is untouched by it.
  assert_kind_of Integer, Process.clock_gettime(Process::CLOCK_REALTIME, :second)
end

assert('Process.clock_gettime at the ends of what a reading fits in') do
  # A reading becomes an Integer without a clock being read, so where that
  # arithmetic ends can be asked about directly.  It has to be: the first of
  # these is a wall clock in nanoseconds in 2262, and the ones below zero are
  # centuries the other way.  The reading is handed over as a port hands one
  # over, in whole seconds and nanoseconds within one, and the answer is read
  # back as a decimal, these being numbers a build's own Integer may have no
  # way to write.
  #
  # Where the build's Integer holds the answer it is that Integer, whether it
  # took a bigint to hold it or not; where it does not, the reading is
  # refused for its size, as an oversized pid is.
  [
    # int64_t's last value, and the nanosecond after it
    ["9223372036", 854775807, :nanosecond, "9223372036854775807"],
    ["9223372036", 854775808, :nanosecond, "9223372036854775808"],
    # and its first, which falls in a second no whole product of seconds
    # lands on, and the nanosecond before it
    ["-9223372037", 145224192, :nanosecond, "-9223372036854775808"],
    ["-9223372037", 145224191, :nanosecond, "-9223372036854775809"],
    # a second int64_t holds, in a unit whose answer it does not: how far a
    # reading reaches is the platform's business and how far an Integer
    # reaches is mruby's, and the two are not the same question
    ["9223372036854775807", 0, :second, "9223372036854775807"],
    ["9223372036854775807", 0, :millisecond, "9223372036854775807000"],
    ["10000000000", 123456789, :nanosecond, "10000000000123456789"],
    # the second int64_t's own first value falls in, asked for in a unit
    # whose whole seconds land either side of it: the product of that second
    # is itself past int64_t, so a reading there is counted up from INT64_MIN
    # rather than multiplied, and the nanoseconds decide whether it lands
    # back inside.  Without that counting the two below would be refused for
    # a size they have.
    ["-9223372036854776", 200000000, :millisecond, "-9223372036854775800"],
    ["-9223372036854776", 999000000, :millisecond, "-9223372036854775001"],
    # and one in the same second that really is past the end
    ["-9223372036854776", 100000000, :millisecond, "-9223372036854775900"],
    # a reading before the epoch, whose nanoseconds count upwards from the
    # second below it, as a port reports every reading
    ["-2", 500000000, :second, "-2"],
    ["-2", 500000000, :millisecond, "-1500"],
    ["-2", 500000000, :nanosecond, "-1500000000"],
  ].each do |sec, nsec, unit, expected|
    if ProcessClockTest.fits?(expected)
      assert_equal expected, ProcessTestUtil.convert(sec, nsec, unit)
      assert_kind_of Integer, ProcessClockTest.convert(sec, nsec, unit)
    else
      assert_nil ProcessTestUtil.convert(sec, nsec, unit)
      assert_raise(RangeError) { ProcessClockTest.convert(sec, nsec, unit) }
    end
  end
end

assert('Process.clock_getres in hertz') do
  # How many times a second the clock can tell apart, which is one over what
  # :float_second says.  Read back against the same resolution in
  # nanoseconds: a hertz for every nanosecond of it is a second's worth,
  # whatever the clock, and the two are computed apart from each other.
  skip "this build has no Float" unless ProcessTestUtil.float?

  ProcessTestUtil.clocks.each do |id|
    next unless ProcessTestUtil.clock?(id)

    hz = Process.clock_getres(id, :hertz)
    res = Process.clock_getres(id, :nanosecond)
    assert_kind_of Float, hz
    assert_operator hz, :>, 0
    assert_operator (hz * res - 1000000000).abs, :<, 1
  end
end

assert('Process.clock_getres') do
  ProcessTestUtil.clocks.each do |id|
    next unless ProcessTestUtil.clock?(id)

    # A clock a port can read is one it answers a granularity for, so
    # nothing is skipped here for a port declining to say.
    res = Process.clock_getres(id, :nanosecond)
    assert_kind_of Integer, res
    # A resolution is never zero, and no clock here is coarser than a whole
    # second, so it is worth at least a nanosecond and at most one second.
    assert_operator res, :>, 0
    assert_operator res, :<=, 1000000000
    # An integer unit truncates, so a resolution finer than one whole unit
    # of it reads as 0; a clock coarser than a second is the only one that
    # reads above it here.
    assert_operator Process.clock_getres(id, :second), :>=, 0
  end
end

assert('Process.clock_getres of a clock read as a FILETIME') do
  # Windows accounts both CPU clocks in FILETIMEs, and a FILETIME is written
  # in 100ns ticks, so that is how finely two of those readings can differ.
  # The wall clock is left out, being read two different ways depending on
  # the Windows; ports/win/process_hal.c pairs each way with its own
  # granularity.
  skip "not on Windows" unless ProcessTestUtil.windows?

  # A tick is 100ns, so the resolution is asked for in nanoseconds; the
  # reading beside it is asked in an integer unit too, the unit being
  # resolved before the HAL is, so that a build without Float does not raise
  # NotImplementedError before the port is reached at all.
  [Process::CLOCK_PROCESS_CPUTIME_ID, Process::CLOCK_THREAD_CPUTIME_ID].each do |id|
    assert_kind_of Integer, Process.clock_gettime(id, :nanosecond)
    assert_equal 100, Process.clock_getres(id, :nanosecond)
  end
end
