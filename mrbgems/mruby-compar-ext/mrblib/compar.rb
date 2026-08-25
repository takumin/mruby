module Comparable
  ##
  #  call-seq:
  #    obj.clamp(min, max) ->  obj
  #    obj.clamp(range)    ->  obj
  #
  # In `(min, max)` form, returns _min_ if _obj_
  # `<=>` _min_ is less than zero, _max_ if _obj_
  # `<=>` _max_ is greater than zero, and _obj_
  # otherwise.
  #
  #    12.clamp(0, 100)         #=> 12
  #    523.clamp(0, 100)        #=> 100
  #    -3.123.clamp(0, 100)     #=> 0
  #
  #    'd'.clamp('a', 'f')      #=> 'd'
  #    'z'.clamp('a', 'f')      #=> 'f'
  #
  # In `(range)` form, returns _range.begin_ if _obj_
  # `<=>` _range.begin_ is less than zero, _range.end_
  # if _obj_ `<=>` _range.end_ is greater than zero, and
  # _obj_ otherwise.
  #
  #    12.clamp(0..100)         #=> 12
  #    523.clamp(0..100)        #=> 100
  #    -3.123.clamp(0..100)     #=> 0
  #
  #    'd'.clamp('a'..'f')      #=> 'd'
  #    'z'.clamp('a'..'f')      #=> 'f'
  #
  # If _range.begin_ is `nil`, it is considered smaller than _obj_,
  # and if _range.end_ is `nil`, it is considered greater than
  # _obj_.
  #
  #    -20.clamp(0..)           #=> 0
  #    523.clamp(..100)         #=> 100
  #
  # When _range.end_ is excluded and not `nil`, an exception is
  # raised.
  #
  #     100.clamp(0...100)       # ArgumentError
  #
  def clamp(min, max=nil)

    if max.nil?
      if Range === min
        max = min.end
        if !max.nil? && min.exclude_end?
          raise ArgumentError, "cannot clamp with an exclusive range"
        end
        min = min.begin
      end
    end

    if !min.nil? && !max.nil?
      cmp = min <=> max
      if cmp.nil?
        raise ArgumentError, "comparison of #{min.class} with #{max.class} failed"
      elsif cmp > 0
        raise ArgumentError, "min argument must be smaller than max argument"
      end
    end

    # A bound that stands in no order with `self` answers nil, and there is no
    # telling which side of it `self` falls on. The pair is refused the way the
    # two bounds are refused above, rather than the nil being read as a number,
    # which raised for having no `<` rather than for the comparison it could
    # not make.
    unless min.nil?
      cmp = self <=> min
      unless cmp
        raise ArgumentError, "comparison of #{self.class} with #{min.class} failed"
      end
      return self if cmp == 0
      return min if cmp < 0
    end
    unless max.nil?
      cmp = self <=> max
      unless cmp
        raise ArgumentError, "comparison of #{self.class} with #{max.class} failed"
      end
      return max if cmp > 0
    end
    return self
  end
end
