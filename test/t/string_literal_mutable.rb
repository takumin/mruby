# frozen_string_literal: false

##
# String literals in a file that asked for mutable ones
#
# The comment above names the answer either way, and a file carrying no
# comment keeps the literal it always had: a string of its own each time it
# runs. The rest of test/ is the file without a comment; this is the file
# that asks for false, and test/t/string_literal_frozen.rb the one that asks
# for true.

assert('mutable string literal, what a literal reads as') do
  s = "a literal"

  assert_false s.frozen?
  assert_equal "A LITERAL", s.upcase!
  assert_false "a literal".equal?("a literal")
end

assert('mutable string literal, a literal that runs twice') do
  run = []
  2.times { run << "twice" }
  run[0].upcase!

  assert_equal ["TWICE", "twice"], run
end

assert('mutable string literal, what is written out of literals') do
  x = 1

  assert_false(("a" "b").frozen?)
  assert_false((<<~TEXT).frozen?)
    one
  TEXT
  assert_false("a#{x}b".frozen?)
  assert_false(%w[a].first.frozen?)
  assert_false(__FILE__.frozen?)
end
