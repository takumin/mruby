MRuby::Build.new do |conf|
  # load specific toolchain settings
  toolchain :gcc

  # include the GEM box
  conf.gembox 'full-core'

  # the call tracer; pulls in MRB_USE_CALL_HOOK for the VM
  conf.gem :core => 'mruby-trace'

  # fibers get their own shadow stack in the tracer; build them so the
  # bintest covers that path
  conf.gem :core => 'mruby-fiber'

  conf.enable_test
  conf.enable_bintest
end
