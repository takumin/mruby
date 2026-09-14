# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

# The block is called from inside the collector's walk over its heap pages,
# whose place is a page and an offset in it. Handing the block to the VM would
# mean carrying that across arbitrary Ruby, which is free to allocate and to
# collect, and so to move the walk out from under itself.
assert_cross(:ng, 'ObjectSpace.each_object') { ObjectSpace.each_object(Class) { Fiber.yield; nil } }
