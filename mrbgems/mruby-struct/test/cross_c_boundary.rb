# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

assert_cross(:ok, 'Struct#each_pair') { Struct.new(:a).new(1).each_pair { Fiber.yield } }
