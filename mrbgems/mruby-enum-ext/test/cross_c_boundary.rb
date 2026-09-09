# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

assert_cross(:ok, 'Enumerable#group_by')     { [1, 2].group_by { Fiber.yield; 1 } }
assert_cross(:ok, 'Array#count with a block') { [1, 2].count { Fiber.yield; true } }
assert_cross(:ok, 'Enumerable#flat_map')     { [1, 2].flat_map { Fiber.yield; [1] } }
