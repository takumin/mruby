##
# Range
#
# ISO 15.2.14
class Range
  ##
  # Range is enumerable
  #
  # ISO 15.2.14.3
  include Enumerable

  ##
  # Calls the given block for each element of `self`
  # and pass the respective element.
  #
  # ISO 15.2.14.4.4
  def each(&block)
    return to_enum(:each) unless block

    val = self.begin
    last = self.end

    if Integer === val && last.nil?
      i = val
      while true
        yield(i)
        i += 1
      end
      return self
    end

    if String === val && last.nil?
      if val.respond_to? :__upto_endless
        return val.__upto_endless(&block)
      else
        str_each = true
      end
    end

    if Integer === val && Integer === last # integers are special
      lim = last
      lim += 1 unless exclude_end?
      i = val
      while i < lim
        yield(i)
        i += 1
      end
      return self
    end

    if String === val && String === last # strings are special
      if val.respond_to? :upto
        return val.upto(last, exclude_end?, &block)
      else
        str_each = true
      end
    end

    raise TypeError, "can't iterate" unless val.respond_to? :succ

    return self if (val <=> last) > 0

    while (val <=> last) < 0
      yield(val)
      val = val.succ
      if str_each
        break if val.size > last.size
      end
    end

    yield(val) if !exclude_end? && (val <=> last) == 0
    self
  end

  ##
  # call-seq:
  #   rng.__eq_from(other, i) -> true or false
  #
  # Internal method: the rest of Range#==, from the beginning for an `i` of
  # 0 and from the end for 1, the one whose `==` is written in Ruby. The C
  # comparison hands over here instead of calling that `==` from C, which
  # would nest a VM under the one this runs in. A pair of ends is compared
  # as `mrb_equal()` compares it: equal when they are the same object, and
  # otherwise when `==` says so.
  #
  def __eq_from(other, i)
    if i == 0
      a = self.begin
      b = other.begin
      return false unless a.equal?(b) || a == b
    end
    a = self.end
    b = other.end
    return false unless a.equal?(b) || a == b
    exclude_end? == other.exclude_end?
  end

  # redefine #hash 15.3.1.3.15
  def hash
    # Use self.begin/self.end instead of first/last to handle endless/beginless ranges
    h = self.begin.hash ^ self.end.hash
    h += 1 if self.exclude_end?
    h
  end

  ##
  # call-seq:
  #    rng.to_a                   -> array
  #    rng.entries                -> array
  #
  # Returns an array containing the items in the range.
  #
  #   (1..7).to_a  #=> [1, 2, 3, 4, 5, 6, 7]
  #   (1..).to_a   #=> RangeError: cannot convert endless range to an array
  def to_a
    a = __num_to_a
    return a if a
    super
  end
  alias entries to_a
end
