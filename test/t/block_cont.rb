##
# mrb_block_cont

assert('mrb_block_cont answers what the nested call answers') do
  a = [1, 2, 3, 4]
  [->(x) { x > 2 }, ->(x) { false }, ->(x) { true }].each do |pred|
    assert_equal BlockCont.detect_nested(a) { |x| pred.call(x) },
                 BlockCont.detect(a) { |x| pred.call(x) }
  end
  assert_nil BlockCont.detect([]) { |x| true }
end

assert('mrb_block_cont hands over an argument list of any length') do
  [[], [1], (1..14).to_a, (1..15).to_a, (1..20).to_a].each do |args|
    assert_equal BlockCont.apply_nested(args) { |*a| a },
                 BlockCont.apply(args) { |*a| a }
  end
  assert_equal [1, 2], BlockCont.apply([1, 2]) { |a, b| [a, b] }
end

assert('mrb_block_cont leaves the caller where it found it') do
  before = BlockCont.depth
  BlockCont.detect([1, 2, 3]) { |x| x == 3 }
  assert_equal before, BlockCont.depth
end

assert('break out of a block that ran through mrb_block_cont') do
  assert_equal :broke, (BlockCont.detect([1, 2, 3]) { |x| break :broke if x == 2; false })
  before = BlockCont.depth
  BlockCont.detect([1, 2, 3]) { |x| break if x == 2; false }
  assert_equal before, BlockCont.depth
end

assert('next out of a block that ran through mrb_block_cont') do
  # `next false` ends that call alone, so the walk goes on to the element
  # after it.
  assert_equal 3, (BlockCont.detect([1, 2, 3]) { |x| next false if x < 3; true })
end

assert('return out of a block that ran through mrb_block_cont') do
  def returns_from_block(a)
    BlockCont.detect(a) { |x| return :returned if x == 2; false }
    :fell_through
  end
  assert_equal :returned, returns_from_block([1, 2, 3])
end

assert('an exception out of a block that ran through mrb_block_cont') do
  assert_raise(ArgumentError) do
    BlockCont.detect([1, 2, 3]) { |x| raise ArgumentError, 'from the block' if x == 2; false }
  end
  before = BlockCont.depth
  begin
    BlockCont.detect([1, 2, 3]) { |x| raise ArgumentError if x == 2; false }
  rescue ArgumentError
  end
  assert_equal before, BlockCont.depth
end
