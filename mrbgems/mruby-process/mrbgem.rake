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
  #
  # Process::Sys defines a method for every call CRuby has and marks the ones
  # this build does not have as unimplemented.  Telling that apart from a
  # method that was never defined is what the tests have to do, and
  # `respond_to?` cannot: it answers false for both, which is the whole point
  # of the mark.  `Object#methods` and `#send` can, and they are this gem's.
  spec.add_test_dependency 'mruby-metaprog', core: 'mruby-metaprog'

  # The defines are written from here rather than from the spec body:
  # setup_build resets this gem's commands before calling the block, so a
  # define set earlier would not reach the compiler.
  spec.build_settings do |spec|
    # glibc declares setresuid(2) and setresgid(2) only where this is defined,
    # and the tree compiles as -std=gnu99, which does not imply it.  Carried
    # here rather than written at the top of the POSIX port, because a
    # feature-test macro has to precede every header and a port source does not
    # always get to: the amalgam concatenates every source into one translation
    # unit, where only the first of them is before the headers.  As a define it
    # is a -D for this gem's objects in an ordinary build, and the amalgam
    # hoists it ahead of its own includes.
    spec.cc.defines << '_GNU_SOURCE'

    # Solaris and illumos declare the four-argument draft form of getpwnam_r(3)
    # and getgrnam_r(3) unless this is defined, and the POSIX port calls the
    # five-argument one POSIX settled on; see standards(7).  Nothing else reads
    # it.
    spec.cc.defines << '_POSIX_PTHREAD_SEMANTICS'

    # `Process.times` reads CPU time through getrusage(2), whose
    # <sys/resource.h> is an XSI extension rather than base POSIX.  Whether a
    # target has it is a question the port cannot ask from inside a `#if`,
    # since finding out means reading the header, so it is asked here and
    # answered to the port as HAVE_SYS_RESOURCE_H.  A target without it falls
    # back to times(2).
    spec.cc.defines << 'HAVE_SYS_RESOURCE_H' if spec.cc.check_header('sys/resource.h')

    # Which of the fifteen credential calls behind `Process::Sys` this host
    # has, one HAVE_* a call as CRuby's configure has them, for the POSIX
    # port's feature header to read.  None of them is settled by naming a
    # system.  The ten POSIX names are on every host, but setruid(2) and
    # setrgid(2) are 4.2BSD's, kept by Darwin, FreeBSD and DragonFly and
    # never in glibc; setresuid(2) and setresgid(2) began on HP-UX, Linux
    # and most of the BSDs took them up and POSIX.1-2024 has them, so a list
    # of systems could only fall behind the standard; and issetugid(2) is a
    # C library's call rather than an operating system's, glibc never having
    # had it and musl declaring it in <unistd.h> on the same Linux, so a
    # Linux build cannot be told from its name.  The compiler is asked
    # whether <unistd.h> declares each, under the _GNU_SOURCE set above that
    # glibc and musl want for setresuid(2), and the linker whether the C
    # library defines it, so that a header declaring a call the library
    # dropped does not count.
    %w[getuid geteuid getgid getegid
       setuid seteuid setruid setgid setegid setrgid
       setreuid setregid setresuid setresgid
       issetugid].each do |func|
      spec.cc.defines << "HAVE_#{func.upcase}" if spec.cc.check_func(func, header: 'unistd.h')
    end

    # Whether a name can stand for an ID, as in `Process::Sys.setuid("nobody")`.
    # The POSIX port reads the account tables through getpwnam_r(3) and
    # getgrnam_r(3), the reentrant forms and no other (the port says why),
    # and every host with the setters has had them since POSIX.1-2001; but a
    # host is not a name either, so the two are asked the same way, and a
    # port without both takes IDs by number alone, as CRuby's does when it was
    # built without <pwd.h>.
    spec.cc.defines << 'HAVE_GETPWNAM_R' if spec.cc.check_func('getpwnam_r', header: 'pwd.h')
    spec.cc.defines << 'HAVE_GETGRNAM_R' if spec.cc.check_func('getgrnam_r', header: 'grp.h')
  end
end
