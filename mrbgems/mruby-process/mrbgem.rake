MRuby::Gem::Specification.new('mruby-process') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mruby developers'
  spec.summary = 'Process module and Process::Status / Process::Tms classes'

  # `Process.kill(:TERM, pid)` takes a signal by name and `Process::Status#to_s`
  # spells one out, so both need the platform's signal table.  mruby-signal owns
  # it; this gem reaches it through signal_hal.h rather than keeping a copy.
  spec.add_dependency 'mruby-signal', core: 'mruby-signal'

  # `Process.times` answers a `Process::Tms`, which is a Struct in CRuby and
  # is one here too rather than a hand-written class that reimplements what a
  # Struct already does: the members, `#to_a`, `#==` and `#inspect`.
  spec.add_dependency 'mruby-struct', core: 'mruby-struct'

  # mruby-process needs no I/O of its own.  The tests do: waiting on a child
  # is only testable with a child, and IO.popen is how one is made.  The
  # dependency stops at the tests; see README.md.
  spec.add_test_dependency 'mruby-io', core: 'mruby-io'

  # A gem's tests run in a state holding its dependency closure and nothing
  # else, so a test that means to name an Errno class has to ask for the gem
  # that defines them.  Without this the tests still pass, by taking the
  # branch that settles for any StandardError.
  spec.add_test_dependency 'mruby-errno', core: 'mruby-errno'

  # Asking a Process::Status what instance_variables it hands out needs the
  # gem that defines Object#instance_variables in the first place.
  spec.add_test_dependency 'mruby-metaprog', core: 'mruby-metaprog'

  # `Process.times` reads CPU time through getrusage(2), whose <sys/resource.h>
  # is an XSI extension rather than base POSIX.  Whether a target has it is a
  # question the port cannot ask from inside a `#if`, since finding out means
  # reading the header, so it is asked here and answered to the port as
  # HAVE_SYS_RESOURCE_H.  A target without it falls back to times(2).
  spec.build_settings do |spec|
    spec.cc.defines << 'HAVE_SYS_RESOURCE_H' if spec.cc.check_header('sys/resource.h')

    # Whether this host has the two calls behind `Process.getrlimit` and
    # `Process.setrlimit`, one HAVE_* a call as CRuby's configure has them,
    # for the POSIX port's feature header to read.  They live in the same
    # <sys/resource.h> that getrusage(2) does and are XSI extensions the same
    # way, so a host is not what settles it; the compiler is asked whether
    # that header declares each, and the linker whether the C library defines
    # it.  Asked one at a time, as CRuby asks, so that a host with the reader
    # and not the writer keeps the reader.  Which resources a limit can be
    # set on is a separate question, and one the preprocessor can answer on
    # its own: the port reads RLIMIT_* with `#ifdef`, as CRuby's process.c
    # does.
    %w[getrlimit setrlimit].each do |func|
      spec.cc.defines << "HAVE_#{func.upcase}" if spec.cc.check_func(func, header: 'sys/resource.h')
    end

    # Whether a limit needs no more than the `rlim_t` this host's getrlimit(2)
    # answers in.  POSIX lets a platform report RLIM_SAVED_CUR in place of a
    # limit that type is too narrow for, which is the 32-bit compilation
    # environment of a host whose kernel counts limits in 64 bits.  Such a
    # host usually declares getrlimit64(2) beside it, which the port asks
    # instead where this probe says the narrower call would have to give up;
    # whether it does is asked below rather than taken for granted.  The width
    # is asked of the compiler, not being something a `#if` can read.
    wide = spec.cc.try_compile(<<~PROBE)
      #include <sys/resource.h>
      int mrb_probe[sizeof(rlim_t) >= 8 ? 1 : -1];
    PROBE
    spec.cc.defines << 'HAVE_WIDE_RLIM_T' if wide

    unless wide
      # glibc declares the wider calls only where this is defined, and answers
      # them in the `rlim64_t` its RLIM64_* values are written in.  Pushed
      # before the probes so that they see the declarations the port will.
      spec.cc.defines << '_LARGEFILE64_SOURCE'
      %w[getrlimit64 setrlimit64].each do |func|
        spec.cc.defines << "HAVE_#{func.upcase}" if spec.cc.check_func(func, header: 'sys/resource.h')
      end
    end
  end
end
