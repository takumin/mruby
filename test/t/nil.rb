##
# NilClass ISO Test

assert('NilClass', '15.2.4') do
  assert_equal Class, NilClass.class
end

assert('NilClass', '15.2.4.1') do
  assert_equal NilClass, nil.class
  assert_false NilClass.method_defined? :new
end

assert('NilClass#&', '15.2.4.3.1') do
  assert_false nil.&(true)
  assert_false nil.&(nil)
end

assert('NilClass#^', '15.2.4.3.2') do
  assert_true nil.^(true)
  assert_false nil.^(false)
end

assert('NilClass#|', '15.2.4.3.3') do
  assert_true nil.|(true)
  assert_false nil.|(false)
end

assert('NilClass#nil?', '15.2.4.3.4') do
  assert_true nil.nil?
end

assert('NilClass#to_s', '15.2.4.3.5') do
  assert_equal '', nil.to_s
end

assert('NilClass#=~') do
  assert_nil nil =~ "a"
  assert_nil nil =~ nil
  assert_nil nil =~ Object.new
  assert_true nil.respond_to?(:=~)
  # `Kernel#!~` is `!(self =~ other)`, so it answers now that `=~` does.
  assert_true nil !~ "a"
  assert_raise(ArgumentError) { nil.=~() }
end

assert('safe navigation') do
  assert_nil nil&.size
  assert_equal 0, []&.size
end

assert('NilClass#nil? redefined reaches the redefinition in a conditional') do
  # `if x.nil?` answers from C for nil as for any other receiver, and a
  # redefinition on `NilClass` is honored on the same terms, a literal `nil`
  # included. The results are read before the method is put back because
  # mrblib itself branches on `nil?`.
  NilClass.class_eval do
    alias_method :__nil_p_before_test, :nil?
    def nil?; false; end
  end
  begin
    x = nil
    var = x.nil? ? :then : :else
    lit = nil.nil? ? :then : :else
    str = "a".nil? ? :then : :else
  ensure
    NilClass.class_eval do
      alias_method :nil?, :__nil_p_before_test
      remove_method :__nil_p_before_test if respond_to?(:remove_method, true)
    end
  end
  assert_equal :else, var
  assert_equal :else, lit
  assert_equal :else, str
  x = nil
  assert_equal :then, (x.nil? ? :then : :else)
  assert_equal :then, (nil.nil? ? :then : :else)
end
