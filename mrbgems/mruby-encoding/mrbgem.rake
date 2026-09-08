MRuby::Gem::Specification.new('mruby-encoding') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = "Poorman's Encoding for mruby"
  # The one define the encoding side of the build branches on. Core and the
  # other gems read it to decide whether their strings index by character;
  # nothing else defines it, so carrying this gem is what turns UTF-8 on.
  spec.build.defines << "HAVE_MRUBY_ENCODING_GEM"
  spec.add_test_dependency 'mruby-string-ext'
  # The tests ask String for the methods that change a receiver, so that a
  # method the build carries and the coderange checklist does not name fails
  # the suite rather than going unasked.
  spec.add_test_dependency 'mruby-metaprog'
  # String#bitwise_*! write arbitrary bytes over a receiver, so they belong in
  # that checklist; without the gem the tests would ask a method that is not
  # there and pass on the refusal.
  spec.add_test_dependency 'mruby-string-bitops'
end
