MRuby::Gem::Specification.new('mruby-metaprog') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'Meta-programming features for mruby'
  spec.add_test_dependency 'mruby-fiber', core: 'mruby-fiber'
end
