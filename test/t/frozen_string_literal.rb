##
# Frozen string literals
#
# A literal in a file that asked for `# frozen_string_literal: true` compiles
# to OP_LOADL over its pool entry, and the VM answers that entry with one
# string for as long as the irep lives. FrozenLit builds such an irep by hand
# (mrbgems/mruby-test/frozen_str.c), since no compiler in this tree emits one
# yet.

def frozen_lit_skip
  skip "no frozen string literal table in this build" if FrozenLit.capacity == 0
end

assert('frozen string literal, what the literal reads as') do
  site = FrozenLit.site("a literal")

  assert_true site.call.frozen?
  assert_equal "a literal", site.call

  other = FrozenLit.site("a literal")
  assert_equal site.call, other.call
  assert_not_same site.call, other.call
end

assert('frozen string literal, one object per site') do
  frozen_lit_skip
  site = FrozenLit.site("a literal")

  assert_same site.call, site.call
end

assert('frozen string literal, a literal held in read-only data') do
  site = FrozenLit.static_site

  assert_true site.call.frozen?
  assert_equal "a literal the binary already holds", site.call

  frozen_lit_skip
  assert_same site.call, site.call
end

assert('frozen string literal, the table keeps its strings from the collector') do
  frozen_lit_skip
  site = FrozenLit.site("collect me")
  str = site.call

  GC.start
  assert_same str, site.call
  assert_equal "collect me", site.call
end

assert('frozen string literal, an irep takes its entries with it') do
  frozen_lit_skip
  GC.start
  held = FrozenLit.count

  1.times { FrozenLit.site("passing through").call }
  assert_equal held + 1, FrozenLit.count

  GC.start
  assert_equal held, FrozenLit.count
end

assert('frozen string literal, a full table builds a string every time') do
  frozen_lit_skip
  GC.start
  sites = []
  (FrozenLit.capacity + 4).times do |i|
    site = FrozenLit.site("full #{i}")
    site.call
    sites << site
  end
  assert_equal FrozenLit.capacity, FrozenLit.count

  last = sites[-1]
  assert_true last.call.frozen?
  assert_equal "full #{FrozenLit.capacity + 3}", last.call
  assert_not_same last.call, last.call

  sites = nil
  GC.start
  assert_equal 0, FrozenLit.count
end
