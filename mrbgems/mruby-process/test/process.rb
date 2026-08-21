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

  # Whether a single-String command reaches a POSIX shell.  There is no
  # `File::ALT_SEPARATOR` to consult here -- this gem's tests run without
  # mruby-io -- so the shell is asked something only a POSIX one answers.
  def self.posix_shell?
    return @posix_shell unless @posix_shell.nil?
    @posix_shell = spawn? && run("test 1 = 1").success?
  rescue StandardError
    @posix_shell = false
  end

  # Whether the platform has the signal +name+ stands for.  Process.kill with
  # no pids resolves the name and sends nothing, which is the cheapest way to
  # ask.
  def self.signal?(name)
    Process.kill(name)
    true
  rescue ArgumentError
    false
  end

  # Why a test is skipped, or nil when it can run.
  def self.child_reason
    return "this build cannot create processes" unless spawn?
    nil
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

assert('Process.kill with signal 0') do
  # Signal 0 sends nothing; it only asks whether the process can be signalled.
  assert_equal 1, Process.kill(0, Process.pid)
  assert_equal 1, Process.kill("EXIT", Process.pid)
  assert_equal 2, Process.kill(0, Process.pid, Process.pid)
  assert_equal 0, Process.kill(0)
end

assert('Process.kill with an unknown signal name') do
  assert_raise(ArgumentError) { Process.kill("NO_SUCH_SIGNAL", Process.pid) }
  assert_raise(ArgumentError) { Process.kill(:NO_SUCH_SIGNAL, Process.pid) }
end

assert('Process.kill rejects the process-group forms') do
  # Signalling a process group is out of this gem's scope for now, and saying
  # so beats signalling the process instead.
  assert_raise(ArgumentError) { Process.kill(-15, Process.pid) }
  assert_raise(ArgumentError) { Process.kill("-TERM", Process.pid) }
end

assert('Process.spawn') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  pid = Process.spawn("exit 3")
  assert_kind_of Integer, pid
  assert_true pid > 0
  assert_equal pid, Process.waitpid(pid)

  # waitpid publishes what it reaped through $?
  assert_kind_of Process::Status, $?
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
  # the order it is written in -- naming err first copies the descriptor 1
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

  pid = Process.spawn("sleep 30")
  # Nothing has finished, so the wait returns at once with nothing to report.
  assert_nil Process.waitpid(pid, Process::WNOHANG)
  assert_nil $?

  Process.kill(:KILL, pid)
  assert_equal pid, Process.waitpid(pid)
  assert_true $?.signaled?
  assert_false $?.exited?
  assert_nil $?.exitstatus
  assert_nil $?.success?
  assert_equal "KILL", Process::Status._signame($?.termsig)
end

assert('Process.waitpid with Process::WUNTRACED') do
  skip ProcessTestUtil.signal_reason if ProcessTestUtil.signal_reason

  pid = Process.spawn("sleep 30")
  Process.kill(:STOP, pid)
  assert_equal pid, Process.waitpid(pid, Process::WUNTRACED)
  assert_true $?.stopped?
  assert_false $?.exited?
  assert_equal "STOP", Process::Status._signame($?.stopsig)

  # A stop is news about the child, not the end of it: it is still this
  # interpreter's to wait for.
  assert_true Process.child(pid).live?
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
  assert_raise(StandardError) { Process.waitpid(pid) }
  assert_raise(StandardError) { Process.waitpid(pid + 1000000) }
end

assert('Process.waitpid rejects the process-group forms') do
  assert_raise(NotImplementedError) { Process.waitpid(0) }
  assert_raise(NotImplementedError) { Process.waitpid(-2) }
end

assert('Process.wait') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  pid = Process.spawn("exit 0")
  # -1 is "whichever child finishes first", which need not be the one just
  # started: another test may have left one behind.
  reaped = Process.wait
  reaped = Process.wait while reaped != pid
  assert_equal pid, reaped
  assert_true $?.success?
end

assert('Process.child') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  pid = Process.spawn("exit 5")
  child = Process.child(pid)
  assert_kind_of Process::Child, child
  assert_equal pid, child.pid
  assert_true child.live?

  status = child.wait
  assert_kind_of Process::Status, status
  assert_equal 5, status.exitstatus
  assert_false child.live?
  assert_equal status, $?

  # Reaching the same child twice is expected and harmless: the second wait
  # answers from what the first one stored and reaches no system call.
  assert_equal 5, child.wait.exitstatus
  assert_equal 5, $?.exitstatus

  # ... and it is gone from the pids that can still be waited on.
  assert_nil Process.child(pid)
end

assert('Process.detach') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  pid = Process.spawn("exit 0")
  assert_nil Process.detach(pid)
  assert_nil Process.child(pid)
  assert_raise(StandardError) { Process.waitpid(pid) }
  assert_nil Process.detach(pid)
end

assert('Process::Child.new, Process::Status.new') do
  # Both stand for something the interpreter did: a child it spawned, and a
  # wait it performed.  Neither can be conjured out of numbers.
  assert_raise(NoMethodError) { Process::Child.new }
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
end

assert('Process::Status#to_s, #inspect') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  status = ProcessTestUtil.run("exit 0")
  assert_equal "pid #{status.pid} exit 0", status.to_s
  assert_equal "#<Process::Status: pid #{status.pid} exit 0>", status.inspect
end
