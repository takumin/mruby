MRuby::Gem::Specification.new('mruby-struct') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'standard Struct class'
  spec.add_test_dependency 'mruby-fiber', core: 'mruby-fiber'
end
