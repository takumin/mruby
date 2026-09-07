begin
  $fiber_test_activity = __FILE__

  assert('Fiber.new') do
    f = Fiber.new{}
    assert_kind_of Fiber, f
  end

  assert('Fiber#resume') do
    f = Fiber.new{|x| x }
    assert_equal 2, f.resume(2)
  end

  assert('Fiber#transfer') do
    ary = []
    f2 = nil
    f1 = Fiber.new{
      ary << f2.transfer(:foo)
      :ok
    }
    f2 = Fiber.new{
      ary << f1.transfer(:baz)
      :ng
    }
    assert_equal(:ok, f1.transfer)
    assert_equal([:baz], ary)
    assert_false f1.alive?
  end

  assert('Fiber#alive?') do
    f = Fiber.new{ Fiber.yield }
    f.resume
    assert_true f.alive?
    f.resume
    assert_false f.alive?
  end

  assert('Fiber#==') do
    root = Fiber.current
    assert_equal root, root
    assert_equal root, Fiber.current
    assert_false root != Fiber.current
    f = Fiber.new {
      assert_false root == Fiber.current
    }
    f.resume
    assert_false f == root
    assert_true f != root
  end

  assert('Fiber.yield') do
    f = Fiber.new{|x| Fiber.yield x }
    assert_equal 3, f.resume(3)
    assert_true f.alive?
  end

  assert('FiberError') do
    assert_equal StandardError, FiberError.superclass
  end

  assert('Fiber iteration') do
    f1 = Fiber.new{
      [1,2,3].each{|x| Fiber.yield(x)}
    }
    f2 = Fiber.new{
      [9,8,7].each{|x| Fiber.yield(x)}
    }
    a = []
    3.times {
      a << f1.resume
      a << f2.resume
    }
    assert_equal [1,9,2,8,3,7], a
  end

  assert('Fiber with splat in the block argument list') {
    assert_equal([1], Fiber.new{|*x|x}.resume(1))
  }

  assert('Fiber raises on resume when dead') do
    assert_raise(FiberError) do
      f = Fiber.new{}
      f.resume
      assert_false f.alive?
      f.resume
    end
  end

  assert('Yield raises when called on root fiber') do
    assert_raise(FiberError) { Fiber.yield }
  end

  assert('Double resume of Fiber') do
    f1 = Fiber.new {}
    f2 = Fiber.new {
      f1.resume
      assert_raise(FiberError) { f2.resume }
      Fiber.yield 0
    }
    assert_equal 0, f2.resume
    f2.resume
    assert_false f1.alive?
    assert_false f2.alive?
  end

  assert('Recursive resume of Fiber') do
    f1, f2 = nil, nil
    f1 = Fiber.new { assert_raise(FiberError) { f2.resume } }
    f2 = Fiber.new {
      f1.resume
      Fiber.yield 0
    }
    f3 = Fiber.new {
      f2.resume
    }
    assert_equal 0, f3.resume
    f2.resume
    assert_false f1.alive?
    assert_false f2.alive?
    assert_false f3.alive?
  end

  assert('Root fiber resume') do
    root = Fiber.current
    assert_raise(FiberError) { root.resume }
    f = Fiber.new {
      assert_raise(FiberError) { root.resume }
    }
    f.resume
    assert_false f.alive?
  end

  assert('Fiber without block') do
    assert_raise(ArgumentError) { Fiber.new }
  end


  assert('Transfer to self.') do
    result = []
    f = Fiber.new { result << :start; f.transfer; result << :end }
    f.transfer
    assert_equal [:start, :end], result

    result = []
    f = Fiber.new { result << :start; f.transfer; result << :end }
    f.resume
    assert_equal [:start, :end], result
  end

  assert('Resume transferred fiber') do
    f = Fiber.new {
      assert_raise(FiberError) { f.resume }
    }
    f.transfer
  end

  assert('Root fiber transfer.') do
    result = nil
    root = Fiber.current
    f = Fiber.new {
      result = :ok
      root.transfer
    }
    f.transfer
    assert_true f.alive?
    assert_equal :ok, result
  end

  assert('Break nested fiber with root fiber transfer') do
    root = Fiber.current

    result = nil
    f2 = nil
    f1 = Fiber.new {
      root.transfer(f2.transfer)
      result = :f1
    }
    f2 = Fiber.new {
      result = :to_root
      root.transfer :from_f2
      result = :f2
    }
    assert_equal :from_f2, f1.transfer
    assert_equal :to_root, result
    assert_equal :f2, f2.transfer
    assert_equal :f2, result
    assert_false f2.alive?
    assert_equal nil, f1.transfer
    assert_equal :f1, f1.transfer
    assert_equal :f1, result
    assert_false f1.alive?
  end

  assert('CRuby Fiber#transfer test.') do
    ary = []
    f2 = nil
    f1 = Fiber.new{
      ary << f2.transfer(:foo)
      :ok
    }
    f2 = Fiber.new{
      ary << f1.transfer(:baz)
      :ng
    }
    assert_equal :ok, f1.transfer
    assert_equal [:baz], ary
  end
ensure
  $fiber_test_activity = nil
end

assert('symbol GC keeps the symbols a suspended fiber holds') do
  # A fiber that is not running is neither the current context nor the root
  # one, so its stack is a root of its own.
  f = Fiber.new do
    s = "fiber_symbol_gc_probe".to_sym
    Fiber.yield
    s.equal?("fiber_symbol_gc_probe".to_sym)
  end
  f.resume
  GC.start
  i = 0
  while i < 6000
    "fiber-symbol-gc-filler-#{i}".to_sym
    i += 1
  end
  GC.start
  assert_true f.resume
end

# The frame a C method hands back with mrb_funcall_k() names the method to
# enter again by the receiver's class and the method's name rather than by an
# address, so the send it made is free to change what that name means.
# `Fiber.define_cont_methods` puts a pair of C methods on a class of the
# test's own: `send_k(target, mid)` hands its frame back to make the send, and
# `resume_k` is what a test redefines `send_k` to.
assert('a C method redefined in Ruby while suspended is not entered again') do
  $fiber_cont_cls = Class.new
  Fiber.define_cont_methods($fiber_cont_cls)
  target = Class.new do
    def redefine
      $fiber_cont_cls.class_eval { def send_k(*args); :a_ruby_method; end }
      :redefined
    end
  end.new
  assert_raise_with_message(RuntimeError, "'send_k' was redefined while suspended") do
    $fiber_cont_cls.new.send_k(target, :redefine)
  end
ensure
  $fiber_cont_cls = nil
end

assert('a C method redefined to another C method while suspended takes over') do
  $fiber_cont_cls = Class.new
  Fiber.define_cont_methods($fiber_cont_cls)
  assert_equal :fresh, $fiber_cont_cls.new.resume_k
  target = Class.new do
    def redefine
      $fiber_cont_cls.class_eval { alias_method :send_k, :resume_k }
      :from_the_send
    end
  end.new
  # the frame is the one `send_k` handed back, so the method that takes its
  # name over is entered with the answer of the send in it
  assert_equal [:taken_over, :from_the_send], $fiber_cont_cls.new.send_k(target, :redefine)
ensure
  $fiber_cont_cls = nil
end

# A C method that hands its frame back with mrb_funcall_k() leaves no C
# activation record under the Ruby method it sends to, so a `Fiber.yield`
# inside that method has no C boundary to cross.
assert('a Ruby == called from Array#index can yield') do
  cls = Class.new do
    def initialize(v); @v = v; end
    attr_reader :v
    def ==(o); Fiber.yield(:asked); o.is_a?(self.class) && o.v == @v; end
  end
  ary = [cls.new(0), cls.new(1), cls.new(2)]
  f = Fiber.new { ary.index(cls.new(2)) }
  asked = 0
  answer = nil
  while f.alive?
    v = f.resume
    v == :asked ? asked += 1 : answer = v
  end
  assert_equal 3, asked
  assert_equal 2, answer
end

assert('a Ruby == called from Range#== can yield') do
  cls = Class.new do
    def initialize(v); @v = v; end
    attr_reader :v
    def ==(o); Fiber.yield(:asked); o.is_a?(self.class) && o.v == @v; end
    def <=>(o); v <=> o.v; end
  end
  f = Fiber.new { (cls.new(0)..cls.new(1)) == (cls.new(0)..cls.new(1)) }
  asked = 0
  answer = nil
  while f.alive?
    v = f.resume
    v == :asked ? asked += 1 : answer = v
  end
  assert_equal 2, asked
  assert_true answer
end

assert('a suspended C frame unwinds like any other') do
  log = []
  cls = Class.new do
    def initialize(v); @v = v; end
    def ==(o)
      $fiber_cont_log << :entered
      raise 'from a suspended =='
    ensure
      $fiber_cont_log << :ensured
    end
  end
  $fiber_cont_log = log
  assert_raise_with_message(RuntimeError, 'from a suspended ==') do
    [cls.new(0)].index(cls.new(1))
  end
  assert_equal [:entered, :ensured], log
ensure
  $fiber_cont_log = nil
end
