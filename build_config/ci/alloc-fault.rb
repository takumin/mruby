# The build the allocation-failure sweep runs on: the fault-injecting driver
# of mruby-alloc-fault, built with the address and undefined sanitizers so
# that a refusal the code mishandles is reported as a leak, a use after free
# or undefined behaviour rather than passing for a clean unwind.
#
# `MRB_DEBUG` is on so that an assertion the unwinding breaks is caught here
# too.  The library tests are left out: the standard suite already runs under
# the sanitizers in its own job, and this build only exists to drive the
# scenarios.
MRuby::Build.new('alloc-fault') do |conf|
  conf.toolchain

  # `full-core` with two gems held back, rather than the gembox itself.
  #
  # mruby-task installs a periodic SIGALRM through its POSIX HAL, and the
  # handler runs the scheduler tick.  Both halves of the sweep need the run
  # to be reproducible -- the same scenario has to ask for its allocations in
  # the same order every time, so that "the 52nd allocation" names the same
  # allocation in the run that counts them and in the run that refuses one --
  # and a timer that fires on wall-clock time takes that away.  The tick also
  # keeps firing after mrb_close(), which is a finding of its own but not one
  # this sweep can say anything about.
  #
  # mruby-bin-debugger, mruby-test and mruby-sleep are what the gembox itself
  # leaves out.  mruby-alloc-fault, which the gembox also leaves out, is what
  # this build is for, so the glob here keeps it.
  Dir.glob("#{MRUBY_ROOT}/mrbgems/mruby-*/mrbgem.rake") do |path|
    gem_name = File.basename(File.dirname(path))
    next if gem_name =~ /\A mruby- (?: bin-debugger | test | sleep | task ) \z/x
    conf.gem core: gem_name
  end

  conf.enable_sanitizer 'address,undefined'
  conf.enable_debug
  conf.enable_bintest
end
