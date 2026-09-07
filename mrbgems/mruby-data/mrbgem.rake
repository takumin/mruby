MRuby::Gem::Specification.new('mruby-data') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'standard Data class'
  spec.add_test_dependency 'mruby-fiber', core: 'mruby-fiber'
end
