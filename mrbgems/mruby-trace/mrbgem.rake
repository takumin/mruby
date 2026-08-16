MRuby::Gem::Specification.new('mruby-trace') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'call tracer that records folded stacks'

  # The tracer rides on the call frame hooks of `mrb_state`, which the VM
  # only compiles in when this is defined.
  spec.build.defines << 'MRB_USE_CALL_HOOK'
end
