assert 'ObjectSpace.memsize_of' do
  slot = ObjectSpace.__memsize_slot

  # immediate literals
  int_size = ObjectSpace.memsize_of 1
  assert_equal int_size, 0, 'int zero'

  sym_size = ObjectSpace.memsize_of :foo
  assert_equal sym_size, 0, 'sym zero'

  assert_equal ObjectSpace.memsize_of(true), int_size
  assert_equal ObjectSpace.memsize_of(false), int_size

  # a number the boxing puts on the heap is its object slot; an immediate
  # one is nothing, and which is which is the boxing's call
  ObjectSpace.__memsize_heap_number.each do |value, immediate|
    assert_equal immediate ? 0 : slot, ObjectSpace.memsize_of(value), "#{value.class} beyond the boxed word"
  end

  assert_not_equal ObjectSpace.memsize_of('a'), 0, 'memsize of str'
  assert_equal slot, ObjectSpace.memsize_of(ObjectSpace.__memsize_nofree_string), 'static storage is not the string\'s to free'

  if __ENCODING__ == "UTF-8"
    assert_not_equal ObjectSpace.memsize_of("こんにちは世界"), 0, 'memsize of utf8 str'
  end

  # class defs
  class_obj_size = ObjectSpace.memsize_of Class
  assert_not_equal class_obj_size, 0, 'Class obj not zero'

  empty_class_def_size = ObjectSpace.memsize_of Class.new
  assert_not_equal empty_class_def_size, 0, 'Class def not zero'

  proc_size = ObjectSpace.memsize_of Proc.new { x = 1; x }
  assert_not_equal proc_size, 0

  class_with_methods = Class.new do
    def foo
      a = 0
      a + 1
    end
    alias bar foo
  end

  m_size = ObjectSpace.memsize_of class_with_methods.instance_method(:foo)
  assert_not_equal m_size, 0, 'method size not zero'

  # an alias proc carries a method id where an irep would be; must not crash
  alias_size = ObjectSpace.memsize_of class_with_methods.instance_method(:bar)
  assert_not_equal alias_size, 0, 'alias size not zero'
  assert_operator alias_size, :<, m_size, 'alias carries no code of its own'
  assert_operator ObjectSpace.memsize_of_all, :>=, alias_size, 'memsize_of_all walks past the alias'

  # ireps built by hand, each with the heap mrb_irep_free() would release
  ObjectSpace.__memsize_ireps.each do |name, proc, owned|
    assert_equal slot + owned, ObjectSpace.memsize_of(proc), "irep: #{name}"
  end

  # collections
  empty_array_size = ObjectSpace.memsize_of []
  assert_not_equal empty_array_size, 0, 'empty array size not zero'
  assert_operator empty_array_size, :<, ObjectSpace.memsize_of(Array.new(16)), 'large array size greater than embed'
  assert_equal slot + 16 * ObjectSpace.__memsize_value_width, ObjectSpace.memsize_of(Array.new(16)), 'a heap buffer holds one mrb_value per slot of capacity'

  # the buffer is sized by capacity; pop keeps it, so a push back into it costs nothing
  grown = Array.new(16)
  grown.pop
  grown_size = ObjectSpace.memsize_of(grown)
  grown << nil
  assert_equal grown_size, ObjectSpace.memsize_of(grown), 'growing within capacity costs nothing'

  # a dup of a long enough array shares one buffer, charged to neither owner
  shared = Array.new(32)
  shared_dup = shared.dup
  assert_equal ObjectSpace.memsize_of(shared), ObjectSpace.memsize_of(shared_dup), 'shared arrays weigh the same'
  assert_operator ObjectSpace.memsize_of(shared), :<, ObjectSpace.memsize_of(Array.new(32)), 'shared buffer not charged'

  # fiber
  empty_fiber_size = ObjectSpace.memsize_of(Fiber.new {})
  assert_not_equal empty_fiber_size, 0, 'empty fiber not zero'

  # Fiber.allocate has no context yet; must not crash
  bare_fiber = Fiber.allocate
  bare_fiber_size = ObjectSpace.memsize_of(bare_fiber)
  assert_not_equal bare_fiber_size, 0, 'uninitialized fiber not zero'
  assert_operator bare_fiber_size, :<, empty_fiber_size, 'uninitialized fiber smaller than initialized'
  assert_operator ObjectSpace.memsize_of_all, :>=, bare_fiber_size, 'memsize_of_all walks past the uninitialized fiber'

  # backtrace: the slot and one location per frame
  backtrace, frames, location = ObjectSpace.__memsize_backtrace
  assert_equal slot + frames * location, ObjectSpace.memsize_of(backtrace), 'a backtrace is its slot and its locations'

  #hash
  assert_not_equal ObjectSpace.memsize_of({}), 0, 'empty hash size not zero'
end

assert 'ObjectSpace.memsize_of_all' do
  foo_class = Class.new do
    def initialize
      @a = 'a'
      @b = 'b'
    end
  end

  foos = Array.new(10) { foo_class.new }
  foo_size = ObjectSpace.memsize_of(foos.first)

  assert_equal ObjectSpace.memsize_of_all(foo_class), foo_size * foos.size, 'Memsize of all instance'
end
