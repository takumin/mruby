##
# Frozen string literals, the instruction they turn into
#
# A literal in a file that asked for `# frozen_string_literal: true` compiles
# to OP_LOADL over its pool entry, and the VM answers that entry with a frozen
# string, which is the one string the table holds for its text. What such a
# literal reads as in a file is in test/t/string_literal_frozen.rb; here it is
# the instruction, over ireps FrozenLit assembles by hand
# (mrbgems/mruby-test/frozen_str.c), which is also the only way to reach an
# entry that points at bytes the binary already holds.

assert('frozen string literal, what a pool entry is answered with') do
  site = FrozenLit.site("a literal")

  assert_true site.call.frozen?
  assert_equal "a literal", site.call
  assert_raise(FrozenError) { site.call.replace("something else") }
end

assert('frozen string literal, a literal held in read-only data') do
  site = FrozenLit.static_site

  assert_true site.call.frozen?
  assert_equal "a literal the binary already holds", site.call
  assert_raise(FrozenError) { site.call.replace("something else") }
end

assert('frozen string literal, one string for every site of the same text') do
  site = FrozenLit.site("a literal")
  other = FrozenLit.site("a literal")
  apart = FrozenLit.site("another literal")

  # The text alone decides the answer, so two sites written the same way are
  # answered with the one string the table holds for those bytes.
  assert_same site.call, other.call
  assert_same site.call, site.call
  assert_not_same site.call, apart.call
end
