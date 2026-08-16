class TraceTarget
  attr_accessor :slot

  def leaf(n)
    n * 2
  end

  def middle(n)
    leaf(n) + leaf(n)
  end

  def outer(n)
    middle(n)
  end

  def repeat(n)
    2.times { leaf(n) }
  end
end

def trace_line(folded, needle)
  folded.split("\n").each do |line|
    return line if line.include?(needle)
  end
  nil
end

assert('Trace.record returns folded stacks') do
  t = TraceTarget.new
  out = Trace.record { t.outer(3) }

  assert_kind_of(String, out)
  assert_include(out, 'TraceTarget#leaf')
  # the whole path from the caller down to the leaf, in one line
  assert_not_nil(trace_line(out, 'TraceTarget#outer;TraceTarget#middle;TraceTarget#leaf'))
  # stacks are rooted at the context they ran in
  assert_true(out.split("\n").all? { |l| l[0, 6] == '<main>' })
end

assert('Trace counts every call of a stack') do
  t = TraceTarget.new
  out = Trace.record(:calls) { t.outer(3) }

  line = trace_line(out, 'TraceTarget#middle;TraceTarget#leaf')
  assert_not_nil(line)
  assert_equal('2', line.split(' ').last)   # middle calls leaf twice

  line = trace_line(out, 'TraceTarget#outer;TraceTarget#middle')
  assert_equal('1', line.split(' ').last)
end

assert('Trace sees the attribute accessor fast paths') do
  t = TraceTarget.new
  out = Trace.record(:calls) { t.slot = 1; t.slot }

  assert_include(out, 'TraceTarget#slot=')
  assert_not_nil(trace_line(out, 'TraceTarget#slot 1'))
end

assert('Trace names blocks apart from their method') do
  t = TraceTarget.new
  out = Trace.record { t.repeat(1) }

  assert_include(out, 'Integer#times')
  # the block belongs to #repeat, and says so instead of borrowing its name
  assert_not_nil(trace_line(out, 'TraceTarget#repeat;Integer#times;block in TraceTarget#repeat'))
end

assert('Trace keeps the stack straight across a raise') do
  t = TraceTarget.new
  out = Trace.record(:calls) do
    begin
      raise 'boom'
    rescue RuntimeError
      t.leaf(1)
    end
  end

  # the frame after the rescue is still hung under the block, not under
  # whatever was on the stack when the exception unwound it
  assert_not_nil(trace_line(out, 'TraceTarget#leaf'))
  assert_true(out.split("\n").all? { |l| l[0, 6] == '<main>' })
end

assert('Trace.start / Trace.stop') do
  assert_false(Trace.running?)
  assert_true(Trace.start)
  assert_true(Trace.running?)
  assert_false(Trace.start)
  assert_true(Trace.stop)
  assert_false(Trace.running?)
  assert_false(Trace.stop)
end

assert('Trace.clear drops what was recorded') do
  t = TraceTarget.new
  Trace.record { t.outer(1) }
  assert_operator(Trace.size, :>, 0)
  Trace.clear
  assert_equal(0, Trace.size)
  assert_equal('', Trace.folded)
end

assert('Trace.elapsed covers the recorded window') do
  t = TraceTarget.new
  Trace.record { 100.times { t.outer(1) } }
  assert_operator(Trace.elapsed, :>, 0)
end

assert('Trace.folded rejects an unknown unit') do
  assert_raise(ArgumentError) { Trace.folded(:furlongs) }
end
