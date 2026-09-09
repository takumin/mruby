# The cost of a C method that runs Ruby by re-entering the VM.
#
# Each phase but the last calls a method that reaches its block, or the method
# it sends, through mrb_funcall() or mrb_yield(). The nested mrb_vm_exec() that
# starts there leaves a C frame between two Ruby frames: it is what a fiber
# cannot be suspended across, and what the phases below price. The last phase
# is the control. Its block runs on the same mrb_vm_exec() as its caller, so it
# moves only if the plain call path moves.

REPEAT = 10_000

ELEMS = (0...100).to_a

class Cell
  attr_reader :v
  def initialize(v)
    @v = v
  end

  def ==(other)
    Cell === other && other.v == @v
  end

  def <=>(other)
    @v <=> other.v
  end
end

CELLS = (0...50).map { |i| Cell.new(i) }
LAST = Cell.new(49)
MISSING = Cell.new(-1)

# a block a C method runs, once per element
found = 0
REPEAT.times { found += 1 if ELEMS.index { |x| x == 99 } }
raise 'unexpected index' unless found == REPEAT

# the same, where the block also builds the array
built = 0
REPEAT.times { built += Array.new(100) { |i| i }.size }
raise 'unexpected size' unless built == REPEAT * 100

# a comparator a C method sends, O(n log n) times per call
sorted = nil
REPEAT.times { sorted = CELLS.sort { |a, b| b <=> a } }
raise 'unexpected order' unless sorted.first.v == 49

# `==` sent from C, once per element until it matches
REPEAT.times do
  raise 'expected a hit' unless CELLS.include?(LAST)
  raise 'expected a miss' if CELLS.include?(MISSING)
end

# a Hash default proc, which runs on the frame of the method that misses
misses = 0
counted = Hash.new { |h, k| misses += 1; 0 }
REPEAT.times { ELEMS.each { |k| counted[k] } }
raise 'unexpected misses' unless misses == REPEAT * 100

# a block String#gsub runs, once per match
REPEAT.times { 'a-b-c-d-e'.gsub('-') { '+' } }

# the control: a block that never leaves the VM
sum = 0
REPEAT.times { ELEMS.each { |x| sum += x } }
raise 'unexpected sum' unless sum == REPEAT * 4950
