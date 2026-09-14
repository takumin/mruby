# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

assert_cross(:ok, 'Enumerator.new with a block') { Enumerator.new { |y| Fiber.yield; y << 1 }.each { } }
