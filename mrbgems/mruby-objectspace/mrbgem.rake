MRuby::Gem::Specification.new('mruby-objectspace') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'ObjectSpace class'
  spec.add_test_dependency 'mruby-fiber', core: 'mruby-fiber'
end
