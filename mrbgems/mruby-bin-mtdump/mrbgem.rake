MRuby::Gem::Specification.new('mruby-bin-mtdump') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'method table dumper for the tracing tools (development only)'
  spec.bins = %w(mruby-mtdump)
end
