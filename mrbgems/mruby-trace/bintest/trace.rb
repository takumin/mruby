require 'open3'
require 'tempfile'

# The gem's own tests run in a bare `mrb_open_core()` state that holds
# nothing but the gem under test, so anything the tracer does with fibers
# or with a real script has to be checked against the built interpreter.
class BinTest_MRubyTrace
  def self.run(src)
    script = Tempfile.new(['trace', '.rb'])
    script.write src
    script.flush
    out, status = Open3.capture2("#{cmd('mruby')} #{script.path}")
    raise "mruby exited with #{status.exitstatus}:\n#{out}" unless status.success?
    out
  end

  def self.fiber?
    run("puts Object.const_defined?(:Fiber)").strip == 'true'
  end
end

assert('mruby-trace: a whole run folds into stacks') do
  out = BinTest_MRubyTrace.run(<<~RUBY)
    class Demo
      def leaf(n); n * 2; end
      def middle(n); leaf(n) + leaf(n); end
      def outer(n); 3.times { middle(n) }; end
      def self.helper; 1; end
    end

    d = Demo.new
    print Trace.record(:calls) { d.outer(2); Demo.helper }
  RUBY

  lines = out.split("\n")
  assert_true lines.all? { |l| l.start_with?('<main>;') }, out

  leaf = lines.find { |l| l.include?('Demo#middle;Demo#leaf') }
  assert_true !leaf.nil?, out
  # 3 rounds of the block, each calling #leaf twice
  assert_equal '6', leaf.split(' ').last

  assert_true lines.any? { |l| l.include?('block in Demo#outer') }, out
  assert_true lines.any? { |l| l.include?('Demo.helper') }, out
end

assert('mruby-trace: fibers are traced under their own root') do
  skip 'Fiber is not built in' unless BinTest_MRubyTrace.fiber?

  out = BinTest_MRubyTrace.run(<<~RUBY)
    class Demo
      def in_fiber; 1 + 1; end
      def on_main; 2 + 2; end
    end

    d = Demo.new
    print Trace.record(:calls) {
      f = Fiber.new { d.in_fiber; Fiber.yield; d.in_fiber }
      f.resume
      d.on_main
      f.resume
    }
  RUBY

  lines = out.split("\n")
  fiber = lines.select { |l| l.start_with?('<fiber>;') }
  main = lines.select { |l| l.start_with?('<main>;') }

  # the two stacks stay apart, each rooted where it ran
  assert_true fiber.any? { |l| l.include?('Demo#in_fiber') }, out
  assert_true main.any? { |l| l.include?('Demo#on_main') }, out
  assert_true main.none? { |l| l.include?('Demo#in_fiber') }, out

  # both resumes land on the same stack
  assert_equal '2', fiber.find { |l| l.include?('Demo#in_fiber') }.split(' ').last
end

assert('mruby-trace: time adds up to the traced window') do
  out = BinTest_MRubyTrace.run(<<~RUBY)
    def busy(n); n.times { |i| i * i }; end

    Trace.start
    busy(2000)
    Trace.stop
    total = Trace.folded.split("\\n").map { |l| l.split(' ').last.to_i }.reduce(0) { |a, b| a + b }
    puts total <= Trace.elapsed
    puts total > 0
  RUBY

  assert_equal ['true', 'true'], out.split("\n")
end
