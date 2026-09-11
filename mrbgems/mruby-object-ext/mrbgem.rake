MRuby::Gem::Specification.new('mruby-object-ext') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'extensional methods shared by all objects'
  spec.add_test_dependency 'mruby-fiber', core: 'mruby-fiber'
end
