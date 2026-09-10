# frozen_string_literal: true

##
# String literals in a file that asked for frozen ones
#
# Every literal this file writes is frozen, which is what the comment above
# asks for; the table they are answered from is in
# test/t/frozen_string_literal.rb, and a file that asks for the other answer
# is in test/t/string_literal_mutable.rb.

# A build that leaves the table out reads the comment as every other build
# does. What it has not got is the one object: a literal frozen with nowhere
# to be answered from is a new string on every run of it.
assert('frozen string literal, a build without the table') do
  skip "this build holds a frozen literal" unless FrozenLit.capacity == 0
  run = []
  2.times { run << "twice" }

  assert_true run[0].frozen?
  assert_equal "twice", run[0]
  assert_false run[0].equal?(run[1])
end

assert('frozen string literal, what a literal reads as') do
  s = "a literal"

  assert_true s.frozen?
  assert_equal "a literal", s
  assert_raise(FrozenError) { s.replace("something else") }
end

assert('frozen string literal, one object where a literal runs twice') do
  frozen_lit_skip(2)             # test/t/frozen_string_literal.rb defines it
  run = []
  2.times { run << "twice" }

  assert_true run[0].equal?(run[1])
  assert_true "twice".equal?("twice")
end

assert('frozen string literal, a copy of a literal') do
  s = "a literal"
  copy = s.dup

  assert_false copy.frozen?
  assert_equal "A LITERAL", copy.upcase!
  assert_false String.new(s).frozen?
  assert_equal "a literal", s
end

assert('frozen string literal, an interpolation is a string of its own') do
  x = 1
  s = "a#{x}b"

  assert_false s.frozen?
  assert_equal "a1b", s
  assert_equal "A1B", s.upcase!
  assert_false "#{x}".frozen?
  assert_equal "1", "#{x}"
end

assert('frozen string literal, literals written next to each other') do
  s = "a" "b"

  assert_true s.frozen?
  assert_equal "ab", s
end

assert('frozen string literal, a heredoc') do
  x = 1
  plain = <<~TEXT
    one
    two
  TEXT
  interpolated = <<~TEXT
    one #{x}
  TEXT

  assert_true plain.frozen?
  assert_equal "one\ntwo\n", plain
  assert_false interpolated.frozen?
  assert_equal "one 1\n", interpolated
end

assert('frozen string literal, a word array') do
  x = "b"

  assert_equal [true, true], %w[a b].map { |w| w.frozen? }
  assert_equal [true, false], %W[a #{x}].map { |w| w.frozen? }
  assert_equal ["a", "b"], %W[a #{x}]
end

assert('frozen string literal, a string that is not written as a literal') do
  x = 1

  assert_equal :a1, :"a#{x}"
  assert_equal "1", 1.to_s
  assert_false 1.to_s.frozen?
  assert_true __FILE__.frozen?
end

assert('frozen string literal, where a literal stands for a value') do
  h = { "k" => 1 }

  assert_equal 1, h["k"]
  assert_equal :ok, (case "lit" when "lit" then :ok end)
  assert_true ["a", "b"].include?("a")
end
