##
# Process ISO Test

module ProcessTestUtil
  # mruby-io is a test-only dependency: mruby-process itself never needs it,
  # but a child process is the only honest way to test waiting on one, and
  # IO.popen is how this build makes children.
  def self.popen?
    Object.const_defined?(:IO) && IO.respond_to?(:popen)
  end

  def self.windows?
    Object.const_defined?(:File) && !File::ALT_SEPARATOR.nil?
  end

  def self.shell(cmd)
    windows? ? "cmd /c #{cmd}" : cmd
  end

  # Start a child running +cmd+ through a shell, or return nil where this
  # build cannot start one.
  def self.spawn(cmd)
    return nil unless popen?
    IO.popen(shell(cmd))
  rescue NotImplementedError
    nil
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

assert('Process::Status.new') do
  # A raw status of 0 means "exited with 0" on every port, which is what lets
  # this be asserted without knowing the platform's status layout.
  st = Process::Status.new(1234, 0)
  assert_equal 1234, st.pid
  assert_equal 0, st.to_i
  assert_true st.exited?
  assert_equal 0, st.exitstatus
  assert_true st.success?
  assert_false st.signaled?
  assert_nil st.termsig
  assert_false st.stopped?
  assert_nil st.stopsig
  assert_false st.coredump?
end

assert('Process::Status#==') do
  st = Process::Status.new(1234, 0)
  assert_operator st, :==, Process::Status.new(1234, 0)
  assert_not_operator st, :==, Process::Status.new(1235, 0)
  assert_operator st, :==, 0
  assert_not_operator st, :==, 1
  assert_not_operator st, :==, "0"
end

assert('Process::Status#to_s, #inspect') do
  st = Process::Status.new(1234, 0)
  assert_equal "pid 1234 exit 0", st.to_s
  assert_equal "#<Process::Status: pid 1234 exit 0>", st.inspect
end

assert('Process.waitpid') do
  io = ProcessTestUtil.spawn("exit 3")
  skip "IO.popen is not available" unless io

  io.read
  pid = io.pid
  assert_equal pid, Process.waitpid(pid)

  # waitpid publishes what it reaped through $?
  assert_kind_of Process::Status, $?
  assert_equal pid, $?.pid
  assert_true $?.exited?
  assert_equal 3, $?.exitstatus
  assert_false $?.success?
  io.close
end

assert('Process.waitpid with Process::WNOHANG') do
  skip "no portable long-running child on this platform" if ProcessTestUtil.windows?
  io = ProcessTestUtil.spawn("sleep 30")
  skip "IO.popen is not available" unless io

  # Nothing has finished, so the wait returns at once with nothing to report.
  assert_nil Process.waitpid(io.pid, Process::WNOHANG)
  assert_nil $?

  Process.kill(:KILL, io.pid)
  assert_equal io.pid, Process.waitpid(io.pid)
  assert_true $?.signaled?
  assert_false $?.exited?
  assert_nil $?.exitstatus
  assert_nil $?.success?
  assert_equal "KILL", Process::Status._signame($?.termsig)
  io.close
end

assert('Process.waitpid with no child to wait for') do
  # A pid reaped once is gone; waiting on it again has nothing to find.
  # Windows waits on a handle rather than a child, so what a second wait
  # reports there is up to the port and not asserted here.
  skip "waiting twice is not a portable error on this platform" if ProcessTestUtil.windows?
  io = ProcessTestUtil.spawn("exit 0")
  skip "IO.popen is not available" unless io

  io.read
  pid = io.pid
  Process.waitpid(pid)
  assert_raise(StandardError) { Process.waitpid(pid) }
  io.close
end

assert('$? after IO.popen') do
  # mruby-io sets $? through Process::Status.new(pid, raw_status) when this
  # gem is present.  Neither gem depends on the other; this is the seam.
  io = ProcessTestUtil.spawn("exit 0")
  skip "IO.popen is not available" unless io

  io.read
  pid = io.pid
  io.close
  assert_kind_of Process::Status, $?
  assert_equal pid, $?.pid
  assert_true $?.success?
end
