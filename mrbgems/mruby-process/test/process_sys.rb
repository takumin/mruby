##
# Process::Sys Test

module ProcessSysTestUtil
  # Every method CRuby's Process::Sys has.  All of them are defined here too;
  # which ones stand for a call this build does not have is what the tests
  # below ask, rather than assuming a platform.
  METHODS = [
    :getuid, :geteuid, :getgid, :getegid,
    :setuid, :seteuid, :setruid, :setgid, :setegid, :setrgid,
    :setreuid, :setregid,
    :setresuid, :setresgid,
    :issetugid,
  ]

  # Whether every method stands for a call this build has.  The tests that
  # exercise the unimplemented ones have nothing to check on such a build,
  # and a test that checks nothing is reported as a warning, so they skip.
  def self.all_implemented?
    METHODS.all? { |m| Process::Sys.respond_to?(m) }
  end

  # Whether every call a test is about to make is one this build has.  The
  # port declares each call on its own, so a build may have a setter and not
  # the getter a test would read the ID back through; a test that makes both
  # asks about both, rather than assuming the one comes with the other.
  def self.has?(*methods)
    methods.all? { |m| Process::Sys.respond_to?(m) }
  end

  # Whether this process is root by either of the IDs it can read.  Such a
  # process is granted every credential call, so the tests that need one to
  # fail skip here rather than try.  The reverse does not hold: a process
  # that is root by neither may still be granted a call, through a saved ID
  # or a capability no getter shows, which is why those tests also undo a
  # call that was granted after all.  Reads both getters, so a test asks
  # `has?` about them before it asks this.
  def self.privileged?
    Process::Sys.getuid == 0 || Process::Sys.geteuid == 0
  end

  # A value on either side of the 32 bits an ID is on every host these tests
  # run on, or nil where this build's Integer is no wider than that and has
  # no bigints to hold one in.
  def self.out_of_range
    over = begin; 2 ** 32; rescue StandardError; nil; end
    under = begin; -(2 ** 31) - 1; rescue StandardError; nil; end
    return nil unless over.is_a?(Integer) && under.is_a?(Integer)
    [under, over]
  end
end

assert('Process::Sys') do
  assert_true Process.const_defined?(:Sys)

  # The set is CRuby's exactly: nothing of CRuby's is left out and answered as
  # a NoMethodError instead, and nothing here is an invention.  Read with
  # `methods` rather than `respond_to?`, which answers false for the ones that
  # stand for a call this build does not have.
  have = Process::Sys.methods
  inherited = Module.methods

  missing = []
  ProcessSysTestUtil::METHODS.each { |m| missing << m unless have.include?(m) }
  assert_equal [], missing

  extra = []
  have.each do |m|
    next if inherited.include?(m) || ProcessSysTestUtil::METHODS.include?(m)
    extra << m
  end
  assert_equal [], extra
end

assert('Process::Sys names the calls this build does not have') do
  # A method standing for a call the port was built without is defined, so it
  # is listed above, but `respond_to?` answers false for it and calling it
  # raises.  Asserted as a pair rather than per platform: whichever way a
  # build answers, the two answers have to agree.
  skip 'every call is implemented on this build' if ProcessSysTestUtil.all_implemented?
  ProcessSysTestUtil::METHODS.each do |m|
    next if Process::Sys.respond_to?(m)
    e = assert_raise(NotImplementedError) { Process::Sys.send(m) }
    assert_equal "#{m}() function is unimplemented on this machine", e.message
  end
  true
end

assert('Process::Sys has what the host it was built for has') do
  # The pair above holds whichever way a build answers, so a probe that
  # answered wrongly about a call would pass it.  Where the answer is known
  # it is pinned: the ten POSIX names are everywhere; Linux has setresuid(2)
  # and setresgid(2) and not setruid(2) or setrgid(2); Darwin has those two
  # and issetugid(2) and not the first two.  Whether Linux has issetugid(2)
  # is the C library's to answer, glibc no and musl yes, so it is not pinned
  # here.
  posix = {
    getuid: true, geteuid: true, getgid: true, getegid: true,
    setuid: true, seteuid: true, setgid: true, setegid: true,
    setreuid: true, setregid: true,
  }
  expected = case MRUBY_PLATFORM.split('-')[1]
             when 'linux'  then posix.merge(setruid: false, setrgid: false,
                                            setresuid: true, setresgid: true)
             when 'darwin' then posix.merge(setruid: true, setrgid: true,
                                            setresuid: false, setresgid: false,
                                            issetugid: true)
             end
  skip "no fixed answer for #{MRUBY_PLATFORM}" unless expected
  expected.each do |m, has|
    assert_equal has, Process::Sys.respond_to?(m), m.to_s
  end
  true
end

assert('Process::Sys refuses an unimplemented call whatever it is passed') do
  # What is wrong with the call is that the machine does not have it, and that
  # cannot depend on how it was written.
  skip 'every call is implemented on this build' if ProcessSysTestUtil.all_implemented?
  ProcessSysTestUtil::METHODS.each do |m|
    next if Process::Sys.respond_to?(m)
    assert_raise(NotImplementedError) { Process::Sys.send(m, 0) }
    assert_raise(NotImplementedError) { Process::Sys.send(m, 0, 0, 0, 0) }
  end
  true
end

assert('Process::Sys.getuid and the rest of the getters') do
  getters = [:getuid, :geteuid, :getgid, :getegid]
  skip 'no getter is available' unless getters.any? { |m| Process::Sys.respond_to?(m) }
  getters.each do |m|
    next unless Process::Sys.respond_to?(m)
    id = Process::Sys.send(m)
    assert_kind_of Integer, id
    assert_true id >= 0, "#{m} reported a negative ID"
  end
  true
end

assert('Process::Sys takes an ID by name') do
  # The name is looked up in whatever the platform keeps its accounts in, and
  # one no table knows is an ArgumentError: nothing was asked of the system, so
  # there is no errno for the call to have failed with.  A port that reads no
  # name takes numbers alone, and a String is then the TypeError anything but
  # an Integer is.
  skip 'Process::Sys.setuid is not available' unless Process::Sys.respond_to?(:setuid)

  unless ProcessSysTest.port_names?
    assert_raise(TypeError) { Process::Sys.setuid('nosuchuser__mruby') }
    next true
  end

  e = assert_raise(ArgumentError) { Process::Sys.setuid('nosuchuser__mruby') }
  assert_equal "can't find user for nosuchuser__mruby", e.message

  if Process::Sys.respond_to?(:setgid)
    e = assert_raise(ArgumentError) { Process::Sys.setgid('nosuchgroup__mruby') }
    assert_equal "can't find group for nosuchgroup__mruby", e.message
  end
  true
end

assert('Process::Sys resolves a name it does know') do
  # The failing lookup above says only that something was consulted.  Asking
  # for the ID this process already has says it answered, and setting an ID to
  # what it already is changes nothing.  Only reachable while running as
  # root, since that is the one account whose name is the same everywhere,
  # and the name is set on whichever of the two IDs is already root's, so the
  # call changes nothing whichever way the process is privileged.
  skip 'Process::Sys.setreuid or a user ID getter is not available' unless
    ProcessSysTestUtil.has?(:setreuid, :getuid, :geteuid)
  skip 'this port reads no name' unless ProcessSysTest.port_names?
  skip 'not running as root' unless ProcessSysTestUtil.privileged?
  real = Process::Sys.getuid == 0
  args = real ? ['root', -1] : [-1, 'root']
  skip 'this system has no account named root' if
    (begin; Process::Sys.setreuid(*args); nil; rescue ArgumentError; true; end)

  assert_nil Process::Sys.setreuid(*args)
  assert_equal 0, real ? Process::Sys.getuid : Process::Sys.geteuid
end

assert('Process::Sys refuses a number no ID could be') do
  # Which numbers name an ID is the port's to say, and on every host these
  # tests run on an ID is 32 bits read as unsigned, or the negative those bits
  # read as: -2**31 to 2**32-1.  That the port refuses the numbers just past
  # both ends is asked of it before they are sent, since on a host whose IDs
  # are wider they name IDs and the call would be made for real; what is
  # checked here is that a setter refuses what the port refuses.  A build
  # whose Integer is 32 bits wide holds such a number only as a bigint, and
  # one without bigints cannot write it at all.
  skip 'Process::Sys.setuid is not available' unless Process::Sys.respond_to?(:setuid)
  if ProcessSysTest.port_fits?('4294967296') || ProcessSysTest.port_fits?('-2147483649')
    skip 'an ID here is wider than 32 bits'
  end
  range = ProcessSysTestUtil.out_of_range
  skip 'this build cannot write a number outside the range' unless range

  under, over = range
  assert_raise(RangeError) { Process::Sys.setuid(over) }
  assert_raise(RangeError) { Process::Sys.setuid(under) }

  if Process::Sys.respond_to?(:setregid)
    # Both arguments are read before either is sent, so the second is refused
    # just as the first is and nothing has been applied by the time it is.
    assert_raise(RangeError) { Process::Sys.setregid(-1, over) }
  end
  true
end

assert('Process::Sys holds a number to the width and sign of the ID type') do
  # Which numbers name an ID is read off the platform's type, and the host has
  # one width and one sign to read it from.  The reading has to be right for
  # the other shapes too, a signed one above all: a number past its top would
  # be folded onto a lower ID by the cast, 2**32-1 onto the -1 that
  # setreuid(2) reads as "leave this one alone".  So every shape an ID has
  # had is asked about, at the number just inside each end and the one just
  # outside.  The numbers are Strings because a build whose Integer is 32 bits
  # cannot write the ones at the ends of a 32-bit type.
  [
    # bytes, signed, below the bottom, bottom, top, above the top
    [2, false, '-32769', '-32768', '65535', '65536'],
    [2, true,  '-32769', '-32768', '32767', '32768'],
    [4, false, '-2147483649', '-2147483648', '4294967295', '4294967296'],
    [4, true,  '-2147483649', '-2147483648', '2147483647', '2147483648'],
    # nothing an int64_t holds is outside a 64-bit type, whichever its sign
    [8, false, nil, '-9223372036854775808', '9223372036854775807', nil],
    [8, true,  nil, '-9223372036854775808', '9223372036854775807', nil],
  ].each do |bytes, signed, under, bottom, top, over|
    shape = "#{signed ? 'signed' : 'unsigned'} #{bytes * 8}-bit"
    assert_true ProcessSysTest.fits?('0', bytes, signed), shape
    assert_true ProcessSysTest.fits?('-1', bytes, signed), shape
    assert_true ProcessSysTest.fits?(bottom, bytes, signed), "#{shape} bottom"
    assert_true ProcessSysTest.fits?(top, bytes, signed), "#{shape} top"
    assert_false ProcessSysTest.fits?(under, bytes, signed), "#{shape} below" if under
    assert_false ProcessSysTest.fits?(over, bytes, signed), "#{shape} above" if over
  end
  true
end

assert('Process::Sys takes a number from the top half of the range') do
  # On a host whose IDs are 32 unsigned bits 2**32-1 and -1 are the same ID,
  # so the call that leaves an ID alone can be made with either spelling.  The
  # first is past what a 32-bit Integer holds and is a bigint on such a build,
  # and this is the assertion that a number the build's own Integer cannot
  # hold still reaches the call rather than being refused on its way there.
  # Whether this host is such a one is asked of the port's own predicate
  # before the call is made, not read off a refusal: a host whose IDs are 32
  # signed bits refuses 2**32-1, but one whose IDs are 64 signed bits holds it
  # as an ID of its own and would set the process to it for real.  A type
  # that holds 2**32-1 and not 2**32 is 32 unsigned bits and no other.
  skip 'Process::Sys.setreuid or a user ID getter is not available' unless
    ProcessSysTestUtil.has?(:setreuid, :getuid, :geteuid)
  unless ProcessSysTest.port_fits?('4294967295') && !ProcessSysTest.port_fits?('4294967296')
    skip 'an ID here is not 32 unsigned bits'
  end
  top = begin; 2 ** 32 - 1; rescue StandardError; nil; end
  skip 'this build cannot write 2**32-1' unless top.is_a?(Integer)

  before = [Process::Sys.getuid, Process::Sys.geteuid]
  assert_nil Process::Sys.setreuid(top, top)
  assert_nil Process::Sys.setreuid(top, -1)
  assert_equal before, [Process::Sys.getuid, Process::Sys.geteuid]

  if ProcessSysTestUtil.has?(:setresgid, :getgid, :getegid)
    before = [Process::Sys.getgid, Process::Sys.getegid]
    assert_nil Process::Sys.setresgid(top, -1, top)
    assert_equal before, [Process::Sys.getgid, Process::Sys.getegid]
  end
  true
end

assert('Process::Sys.setreuid leaves an ID alone when asked for -1') do
  # -1 is not a number this gem gives a meaning to; it reaches the platform as
  # the (uid_t)-1 that setreuid(2) reads as "leave this one alone".  A call
  # that changes nothing is the one call these tests can make for real.
  skip 'Process::Sys.setreuid or a user ID getter is not available' unless
    ProcessSysTestUtil.has?(:setreuid, :getuid, :geteuid)
  before = [Process::Sys.getuid, Process::Sys.geteuid]
  assert_nil Process::Sys.setreuid(-1, -1)
  assert_equal before, [Process::Sys.getuid, Process::Sys.geteuid]
end

assert('Process::Sys.setregid leaves an ID alone when asked for -1') do
  skip 'Process::Sys.setregid or a group ID getter is not available' unless
    ProcessSysTestUtil.has?(:setregid, :getgid, :getegid)
  before = [Process::Sys.getgid, Process::Sys.getegid]
  assert_nil Process::Sys.setregid(-1, -1)
  assert_equal before, [Process::Sys.getgid, Process::Sys.getegid]
end

assert('Process::Sys.setresuid leaves an ID alone when asked for -1') do
  skip 'Process::Sys.setresuid or a user ID getter is not available' unless
    ProcessSysTestUtil.has?(:setresuid, :getuid, :geteuid)
  before = [Process::Sys.getuid, Process::Sys.geteuid]
  assert_nil Process::Sys.setresuid(-1, -1, -1)
  assert_equal before, [Process::Sys.getuid, Process::Sys.geteuid]
end

assert('Process::Sys.setresgid leaves an ID alone when asked for -1') do
  skip 'Process::Sys.setresgid or a group ID getter is not available' unless
    ProcessSysTestUtil.has?(:setresgid, :getgid, :getegid)
  before = [Process::Sys.getgid, Process::Sys.getegid]
  assert_nil Process::Sys.setresgid(-1, -1, -1)
  assert_equal before, [Process::Sys.getgid, Process::Sys.getegid]
end

assert('Process::Sys.issetugid answers yes or no') do
  skip 'Process::Sys.issetugid is not available' unless Process::Sys.respond_to?(:issetugid)
  # Which of the two it is depends on how this process was started, so only
  # that it is one of them can be asserted.
  assert_include [true, false], Process::Sys.issetugid
end

assert('Process::Sys reports a refused call by the error alone') do
  # What a SystemCallError message carries after the error is the object the
  # call was working on, the way File.open names the path it could not open.
  # A credential call works on no such object, so the message is the error by
  # itself.  Compared against the text this platform gives that errno rather
  # than against a literal, so the wording is not pinned.
  #
  # The refusal is provoked with seteuid(0).  A process that is root already
  # is skipped without asking.  For the rest, whether the call is granted is
  # not settled by the two IDs they can read: Linux also grants it to a
  # process whose saved user ID is 0, which no getter shows, and to one
  # holding CAP_SETUID.  So the call is made and, in the one case it is
  # granted, the effective ID is put back and the test skips.  seteuid(2)
  # rather than setuid(2) because it moves only the effective ID, and what
  # makes the move one that can be undone is where it is undone to: the one
  # ID seteuid(2) is promised to accept, whatever the process holds, is the
  # real user ID.  So the test asks that the effective ID be the real one
  # before it moves it, and skips otherwise.  A process whose two IDs differ
  # may owe that to a saved user ID of 0, which lets seteuid(0) in, and the
  # way back to its old effective ID would then need a privilege that being
  # effectively root does not carry on every host.
  skip 'Process::Sys.seteuid or a user ID getter is not available' unless
    ProcessSysTestUtil.has?(:seteuid, :getuid, :geteuid)
  skip 'running with privileges; seteuid(0) would succeed' if ProcessSysTestUtil.privileged?

  uid = Process::Sys.getuid
  skip 'the effective user ID differs from the real one' unless uid == Process::Sys.geteuid
  e = begin
        Process::Sys.seteuid(0)
        nil
      rescue SystemCallError => x
        x
      end
  if e.nil?
    Process::Sys.seteuid(uid)
    skip 'this process may become root'
  end
  assert_equal uid, Process::Sys.geteuid
  assert_equal SystemCallError.new(e.errno).message, e.message
end
