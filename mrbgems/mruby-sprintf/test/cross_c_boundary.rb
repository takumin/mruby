# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

class SprintfCrossStr
  def to_s; Fiber.yield; 'x'; end
end

# The conversion asked for here is made in the middle of the format walk,
# whose place is a dozen locals -- where it is in the format, where it is in
# the buffer it is still writing, the width and precision it has read for this
# directive, which argument comes next. A resumed method carries an integer
# and its own registers, and the walk that Array#join shares between the
# method and the C API shows what carrying that much costs to write. Hoisting
# the conversions ahead of the walk instead would change what a bad format
# meets first: a `to_s` with something to say for itself would run before the
# error that the format is malformed, rather than after it.
assert_cross(:ng, 'sprintf %s sending to_s') { sprintf('%s', SprintfCrossStr.new) }
