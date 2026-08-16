MRuby::Build.new do |conf|
  # load specific toolchain settings
  toolchain :gcc

  # include the GEM box
  conf.gembox 'full-core'

  # Dumps this build's method tables for tools/mruby_method_index.rb, which is
  # what turns a Ruby method name into the C function uftrace should record.
  # Development only: it is in no gembox, and belongs to this config alone.
  conf.gem :core => 'mruby-bin-mtdump'

  conf.cc.flags << '-pg'
  conf.linker.flags << '-pg'

  # Turn on `enable_debug` for better debugging
  conf.enable_debug
  conf.enable_test
end
