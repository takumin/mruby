# Run the cases gen.rb wrote, under CRuby or under an mruby, and print what
# each pattern answers for its subject.
#
#   ruby  mrbgems/mruby-regexp/tools/differential/run.rb cases.txt [START] > cruby.out
#   mruby mrbgems/mruby-regexp/tools/differential/run.rb cases.txt [START] > mruby.out
#
# One line out per line in, in order: the pattern, the subject and the answer,
# tab-separated. The answer is
#
#   ERR <message>    the pattern did not compile
#   LIMIT <class>    the match raised (CRuby's Regexp::TimeoutError, or its
#                    RegexpError when the address space runs out)
#   nil              no match
#   "ab"|"a"|nil     Regexp#match's MatchData#to_a, each element inspected
#
# Offsets are not compared: where an empty capture sits when the group is
# repeated is a known difference. This gem answers a step or recursion limit
# as no match, so a nil that only mruby gives may be a limit.
#
# START is the 0-based index of the first case to run, so that a run killed
# from outside can be resumed after the case it died on; every line is flushed
# for the same reason. Under CRuby, RE_TIMEOUT seconds (5 unless set) bound
# each match through Regexp.timeout, and the run itself should be started under
# a cap on its address space (`ulimit -v`), which is what turns a match that
# would take the whole machine into a RegexpError this file can record.
#
# The mruby needs mruby-io, which the default gembox has.

$stdout.sync = true
$VERBOSE = nil  # CRuby warns about nested repeats such as (?:a+)*, which are meant
path = ARGV[0] or raise "usage: run.rb cases.txt [START]"
start = (ARGV[1] || 0).to_i
if Regexp.respond_to?(:timeout=)
  Regexp.timeout = (ENV["RE_TIMEOUT"] || 5).to_f
end

def emit(pat, subj, answer)
  $stdout.puts "#{pat}\t#{subj}\t#{answer}"
end

i = -1
File.read(path).each_line do |line|
  line = line.chomp
  next if line.empty?
  i += 1
  next if i < start
  pat, subj = line.split("\t", 2)
  subj ||= ""
  begin
    re = Regexp.new(pat)
  rescue RegexpError => e
    emit(pat, subj, "ERR #{e.message.split("\n")[0].sub(/: \/.*\z/, "")}")
    next
  end
  begin
    m = re.match(subj)
  rescue NoMemoryError
    emit(pat, subj, "LIMIT NoMemoryError")
    next
  rescue => e
    emit(pat, subj, "LIMIT #{e.class}")
    next
  end
  emit(pat, subj, m ? m.to_a.map { |s| s.inspect }.join("|") : "nil")
end
