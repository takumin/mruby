class Data
  # Bridge used by the C constructor when #initialize is overridden.
  # mruby has no C API to invoke a method with keyword arguments, so the
  # member values are forwarded to the (user-defined) #initialize here as
  # keyword arguments, matching CRuby's calling convention.
  private def __init_with_kw(kw)
    initialize(**kw)
  end

  ##
  # call-seq:
  #   data.__eq_from(vals, ovals, i) -> true or false
  #
  # Internal method: the rest of Data#==, from the `i`th member, whose `==`
  # is written in Ruby. The C walk hands over here instead of calling that
  # `==` from C, which would nest a VM under the one this runs in. `vals`
  # and `ovals` hold the members of self and of the other Data. A pair is
  # compared as `mrb_equal()` compares it: equal when they are the same
  # object, and otherwise when `==` says so.
  #
  def __eq_from(vals, ovals, i)
    while i < vals.size
      a = vals[i]
      b = ovals[i]
      return false unless a.equal?(b) || a == b
      i += 1
    end
    true
  end
end
