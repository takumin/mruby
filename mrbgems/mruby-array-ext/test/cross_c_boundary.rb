# The crossing-table cases this gem's methods own. The core cases are in
# mruby-fiber's own test file, and `assert_cross` is in test/assert.rb.

class AryCrossEq
  def ==(other); Fiber.yield; true; end
  def eql?(other); Fiber.yield; true; end
  def hash; Fiber.yield; 1; end
end

class AryCrossCmp
  def initialize(v); @v = v; end
  def <=>(other); Fiber.yield; 0; end
end

class AryCrossToAry
  def to_ary; Fiber.yield; [1]; end
end

assert_cross(:ok, 'Array#fill with a block')    { [1, 2].fill { Fiber.yield; 0 } }
assert_cross(:ok, 'Array#bsearch')              { [1, 2, 3].bsearch { Fiber.yield; true } }
assert_cross(:ok, 'Array#uniq with a block')    { [1, 2].uniq { Fiber.yield; 1 } }
assert_cross(:ok, 'Array#find')                 { [1, 2].find { Fiber.yield; true } }
assert_cross(:ok, 'Array#flatten sending to_ary') { [AryCrossToAry.new].flatten }
assert_cross(:ok, 'Array#include? sending ==')  { [AryCrossEq.new].include?(AryCrossEq.new) }

assert_cross(:ok, 'Array#max sending <=>')      { [AryCrossCmp.new(2), AryCrossCmp.new(1)].max }
assert_cross(:ok, 'Array#min sending <=>')      { [AryCrossCmp.new(2), AryCrossCmp.new(1)].min }
assert_cross(:ng, 'Array#uniq sending hash')    { [AryCrossEq.new, AryCrossEq.new].uniq }
assert_cross(:ng, 'Array#- sending hash')       { [AryCrossEq.new] - [AryCrossEq.new] }
