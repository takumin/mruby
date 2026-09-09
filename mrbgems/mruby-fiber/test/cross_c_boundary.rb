# Which methods a Fiber.yield written inside a block can cross.
#
# Only methods the core defines are listed here. A gem's test file is compiled
# with the init of that gem and its dependencies alone, so a case for a method
# another gem defines belongs in that gem's own test file, with
# `add_test_dependency "mruby-fiber"` on its spec.
#
# `assert_cross` is in test/assert.rb, together with what :ok and :ng mean.

# --- blocks the core runs without leaving the VM ---------------------------
assert_cross(:ok, 'Array#each')           { [1, 2].each { Fiber.yield } }
assert_cross(:ok, 'Array#map')            { [1, 2].map { Fiber.yield } }
assert_cross(:ok, 'Array#select')         { [1, 2].select { Fiber.yield; true } }
assert_cross(:ok, 'Hash#each')            { {a: 1}.each { Fiber.yield } }
assert_cross(:ok, 'Hash#each_value')      { {a: 1}.each_value { Fiber.yield } }
assert_cross(:ok, 'Hash#any? with a block') { {a: 1}.any? { Fiber.yield; true } }
assert_cross(:ok, 'Integer#times')        { 2.times { Fiber.yield } }
assert_cross(:ok, 'Integer#step')         { 1.step(3, 1) { Fiber.yield } }
assert_cross(:ok, 'Float#step', Object.const_defined?(:Float)) { 1.0.step(3.0, 1.0) { Fiber.yield } }
assert_cross(:ok, 'Range#each')           { (1..2).each { Fiber.yield } }
assert_cross(:ok, 'String#each_line')     { "a\nb".each_line { Fiber.yield } }
assert_cross(:ok, 'String#each_byte')     { 'ab'.each_byte { Fiber.yield } }
assert_cross(:ok, 'Kernel#loop')          { n = 0; loop { Fiber.yield; n += 1; break if n > 1 } }
assert_cross(:ok, 'Module#module_eval')   { Object.module_eval { Fiber.yield } }
assert_cross(:ok, 'Object#instance_eval') { Object.new.instance_eval { Fiber.yield } }

# --- blocks a C method runs by re-entering the VM --------------------------
assert_cross(:ng, 'Array#sort with a block')   { [3, 1, 2].sort { |a, b| Fiber.yield; a <=> b } }
assert_cross(:ng, 'Array#sort! with a block')  { [3, 1, 2].sort! { |a, b| Fiber.yield; a <=> b } }
assert_cross(:ng, 'Array.new with a block')    { Array.new(2) { Fiber.yield; 0 } }
assert_cross(:ng, 'Class.new with a block')    { Class.new { Fiber.yield } }
assert_cross(:ng, 'Module.new with a block')   { Module.new { Fiber.yield } }

# --- blocks a C method passes to the VM instead of re-entering it ----------
assert_cross(:ok, "a Hash's default proc")     { h = Hash.new { Fiber.yield; 1 }; h[:x] }
assert_cross(:ok, 'Hash#default with a proc')  { Hash.new { Fiber.yield; 1 }.default(:x) }
assert_cross(:ok, 'Array#delete with a block') { [1].delete(2) { Fiber.yield } }
assert_cross(:ok, 'Array#index with a block')  { [1, 2].index { Fiber.yield; false } }
assert_cross(:ok, 'Array#rindex with a block') { [1, 2].rindex { Fiber.yield; false } }

# --- protocol methods a C method sends -------------------------------------
class CrossEq
  def ==(other); Fiber.yield; true; end
  def eql?(other); Fiber.yield; true; end
  def hash; Fiber.yield; 1; end
end

class CrossCmp
  include Comparable
  def initialize(v); @v = v; end
  def <=>(other); Fiber.yield; 0; end
end

class CrossStr
  def inspect; Fiber.yield; 'x'; end
  def to_s; Fiber.yield; 'x'; end
end

class CrossAry
  def to_ary; Fiber.yield; [1]; end
end

class CrossMissing
  def method_missing(name, *args); Fiber.yield; 1; end
  def respond_to_missing?(name, priv); Fiber.yield; true; end
end

class CrossConst
  def self.const_missing(name); Fiber.yield; 1; end
end

class CrossProc
  def to_proc; Fiber.yield; ->(x) { x }; end
end

assert_cross(:ok, 'method_missing')             { CrossMissing.new.nosuch }
assert_cross(:ok, 'Comparable#< sending <=>')   { CrossCmp.new(1) < CrossCmp.new(2) }
assert_cross(:ok, 'Array#+ sending to_ary')     { [1] + [CrossAry.new] }

assert_cross(:ng, 'Array#sort sending <=>')     { [CrossCmp.new(2), CrossCmp.new(1)].sort }
assert_cross(:ng, 'Array#index sending ==')     { [CrossEq.new].index(CrossEq.new) }
assert_cross(:ng, 'Array#delete sending ==')    { [CrossEq.new].delete(CrossEq.new) }
assert_cross(:ng, 'Hash#[] sending hash')       { ({CrossEq.new => 1})[CrossEq.new] }
assert_cross(:ng, 'Array#inspect sending inspect') { [CrossStr.new].inspect }
assert_cross(:ng, 'Array#join sending to_s')    { [CrossStr.new].join }
assert_cross(:ng, 'string interpolation sending to_s') { "#{CrossStr.new}" }
assert_cross(:ng, 'respond_to? sending respond_to_missing?') { CrossMissing.new.respond_to?(:nosuch) }
assert_cross(:ng, 'const_missing')              { CrossConst::NoSuch }
assert_cross(:ng, '&obj sending to_proc')       { [1].map(&CrossProc.new) }

# --- the API a conversion is written with ----------------------------------
# Not a method of its own: this is mrb_funcall_tail() driven from the test
# driver. A stage that converts a method with it inherits what is recorded
# here, so the API answers for its own crossing rather than being read off
# the first method that uses it.

class CrossTail
  def run; Fiber.yield; 1; end
end

assert_cross(:ok, 'mrb_funcall_tail')           { FuncallTail.tail(CrossTail.new, :run) }
assert_cross(:ng, 'mrb_funcall_tail falling back') { FuncallTail.from_c(CrossTail.new, :run) }
assert_cross(:ok, 'mrb_block_cont')             { BlockCont.detect([1]) { Fiber.yield; true } }
assert_cross(:ng, 'a block run by re-entering')  { BlockCont.detect_nested([1]) { Fiber.yield; true } }
