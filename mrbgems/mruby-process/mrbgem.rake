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

  # mruby-process needs no I/O of its own.  One test does: mruby-io sets `$?`
  # through this gem when an IO.popen stream closes, and that seam is asserted
  # from this side.  The children the other tests need are their own, made
  # with `Process.spawn`.  The dependency stops at the tests; see README.md.
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
  #
  # `Process.spawn` reports a failed exec down a close-on-exec pipe, and that
  # pipe has to be close-on-exec from the call that creates it: pipe() and a
  # following fcntl() leave a window in which another thread of the embedding
  # process can fork and exec, carrying the write end into a child this parent
  # cannot close.  pipe2() is the call that leaves no window, and it is POSIX
  # only as of Issue 8, so which hosts have it is a question rather than a list
  # of platforms: it is asked of the compiler here and answered to the port as
  # HAVE_PIPE2.  A host without it makes the pipe in two calls; see
  # ports/posix/process_hal.c.
  #
  # A C library may also keep the call behind a feature-test macro, glibc's
  # _GNU_SOURCE among them, and a macro that has to be defined before any
  # header is read is one the compile line is the place for.  The probe below
  # is run by the compiler that compiles the gem and so is asked the question
  # under the flags the port will be compiled with, this define included: the
  # answer cannot disagree with the compile it is answering for.
  #
  # pipe2 is named rather than called, as `check_func` names one: an undeclared
  # name is the error that answers no, and a declared one asks nothing of a
  # library the probe never links.
  #
  # A host that has posix_spawn() need make neither the pipe nor the fork: it
  # creates the child and executes the image in the one call and answers with
  # the errno a failed exec left.  Whether the call is there is HAVE_POSIX_SPAWN.
  # Whether a host's posix_spawn() reports that failure at all is a question no
  # compile can answer and is settled in the port; see
  # MRB_PROCESS_HAVE_POSIX_SPAWN there.
  spec.build_settings do |spec|
    spec.cc.defines << 'HAVE_SYS_RESOURCE_H' if spec.cc.check_header('sys/resource.h')

    spec.cc.defines << '_GNU_SOURCE'
    spec.cc.defines << 'HAVE_PIPE2' if spec.cc.try_compile(<<~PROBE)
      #include <fcntl.h>
      #include <unistd.h>
      int mrb_probe(void);
      int mrb_probe(void) { (void)pipe2; return O_CLOEXEC; }
    PROBE

    spec.cc.defines << 'HAVE_POSIX_SPAWN' if spec.cc.check_func('posix_spawn', header: 'spawn.h')
  end
end
