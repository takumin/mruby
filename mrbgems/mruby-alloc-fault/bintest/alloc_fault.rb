require 'open3'

# The gate the ordinary test run keeps on the fault driver: that it counts,
# that it refuses, and that the two cases the sweep cannot reach from Ruby --
# a refusal inside mrb_open(), and one driven from inside the running program
# -- still behave.  The sweep itself (sweep.rb) is the long form of this and
# is not run here.

def alloc_fault(*args)
  out, err, stat = Open3.capture3(*(cmd_list('mruby-alloc-fault') + args))
  [out, err, stat]
end

def report_of(out)
  out.lines.map { |l| l.chomp.split(': ', 2) }.select { |pair| pair.size == 2 }.to_h
end

assert('mruby-alloc-fault counts what a scenario allocates') do
  out, err, stat = alloc_fault('-e', 'a = []; 200.times { |i| a << "s#{i}" }', '--count')
  assert_equal '', err
  assert_true stat.success?
  r = report_of(out)
  assert_equal 'ok', r['outcome']
  assert_equal '0', r['refusals']
  assert_operator r['allocations'].to_i, :>, 0
end

assert('mruby-alloc-fault refuses the allocation it is told to') do
  out, _err, stat = alloc_fault('-e', 'a = []; 200.times { |i| a << "s#{i}" }', '--fail-at', '1')
  assert_true stat.success?
  r = report_of(out)
  assert_equal 'nomem', r['outcome']
  assert_equal 'NoMemoryError', r['exception']
  assert_operator r['refusals'].to_i, :>, 0
end

assert('a single refusal is one the collection can answer') do
  # gc.c collects and asks again when an allocation is refused, so refusing
  # one allocation alone leaves the scenario able to finish.
  out, _err, stat = alloc_fault('-e', 'a = []; 200.times { |i| a << "s#{i}" }', '--fail-once', '1')
  assert_true stat.success?
  assert_include %w[ok nomem], report_of(out)['outcome']
end

assert('the state still works once the refusals stop') do
  # --no-recheck turns off the very check this asserts, so the two runs
  # together say that the check is what passed and not that it was skipped.
  out, _err, stat = alloc_fault('-e', '"x" * 100_000', '--fail-at', '1')
  assert_true stat.success?
  assert_equal 'nomem', report_of(out)['outcome']

  out, _err, stat = alloc_fault('-e', '"x" * 100_000', '--fail-at', '1', '--no-recheck')
  assert_true stat.success?
  assert_equal 'nomem', report_of(out)['outcome']
end

assert('mrb_open() answers a state that can be closed') do
  out, _err, stat = alloc_fault('-c', 'open', '--count')
  assert_true stat.success?
  r = report_of(out)
  assert_equal 'ok', r['outcome']
  assert_operator r['allocations'].to_i, :>, 0
end

assert('a refusal at the first allocation of mrb_open() is survivable') do
  # The first allocation mrb_open() makes is the mrb_state itself, so this is
  # the one case where it answers NULL rather than a half-built state, and
  # mrb_close(NULL) has to be a no-op.
  out, _err, stat = alloc_fault('-c', 'open', '--fail-at', '1')
  assert_true stat.success?
  r = report_of(out)
  assert_equal 'open-failure', r['outcome']
  assert_equal '1', r['refusals']
end

assert('AllocFault drives the refusals from inside the program') do
  out, _err, stat = alloc_fault('-e', <<~RUBY)
    n = AllocFault.count { "x" * 200_000 }
    raise "counted nothing" unless n > 0
    raise "armed outside a block" if AllocFault.armed?
    begin
      AllocFault.fail_at(1) { "y" * 200_000 }
      raise "the refusal did not raise"
    rescue NoMemoryError
    end
    raise "left armed after a raise" if AllocFault.armed?
    raise "the state stopped working" unless ("z" * 100).size == 100
  RUBY
  assert_true stat.success?
  assert_equal 'ok', report_of(out)['outcome']
end

assert('mruby-alloc-fault rejects a command line it cannot run') do
  _out, _err, stat = alloc_fault('--fail-at', '1')
  assert_equal 2, stat.exitstatus

  _out, _err, stat = alloc_fault('-e', '1', '--fail-at', '0')
  assert_equal 2, stat.exitstatus
end
