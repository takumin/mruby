# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

class SprintfCrossStr
  def to_s; Fiber.yield; 'x'; end
end

assert_cross(:ng, 'sprintf %s sending to_s') { sprintf('%s', SprintfCrossStr.new) }
