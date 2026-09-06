MRuby::Gem::Specification.new('mruby-hash-ext') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'Hash class extension'
  spec.add_dependency 'mruby-array-ext', core: 'mruby-array-ext'
  spec.add_test_dependency 'mruby-fiber', core: 'mruby-fiber'
end
