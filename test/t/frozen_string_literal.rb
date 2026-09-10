# frozen_string_literal: true
##
# Literals of a file carrying a `frozen_string_literal: true` comment.
#
# The comment is read per file, so these tests have a file of their own: every
# string literal written below is frozen, and every literal of the same text is
# the same string.

assert('frozen_string_literal freezes the literals of the file') do
  a = 'abc'
  assert_predicate a, :frozen?
  assert_same a, 'abc'
  assert_raise(FrozenError) { a[0] = 'x' }
end

assert('frozen_string_literal freezes adjacent literals') do
  a = 'ab' 'c'
  assert_predicate a, :frozen?
  assert_same a, 'abc'
end

assert('frozen_string_literal leaves an interpolation writable') do
  n = 1
  s = "a#{n}b"
  assert_not_predicate s, :frozen?
  s[0] = 'z'
  assert_equal 'z1b', s
end

assert('frozen_string_literal and a copy of a literal') do
  s = 'abc'.dup
  assert_not_predicate s, :frozen?
  assert_not_same s, 'abc'
  s[0] = 'x'
  assert_equal 'xbc', s
end

assert('frozen_string_literal and freeze on a literal') do
  assert_same 'abc', 'abc'.freeze
end
