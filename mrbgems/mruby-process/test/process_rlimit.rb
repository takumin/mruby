##
# Process.getrlimit and Process.setrlimit Test
#
# ProcessTestUtil comes from process.rb, which the test runner loads first:
# a gem's test files are run in name order.
#
# Limits are the running process's own and outlive the test that changed one,
# so every test here either restores what it found or asks for something the
# system refuses.  RLIMIT_CORE is the resource they work on: the size of a
# core file this process would leave behind, which nothing else in the suite
# reads.  A hard limit lowered cannot be raised again by a process that is
# not privileged, so none of them lowers one; the call that would, the one
# that lets max_limit default to cur_limit, is exercised with the hard limit
# already there.

assert('the resource limits are what the port declares') do
  # Whether the limits can be read and written is the port's to say, through
  # its process_hal_features.h, and it says it for each of the two calls.  A
  # method standing for a call the port does not declare is marked not
  # implemented, which `respond_to?` answers false for and a call raises
  # NotImplementedError from.  The two are read apart here: what the method
  # table answers is compared against what running the method does, on a
  # resource no list names, which a call that is really there turns down
  # before it asks the system anything.
  if Process.respond_to?(:getrlimit)
    assert_raise(ArgumentError) { Process.__send__(:getrlimit, :NOPE) }
  else
    assert_raise(NotImplementedError) { Process.__send__(:getrlimit, :NOPE) }
  end
  if Process.respond_to?(:setrlimit)
    assert_raise(ArgumentError) { Process.__send__(:setrlimit, :NOPE, 0) }
  else
    assert_raise(NotImplementedError) { Process.__send__(:setrlimit, :NOPE, 0) }
  end
  # The constants come from the same declaration the methods do: a port that
  # declares neither call names no resource, and one that declares either
  # names at least the resources POSIX gives every host with the calls.
  named = ProcessTestUtil.rlimit_resources.any? { |name| Process.const_defined?("RLIMIT_#{name}") }
  assert_equal Process.respond_to?(:getrlimit) || Process.respond_to?(:setrlimit), named
  # Process::RLIM_INFINITY is the shape of the call and stays defined either
  # way, as the wait flags do.
  assert_true Process.const_defined?(:RLIM_INFINITY)
  assert_true Process.const_defined?(:RLIM_SAVED_CUR)
  assert_true Process.const_defined?(:RLIM_SAVED_MAX)
end

assert('the answers a limit can be instead of a number') do
  # No limit, and the two saved limits a platform answers where its own type
  # is too narrow to report the limit it holds.  The numbers are mruby's own
  # and stay distinct on every host, where CRuby answers with the platform's
  # and so spells all three alike wherever the platform does.
  answers = ProcessTestUtil.rlimit_answers
  # A limit is a count and is never negative, which is what leaves these free.
  answers.each do |v|
    assert_kind_of Integer, v
    assert_operator v, :<, 0
  end
  assert_not_equal Process::RLIM_INFINITY, Process::RLIM_SAVED_CUR
  assert_not_equal Process::RLIM_INFINITY, Process::RLIM_SAVED_MAX
  assert_not_equal Process::RLIM_SAVED_CUR, Process::RLIM_SAVED_MAX
end

assert('Process.getrlimit') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?

  limits = Process.getrlimit(Process::RLIMIT_CORE)
  assert_kind_of Array, limits
  assert_equal 2, limits.size
  cur, max = limits
  assert_kind_of Integer, cur
  assert_kind_of Integer, max
  # Either is a count, or one of the three answers that are not counts; a soft
  # limit is at most the hard one it sits under.
  assert_true ProcessTestUtil.rlimit?(cur)
  assert_true ProcessTestUtil.rlimit?(max)
  assert_operator cur, :<=, max if cur >= 0 && max >= 0
end

assert('Process.getrlimit names a resource three ways') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?

  # The constant, the name as a Symbol, and the name as a String pick out the
  # same resource.  The name is the constant's without the prefix, as it is
  # in CRuby, so the prefixed spelling names nothing.
  limits = Process.getrlimit(Process::RLIMIT_CORE)
  assert_equal limits, Process.getrlimit(:CORE)
  assert_equal limits, Process.getrlimit("CORE")
  assert_raise(ArgumentError) { Process.getrlimit(:RLIMIT_CORE) }
end

assert('Process.getrlimit answers for the resources this build has') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?

  # A constant is defined for each resource this port has and for no other,
  # so `defined?(Process::RLIMIT_NPTS)` is the question a program asks rather
  # than calling to find out.  A resource on the list that this platform has
  # not is refused the way a clock it has not is, with the errno the platform
  # itself answers for one.
  ProcessTestUtil.rlimit_resources.each do |name|
    if Process.const_defined?("RLIMIT_#{name}")
      id = Process.const_get("RLIMIT_#{name}")
      assert_kind_of Integer, id
      # Both spellings name the same resource.  A limit larger than this
      # build's Integer can hold is refused for its size, by either spelling
      # alike, and that is an answer about the number rather than about the
      # resource being named.
      limits = begin
        Process.getrlimit(id)
      rescue RangeError
        nil
      end
      if limits
        assert_equal limits, Process.getrlimit(name.to_sym), name
        limits.each { |limit| assert_true ProcessTestUtil.rlimit?(limit), name }
      else
        assert_raise(RangeError, name) { Process.getrlimit(name.to_sym) }
      end
    else
      assert_raise(Errno::EINVAL, name) { Process.getrlimit(name.to_sym) }
    end
  end
end

assert('Process.getrlimit with a resource that is not one') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?

  # A name no resource has is an ArgumentError: nothing was asked of the
  # system, so there is no errno it could have failed with.  A number outside
  # the list is refused before a port sees it, with the EINVAL a platform
  # answers for a resource it does not have.
  assert_raise(ArgumentError) { Process.getrlimit(:NOPE) }
  assert_raise(ArgumentError) { Process.getrlimit("NOPE") }
  assert_raise(Errno::EINVAL) { Process.getrlimit(-1) }
  assert_raise(Errno::EINVAL) { Process.getrlimit(1000) }
  assert_raise(TypeError) { Process.getrlimit(nil) }
end

assert('Process.setrlimit lowers a soft limit') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?
  skip "this port declares no setrlimit" unless ProcessTestUtil.setrlimit?

  cur, max = Process.getrlimit(:CORE)
  begin
    # The hard limit is handed back as it was found: a soft limit is what the
    # system enforces and may be raised again, a hard one may not.
    assert_nil Process.setrlimit(:CORE, 0, max)
    assert_equal [0, max], Process.getrlimit(:CORE)
  ensure
    Process.setrlimit(:CORE, cur, max)
  end
  assert_equal [cur, max], Process.getrlimit(:CORE)
end

assert('Process.setrlimit sets the hard limit to the soft one by default') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?
  skip "this port declares no setrlimit" unless ProcessTestUtil.setrlimit?

  # Left out, or given as nil, max_limit is cur_limit.  Asked for with the
  # hard limit this process already has, so what the default writes is what
  # was there and no limit is lost to the rest of the run.
  cur, max = Process.getrlimit(:CORE)
  begin
    assert_nil Process.setrlimit(:CORE, max)
    assert_equal [max, max], Process.getrlimit(:CORE)
    assert_nil Process.setrlimit(:CORE, max, nil)
    assert_equal [max, max], Process.getrlimit(:CORE)
  ensure
    Process.setrlimit(:CORE, cur, max)
  end
end

assert('Process.setrlimit takes :INFINITY for no limit') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?
  skip "this port declares no setrlimit" unless ProcessTestUtil.setrlimit?

  cur, max = Process.getrlimit(:CORE)
  skip "this process may not leave a core file of any size" unless max == Process::RLIM_INFINITY

  # Process::RLIM_INFINITY is the number, :INFINITY and "INFINITY" name it.
  begin
    assert_nil Process.setrlimit(:CORE, :INFINITY, :INFINITY)
    assert_equal [Process::RLIM_INFINITY, Process::RLIM_INFINITY], Process.getrlimit(:CORE)
    assert_nil Process.setrlimit(:CORE, 0, "INFINITY")
    assert_equal [0, Process::RLIM_INFINITY], Process.getrlimit(:CORE)
    assert_nil Process.setrlimit(:CORE, Process::RLIM_INFINITY, max)
    assert_equal [Process::RLIM_INFINITY, max], Process.getrlimit(:CORE)
  ensure
    Process.setrlimit(:CORE, cur, max)
  end
  assert_raise(ArgumentError) { Process.setrlimit(:CORE, "NOPE", max) }
end

assert('Process.setrlimit takes the saved limits by name') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?
  skip "this port declares no setrlimit" unless ProcessTestUtil.setrlimit?

  # :SAVED_CUR and :SAVED_MAX name the limits a platform holds but cannot
  # report, as they do in CRuby.  What a host does with one is the host's to
  # say: one that spells them the way it spells no limit, which is what Linux,
  # the BSDs and macOS do, sets no limit, and one that has no such value at all
  # refuses them through errno.
  # The name itself is known either way, which is what an unknown one below
  # shows by being turned down before anything is asked of the system.
  cur, max = Process.getrlimit(:CORE)
  [:SAVED_CUR, :SAVED_MAX, Process::RLIM_SAVED_CUR, Process::RLIM_SAVED_MAX].each do |saved|
    begin
      Process.setrlimit(:CORE, saved, max)
      assert_true ProcessTestUtil.rlimit?(Process.getrlimit(:CORE)[0]), saved.to_s
    rescue Errno::EINVAL
      assert_true true, saved.to_s
    ensure
      Process.setrlimit(:CORE, cur, max)
    end
  end
  assert_raise(ArgumentError) { Process.setrlimit(:CORE, :SAVED_NOPE, max) }
  assert_equal [cur, max], Process.getrlimit(:CORE)
end

assert('Process.setrlimit with a limit that is not one') do
  skip "this port declares no getrlimit" unless ProcessTestUtil.getrlimit?
  skip "this port declares no setrlimit" unless ProcessTestUtil.setrlimit?

  cur, max = Process.getrlimit(:CORE)
  # A negative number that names none of the three answers is refused for its
  # size, before a port has to narrow it into an unsigned platform type.
  # CRuby lets one through and sets a limit of 2**64 - 4 for it.
  assert_raise(RangeError) { Process.setrlimit(:CORE, -4, max) }
  assert_raise(RangeError) { Process.setrlimit(:CORE, 0, -4) }
  assert_raise(TypeError) { Process.setrlimit(:CORE, nil, max) }
  # A soft limit above the hard one is the platform's to refuse, and it does
  # so without writing either.
  assert_raise(Errno::EINVAL) { Process.setrlimit(:CORE, 1, 0) }
  assert_equal [cur, max], Process.getrlimit(:CORE)
end
