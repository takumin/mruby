# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

assert_cross(:ng, 'ObjectSpace.each_object') { ObjectSpace.each_object(Class) { Fiber.yield; nil } }
