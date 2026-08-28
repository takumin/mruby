# Shrink one case from a differential run to the smallest pattern and subject
# that still part the runs the same way.
#
#   ruby mrbgems/mruby-regexp/tools/differential/minimize.rb [options] \
#     PATTERN SUBJECT -- [name=]mruby...
#
# The case is the pattern and the subject as compare.rb prints them, and what
# follows `--` names the mruby binaries, as differential.sh takes them. The
# case is run through differential.sh, so the address-space cap, the timeout
# and the resume after a kill are the ones a full run uses; TIMEOUT is 15
# seconds here rather than 600, since a candidate that runs long is one to
# drop, and MEM_MB, RUBY and OUT keep their meaning.
#
# What is held fixed is which runs disagree with CRuby and how: every run's
# answer is read as "same as CRuby", "differs", ERR or LIMIT, and a candidate
# is taken only when that reading is unchanged for every run. So a case both
# mrubies answer differently shrinks to a smaller one they both answer
# differently, and a case only one of them gives up on shrinks to one only
# that run gives up on, rather than to any case where something disagrees.
# A case CRuby itself answers ERR or LIMIT is not shrunk: there is nothing to
# hold it against.
#
# Candidates are the pattern with one run of characters cut out, longest cut
# first, and the subject with one character cut out; the first candidate that
# reads the same is taken and the search starts over on it. Cuts that do not
# compile drop out on their own, since CRuby then answers ERR where it
# answered a match.
#
# The case is written and cut in the spelling a case file carries, which is
# the one compare.rb prints: a backslash is `\\` there and a newline, a tab
# and a carriage return are `\n`, `\t` and `\r`, so that a case holding one
# of them is still one line. run.rb unescapes what it runs, and the candidate
# file this writes says so in the `#escaped` header it opens with. A cut may
# fall inside such a spelling, and the candidate it makes then reads
# differently and is dropped like any other.
#
# Options
#   -q, --quiet   do not report each round on stderr

require 'optparse'
require 'fileutils'

usage = "usage: ruby #{$0} [options] PATTERN SUBJECT -- [name=]mruby..."
sep = ARGV.index("--")
abort usage if sep.nil?
head = ARGV[0...sep]
bins = ARGV[(sep + 1)..]

opts = { quiet: false }
OptionParser.new do |o|
  o.banner = usage
  o.on("-q", "--quiet") { opts[:quiet] = true }
end.parse!(head)

abort usage if head.empty?
pattern, subject = head[0], (head[1] || "")
abort "no mruby given" if bins.empty?

HERE = File.dirname(File.expand_path(__FILE__))
OUT = ENV["OUT"] || "minimize.out"
NAMES = ["cruby"] + bins.map { |b| b.include?("=") ? b.split("=", 2)[0] : File.basename(File.dirname(File.dirname(b))) }

FileUtils.mkdir_p(OUT)

# Run every case through differential.sh and answer one line per case per run.
def run(cases, bins)
  path = File.join(OUT, "candidates.txt")
  File.write(path, (["#escaped"] + cases.map { |p, s| "#{p}\t#{s}" }).join("\n") + "\n")
  env = { "CASES" => path, "OUT" => OUT, "NO_COMPARE" => "1", "TIMEOUT" => ENV["TIMEOUT"] || "15" }
  ok = system(env, "bash", File.join(HERE, "differential.sh"), *bins, out: File::NULL, err: File::NULL)
  abort "differential.sh failed" unless ok
  NAMES.map do |name|
    File.readlines(File.join(OUT, "#{name}.out"), chomp: true).map { |l| l.split("\t", 3)[2].to_s }
  end
end

# same, differs, ERR or LIMIT, as compare.rb reads an answer against CRuby's.
def reading(reference, answer)
  return answer.split(" ", 2)[0] if answer.start_with?("ERR", "LIMIT")
  answer == reference ? "same" : "differs"
end

def readings(answers, index)
  reference = answers[0][index]
  return nil if reference.start_with?("ERR", "LIMIT")
  (1...answers.size).map { |k| reading(reference, answers[k][index]) }
end

answers = run([[pattern, subject]], bins)
wanted = readings(answers, 0)
abort "CRuby answers #{answers[0][0]} for this case, so there is nothing to hold it against" if wanted.nil?
warn "holding: #{NAMES.drop(1).zip(wanted).map { |n, r| "#{n}=#{r}" }.join(', ')}" unless opts[:quiet]

round = 0
loop do
  candidates = []
  (1...pattern.length).to_a.reverse.each do |cut|
    (0..pattern.length - cut).each do |at|
      shorter = pattern.dup
      shorter[at, cut] = ""
      candidates << [shorter, subject]
    end
  end
  subject.length.times do |at|
    shorter = subject.dup
    shorter[at, 1] = ""
    candidates << [pattern, shorter]
  end
  candidates.uniq!
  candidates.reject! { |p, s| p.empty? || p.include?("\t") || s.include?("\t") }
  break if candidates.empty?

  warn "round #{round}: #{candidates.size} candidates from /#{pattern}/ on #{subject.inspect}" unless opts[:quiet]
  answers = run(candidates, bins)
  taken = candidates.each_index.find { |i| readings(answers, i) == wanted }
  break if taken.nil?
  pattern, subject = candidates[taken]
  round += 1
end

answers = run([[pattern, subject]], bins)
puts "#{pattern}\t#{subject}"
NAMES.each_with_index { |name, k| puts "  #{name.ljust(8)}#{answers[k][0]}" }
