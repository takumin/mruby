##
# ArgumentError ISO Test

def assert_argnum_error(given, expected, &block)
  assert("wrong number of arguments") do
    message = "wrong number of arguments (given #{given}, expected #{expected})"
    assert_raise_with_message(ArgumentError, message, &block)
  end
end

assert('ArgumentError', '15.2.24') do
  e2 = nil
  a = []
  begin
    # this will cause an exception due to the wrong arguments
    a[]
  rescue => e1
    e2 = e1
  end

  assert_equal(Class, ArgumentError.class)
  assert_equal(ArgumentError, e2.class)
end

assert("'wrong number of arguments' from mrb_get_args") do
  assert_argnum_error(0, "1+"){__send__}
  assert_argnum_error(0, 1..2){Object.const_defined?}
  assert_argnum_error(3, 1..2){Object.const_defined?(:A, true, 2)}
  assert_argnum_error(2, 0..1){{}.default(1, 2)}
  assert_argnum_error(1, 2){Object.const_set(:B)}
  assert_argnum_error(3, 2){Object.const_set(:C, 1, 2)}
end

assert("'wrong number of arguments' from packed arguments") do
  # a send of fifteen arguments or more hands them over packed, and the count
  # is read from where they were packed
  assert_argnum_error(14, 1..2){"abc".index(*Array.new(14, 1))}
  assert_argnum_error(15, 1..2){"abc".index(*Array.new(15, 1))}
  assert_argnum_error(20, 1..2){"abc".index(*Array.new(20, 1))}
  assert_argnum_error(15, 0..1){String.new(*Array.new(15, ""))}
  # the count comes before any argument is converted, so a wrong type among
  # the arguments that would be read does not hide it
  assert_argnum_error(15, 1..2){"abc".index(*(["b", "x"] + Array.new(13, 1)))}
end

assert('Call to MRB_ARGS_NONE method') do
  assert_raise(ArgumentError) { nil.__id__ 1 }
  assert_raise(ArgumentError) { nil.__id__ opts: 1 }
end
