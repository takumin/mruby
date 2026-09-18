# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

assert_cross(:ok, 'Object#define_singleton_method') do
  o = Object.new
  o.define_singleton_method(:z) { Fiber.yield }
  o.z
end
