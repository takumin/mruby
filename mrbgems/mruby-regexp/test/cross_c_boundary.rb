# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.
#
# Without this gem these three are the mrblib ones and the crossing works. The
# C forms here run the block by re-entering the VM, which is what makes the
# answer a property of the build rather than of the method name.

fiber = Object.const_defined?(:Fiber)

assert_cross(:ng, 'String#gsub with a block', fiber) { 'ab'.gsub('a') { Fiber.yield; 'x' } }
assert_cross(:ng, 'String#sub with a block', fiber)  { 'ab'.sub('a') { Fiber.yield; 'x' } }
assert_cross(:ng, 'String#scan with a block', fiber) { 'ab'.scan(/a/) { Fiber.yield } }

# Regexp#match's block is this gem's own, not a String method reached through
# it, and the block's result is the method's result. It takes the frame.
assert_cross(:ok, 'Regexp#match with a block', fiber) { /a/.match('a') { Fiber.yield } }
