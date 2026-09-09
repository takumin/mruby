# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

class SprintfCrossStr
  def to_s; Fiber.yield; 'x'; end
end

# The walk over the format is convertible in the shape the array walks took,
# but mrb_str_format() is what mrb_format() builds every message the core
# raises with, and only the method's own frame can be handed over. It is left
# to a stage that can pay for the walk to exist twice.
assert_cross(:ng, 'sprintf %s sending to_s') { sprintf('%s', SprintfCrossStr.new) }
