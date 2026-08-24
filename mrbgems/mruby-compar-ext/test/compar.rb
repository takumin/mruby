assert("Comparable#clamp") do
  assert_equal(12, 12.clamp(0, 100))
  assert_equal(100, 532.clamp(0, 100))
  assert_equal(0, -3.123.clamp(0, 100))
  assert_equal('d', 'd'.clamp('a', 'f'))
  assert_equal('f', 'z'.clamp('a', 'f'))

  assert_equal(12, 12.clamp(0..100))
  assert_equal(100, 523.clamp(0..100))
  assert_equal(0, -3.123.clamp(0..100))

  assert_equal('d', 'd'.clamp('a'..'f'))
  assert_equal('f', 'z'.clamp('a'..'f'))

  assert_equal(0, -20.clamp(0..))
  assert_equal(100, 523.clamp(..100))

  assert_raise(ArgumentError) {
    100.clamp(0...100)
  }
end

assert("Comparable#clamp refuses a bound it cannot be ordered against") do
  # There is no telling which side of a bound `self` falls on when the two
  # stand in no order, which is what `<=>` answers nil for. The pair is
  # refused the way two bounds that cannot be ordered already are, rather
  # than the nil being read as a number.
  assert_raise(ArgumentError) { 1.clamp('a', 'z') }
  assert_raise(ArgumentError) { 'a'.clamp(1, 2) }
  assert_raise(ArgumentError) { 1.clamp('a'..'z') }
end
