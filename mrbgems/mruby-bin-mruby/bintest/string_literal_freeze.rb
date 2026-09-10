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
  # name resolves to from String -- a replacement written anywhere above it
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

assert('a string literal answers its own -@') do
  assert_mruby_out "true\ntrue\n", <<~'RUBY'
    p (-"abc").frozen?
    p (-"abc").equal?("abc".freeze)
  RUBY
end

assert('a redefined String#-@ is sent the literal') do
  # `remove_method` has nothing to leave standing here -- `-@` is String's own
  # method, where `freeze` above is Object's -- so this case only asks what the
  # replacement is handed.
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
