##
# mrb_funcall_tail

class FuncallTailTarget
  def none; :none; end
  def one(a); a; end
  def many(*a); a; end
  def boom; raise ArgumentError, 'from the callee'; end
  def method_missing(name, *args); [name, args]; end
end

assert('mrb_funcall_tail answers what the method answers') do
  o = FuncallTailTarget.new
  assert_equal :none, FuncallTail.tail(o, :none)
  assert_equal 7, FuncallTail.tail(o, :one, 7)
  assert_equal [1, 2, 3], FuncallTail.tail(o, :many, 1, 2, 3)
end

assert('mrb_funcall_tail answers what the nested call answers') do
  o = FuncallTailTarget.new
  # A long list is packed into an array rather than written into the frame's
  # registers, so both arms of that decision are here.
  [[], [1], (1..14).to_a, (1..20).to_a].each do |args|
    assert_equal FuncallTail.nested(o, :many, *args), FuncallTail.tail(o, :many, *args)
  end
end

assert('mrb_funcall_tail leaves the caller where it found it') do
  o = FuncallTailTarget.new
  before = FuncallTail.depth
  FuncallTail.tail(o, :none)
  assert_equal before, FuncallTail.depth
end

assert('an exception passes through mrb_funcall_tail') do
  o = FuncallTailTarget.new
  assert_raise(ArgumentError) { FuncallTail.tail(o, :boom) }
  # The frame the raise unwound is not one the caller keeps.
  before = FuncallTail.depth
  begin
    FuncallTail.tail(o, :boom)
  rescue ArgumentError
  end
  assert_equal before, FuncallTail.depth
end

assert('mrb_funcall_tail makes the calls it cannot hand over the ordinary way') do
  o = FuncallTailTarget.new
  # A method the receiver does not have, which goes through method_missing.
  assert_equal [:nosuch, [1]], FuncallTail.tail(o, :nosuch, 1)
  # A C method, which enters no VM either way.
  assert_equal 2, FuncallTail.tail([1, 2], :size)
  # A frame this method's C caller is waiting for.
  assert_equal 7, FuncallTail.from_c(o, :one, 7)
end
