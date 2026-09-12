##
# Process::Status Test
#
# ProcessTestUtil comes from process.rb, which the test runner loads first:
# a gem's test files are run in name order.

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

assert('Process::Status does not answer to_int') do
  # mruby has no implicit-conversion protocol, so nothing would ever call it,
  # and CRuby does not have the method either.
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  assert_false ProcessTestUtil.run("exit 0").respond_to?(:to_int)
end

assert('Process::Status#to_s, #inspect') do
  skip ProcessTestUtil.child_reason if ProcessTestUtil.child_reason

  status = ProcessTestUtil.run("exit 0")
  assert_equal "pid #{status.pid} exit 0", status.to_s
  assert_equal "#<Process::Status: pid #{status.pid} exit 0>", status.inspect
end

assert('Process::Status#to_s spells a signal out') do
  # The seam with mruby-signal: a status carries a number, and the name it is
  # written with is the one Signal.signame answers with.
  skip ProcessTestUtil.signal_reason if ProcessTestUtil.signal_reason

  pid = Process.spawn("sleep", "30")
  Process.kill(:KILL, pid)
  Process.waitpid(pid)
  status = $?
  skip "a killed child is not reported as signalled here" unless status.signaled?

  kill = Signal.list["KILL"]
  assert_equal kill, status.termsig
  assert_equal "pid #{pid} SIGKILL (signal #{kill})", status.to_s
  assert_equal "#<Process::Status: pid #{pid} SIGKILL (signal #{kill})>", status.inspect
end
