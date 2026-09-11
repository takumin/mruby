require 'open3'

# `"lit".freeze` and `-"lit"` are compiled to the literal's own answer, which
# stands only while the method each is written as is still the builtin.  A
# program that replaces one has to be sent it instead, and taking the
# replacement away has to leave the literal answering again.  Each case runs in
# a state of its own, which is what a whole process gives it: a replacement
# left standing would be read by every case after it.

def assert_mruby_out(exp, script)
  out, err, stat = Open3.capture3(*(cmd_list("mruby") + ["-e", script]))
  assert_equal "", err
  assert_true stat.success?, "mruby exited with a failure"
  assert_equal exp, out
end

assert('a string literal answers its own freeze') do
  assert_mruby_out "true\ntrue\n", <<~'RUBY'
    p "abc".freeze.frozen?
    p "abc".freeze.equal?("abc".freeze)
  RUBY
end

assert('a redefined String#freeze is sent the literal') do
  # The literal reaches the replacement unfrozen, as it would have had the
  # call been compiled as a send, and stops being answered by the compiler.
  assert_mruby_out "\"redefined:abc\"\nfalse\n", <<~'RUBY'
    class String
      def freeze
        "redefined:#{self}"
      end
    end
    p "abc".freeze
    p "abc".freeze.equal?("abc".freeze)
  RUBY
end

assert('a redefined Object#freeze is sent the literal') do
  # `freeze` is Object's, so what the literal stands in for is whatever the
  # name resolves to from String: a replacement written anywhere above it
  # counts, not only one written on String itself.
  assert_mruby_out "\"redefined:abc\"\nfalse\n", <<~'RUBY'
    class Object
      def freeze
        "redefined:#{self}"
      end
    end
    p "abc".freeze
    p "abc".freeze.equal?("abc".freeze)
  RUBY
end

assert('removing a redefined String#freeze leaves the literal answering') do
  assert_mruby_out "true\n", <<~'RUBY'
    class String
      def freeze
        "redefined:#{self}"
      end
      remove_method :freeze
    end
    p "abc".freeze.equal?("abc".freeze)
  RUBY
end

assert('a frozen literal of an irep that has been freed') do
  # The (irep, pool index) cache in front of the table is keyed by the irep's
  # address, so an address a later irep is given must not be answered with the
  # literal of the one before it.  Each `eval` here makes an irep and drops it.
  assert_mruby_out "[]\ntrue\n", <<~'RUBY'
    res = []
    300.times do |i|
      res << eval("# frozen_string_literal: true\n\"lit#{i}\"")
      GC.start if i % 5 == 0
    end
    p((0...300).reject { |i| res[i] == "lit#{i}" })
    p res.all? { |s| s.frozen? }
  RUBY
end

assert('the same text is one frozen string wherever it is written') do
  assert_mruby_out "true\ntrue\n", <<~'RUBY'
    def m
      "shared".freeze
    end
    p m.equal?("shared".freeze)
    p eval("# frozen_string_literal: true\n'shared'").equal?("shared".freeze)
  RUBY
end

assert('frozen_string_literal and a copy through +@') do
  assert_mruby_out "true\nfalse\ntrue\n", <<~'RUBY'
    p eval("# frozen_string_literal: true\n'abc'").frozen?
    p eval("# frozen_string_literal: true\n+'abc'").frozen?
    p eval("# frozen_string_literal: true\n+'abc'") == "abc"
  RUBY
end

assert('a string literal answers its own -@') do
  assert_mruby_out "true\ntrue\n", <<~'RUBY'
    p (-"abc").frozen?
    p (-"abc").equal?("abc".freeze)
  RUBY
end

assert('a redefined String#-@ is sent the literal') do
  # `remove_method` has nothing to leave standing here, since `-@` is String's
  # own method where `freeze` above is Object's, so this case only asks what
  # the replacement is handed.
  assert_mruby_out "\"uminus:abc\"\nfalse\n", <<~'RUBY'
    class String
      def -@
        "uminus:#{self}"
      end
    end
    p(-"abc")
    p (-"abc").equal?(-"abc")
  RUBY
end

assert('a frozen literal lets go of its string with the code it is written in') do
  # Code holds the strings its literals answer with for as long as it lives
  # and gives them back when it is freed, so code compiled, run once and
  # dropped leaves the table where it was.  Two cycles: the first frees the
  # code, the second the strings nothing holds any longer.
  assert_mruby_out "true\n", <<~'RUBY'
    GC.start; GC.start
    base = GC.stat[:frozen_string_count]
    300.times { |i| eval("'dropped with its code #{i}'.freeze") }
    GC.start; GC.start
    p GC.stat[:frozen_string_count] <= base + 1
  RUBY
end

assert('the table of sites is made again after the code that filled it is freed') do
  # Each irep that answered a literal keeps a bit per pool entry until it is
  # freed, and the last one to give its bits back leaves that table empty too.
  # The code given to `eval` below is dropped as it goes, so what the next
  # `eval` asks is asked of a table made afresh.
  assert_mruby_out "0\ntrue\ntrue\n", <<~'RUBY'
    30.times do |i|
      eval("def m#{i}; 'lit_#{i}'.freeze; end")
      send("m#{i}")
      Object.send(:remove_method, "m#{i}")
    end
    GC.start; GC.start
    p GC.stat[:frozen_string_count]
    eval("def after_it; 'after the tables went'.freeze; end")
    p after_it.equal?(after_it)
    p after_it.equal?('after the tables went'.freeze)
  RUBY
end
