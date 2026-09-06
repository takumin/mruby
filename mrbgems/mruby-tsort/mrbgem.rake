MRuby::Gem::Specification.new('mruby-tsort') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'TSort module (topological sort and strongly connected components)'

  spec.add_dependency 'mruby-enumerator', :core => 'mruby-enumerator'
end
