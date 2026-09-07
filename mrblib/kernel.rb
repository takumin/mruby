##
# Kernel
#
# ISO 15.3.1
module Kernel

  # 15.3.1.2.1 Kernel.`
  # provided by Kernel#`
  # 15.3.1.3.3
  def `(s)
    raise NotImplementedError.new("backquotes not implemented")
  end

  ##
  # call-seq:
  #   obj.__eqq_from(other) -> true or false
  #
  # Internal method: the rest of Kernel#===, which is `==` taken for true or
  # false, for a receiver whose `==` is written in Ruby. The C method hands
  # over here instead of calling that `==` from C, which would nest a VM
  # under the one this runs in.
  #
  def __eqq_from(other)
    self == other ? true : false
  end

  ##
  # call-seq:
  #   obj.__cmp_from(other) -> 0 or nil
  #
  # Internal method: the rest of Kernel#<=> the same way: 0 when `==` says
  # the two are equal, nil otherwise.
  #
  def __cmp_from(other)
    self == other ? 0 : nil
  end

  ##
  # 15.3.1.2.3  Kernel.eval
  # 15.3.1.3.12 Kernel#eval
  # NotImplemented by mruby core; use mruby-eval gem

  ##
  # ISO 15.3.1.2.8 Kernel.loop
  # not provided by mruby

  ##
  # Calls the given block repetitively.
  #
  # ISO 15.3.1.3.29
  module_function def loop(&block)
    return to_enum(:loop) unless block

    while true
      yield
    end
  rescue StopIteration => e
    e.result
  end

  def to_enum(*a)
    raise NotImplementedError.new("fiber required for enumerator")
  end
end
