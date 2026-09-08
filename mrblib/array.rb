##
# Array
#
# ISO 15.2.12
class Array
  ##
  # call-seq:
  #   array.each {|element| ... } -> self
  #   array.each -> Enumerator
  #
  # Calls the given block for each element of `self`
  # and pass the respective element.
  #
  # ISO 15.2.12.5.10
  def each(&block)
    return to_enum(:each) unless block

    idx = 0
    while idx < length
      yield self[idx]
      idx += 1
    end
    self
  end

  ##
  # call-seq:
  #   array.each_index {|index| ... } -> self
  #   array.each_index -> Enumerator
  #
  # Calls the given block for each element of `self`
  # and pass the index of the respective element.
  #
  # ISO 15.2.12.5.11
  def each_index(&block)
    return to_enum(:each_index) unless block

    idx = 0
    while idx < length
      yield idx
      idx += 1
    end
    self
  end

  ##
  # call-seq:
  #   array.collect! {|element| ... } -> self
  #   array.collect! -> new_enumerator
  #
  # Calls the given block for each element of `self`
  # and pass the respective element. Each element will
  # be replaced by the resulting values.
  #
  # ISO 15.2.12.5.7
  def collect!(&block)
    return to_enum(:collect!) unless block
    # An empty array assigns no element below, so nothing else on this path
    # asks whether the receiver may be written to.
    raise FrozenError, "can't modify frozen #{self.class}" if frozen?

    idx = 0
    len = size
    while idx < len
      self[idx] = yield(self[idx])
      idx += 1
    end
    self
  end

  ##
  # call-seq:
  #   array.map! {|element| ... } -> self
  #   array.map! -> new_enumerator
  #
  # Alias for collect!
  #
  # ISO 15.2.12.5.20
  alias map! collect!

  ##
  # call-seq:
  #   array.sort -> new_array
  #   array.sort {|a, b| ... } -> new_array
  #
  # Returns a new Array whose elements are those from `self`, sorted.
  def sort(&block)
    self.dup.sort!(&block)
  end

  ##
  # call-seq:
  #   array.__index_from(obj, i) -> int or nil
  #
  # Internal method: the rest of Array#index, from the element at `i`, whose
  # `==` is written in Ruby. The C walk hands over here instead of calling
  # that `==` from C, which would nest a VM under the one this runs in. An
  # element is compared as `mrb_equal()` compares it: it is equal to `obj`
  # when it is the same object, and otherwise when its `==` says so.
  #
  def __index_from(obj, i)
    while i < size
      e = self[i]
      return i if e.equal?(obj) || e == obj
      i += 1
    end
    nil
  end

  ##
  # call-seq:
  #   array.__rindex_from(obj, i) -> int or nil
  #
  # Internal method: the rest of Array#rindex, from the element at `i`,
  # whose `==` is written in Ruby, as `__index_from` is the rest of #index.
  #
  def __rindex_from(obj, i)
    while i >= 0
      e = self[i]
      return i if e.equal?(obj) || e == obj
      i -= 1
      i = size - 1 if i >= size
    end
    nil
  end

  ##
  # call-seq:
  #   array.__eq_from(other, i) -> true or false
  #
  # Internal method: the rest of Array#==, from the `i`th pair, whose `==`
  # is written in Ruby, as `__index_from` is the rest of #index. `other` is
  # an Array of the same length, and stays the first argument for the
  # recursion check Array#== makes.
  #
  def __eq_from(other, i)
    while i < size
      a = self[i]
      b = other[i]
      return false unless a.equal?(b) || a == b
      i += 1
    end
    true
  end

  ##
  # call-seq:
  #   array.__delete_from(obj, i, last, found) -> object or nil
  #
  # Internal method: the rest of Array#delete, from the element at `i`,
  # whose `==` is written in Ruby, as `__index_from` is the rest of #index.
  # The elements before `i` hold none equal to `obj` any more; `last` is
  # the one deleted last so far, and `found` whether any was.
  #
  def __delete_from(obj, i, last, found, &block)
    j = i
    while i < size
      e = self[i]
      if e.equal?(obj) || e == obj
        last = e
        found = true
      else
        self[j] = e if i != j
        j += 1
      end
      i += 1
    end
    if found
      self[j, size - j] = [] if j < size
      last
    else
      block ? yield(obj) : nil
    end
  end

  ##
  # call-seq:
  #   array.deconstruct -> self
  #
  # Returns self. Used for array pattern matching in case/in expressions.
  #
  def deconstruct
    self
  end

  ##
  # Array is enumerable
  # ISO 15.2.12.3
  include Enumerable
end
