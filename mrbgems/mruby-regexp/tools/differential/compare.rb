# Compare what run.rb wrote under CRuby with what it wrote under one or more
# mrubies, and say where they part.
#
#   ruby mrbgems/mruby-regexp/tools/differential/compare.rb [options] cruby.out mruby.out...
#
# The first file is the reference. Every file must come from the same cases
# file, so that line N is the same case in each; a run that ends early (killed,
# not resumed) is compared as far as it goes. A case is left out of the
# comparison only when the reference answered ERR or LIMIT for it, since there
# is then nothing to compare against; the count of those is printed with the
# messages. An mruby that answers ERR or LIMIT where the reference has an
# answer disagrees with it, and the case stays in: it is sorted with the ERR
# or LIMIT as that run's answer, so a pattern one mruby refuses and another
# answers is still compared for the one that answers. The messages each mruby
# refused patterns with are counted per run and printed first.
#
# The cases are sorted by what each run does against the reference: answers
# the same, answers differently, ERR or LIMIT. With two mrubies, master and a
# branch, "branch differs" is what the branch changed away from CRuby and
# "master differs" what it changed towards CRuby; "master, branch differ" is
# what neither answers as CRuby does, and "master ERR, branch differs" is what
# the branch made compile and answers differently, which "master, branch
# differ" would hide if the case were dropped. Each set is printed with its
# count and, unless -a asks for every line, its first few cases with what each
# run answered.
#
# Options
#   -s, --show N       cases to print per set (5)
#   -a, --all          print every differing case, tab-separated:
#                      set (run=status,...), pattern, subject, reference
#                      answer, run answers...
#   -m, --match REGEX  only cases whose pattern matches (a Ruby regexp)

require 'optparse'

opts = { show: 5, all: false, match: nil }
OptionParser.new do |o|
  o.banner = "usage: ruby #{$0} [options] cruby.out mruby.out..."
  o.on("-s", "--show N", Integer) { |v| opts[:show] = v }
  o.on("-a", "--all") { opts[:all] = true }
  o.on("-m", "--match REGEX") { |v| opts[:match] = Regexp.new(v) }
end.parse!
abort "usage: ruby #{$0} [options] cruby.out mruby.out..." if ARGV.size < 2

names = ARGV.map { |f| File.basename(f, ".*") }
runs = ARGV.map { |f| File.readlines(f, chomp: true) }
ref = runs[0]

def split3(line)
  pat, subj, ans = line.split("\t", 3)
  [pat, subj || "", ans || ""]
end

# ERR or LIMIT for an answer that is one, nil for an answer to compare.
def kind(ans)
  ans.start_with?("ERR", "LIMIT") ? ans.split(" ", 2)[0] : nil
end

# Sanity: the same case on every line that all runs reached.
runs.each_with_index do |r, k|
  next if k == 0
  [r.size, ref.size].min.times do |i|
    a = split3(ref[i]); b = split3(r[i])
    if a[0] != b[0] || a[1] != b[1]
      abort "#{names[k]} line #{i + 1} is a different case from #{names[0]}: not the same cases file?"
    end
  end
end

n = runs.map(&:size).min
if runs.map(&:size).uniq.size > 1
  short = runs.each_with_index.reject { |r, _| r.size == ref.size }.map { |r, k| "#{names[k]} #{r.size}" }
  puts "cases: #{ref.size} in #{names[0]}, fewer in #{short.join(', ')}; comparing the first #{n}"
else
  puts "cases: #{n}"
end

# What each run does against the reference on a case, in the order the sets
# are listed in.
STATUS = %w[same differs ERR LIMIT].freeze

# Refusals and limits per run; the reference's leave the case out.
messages = Array.new(runs.size) { Hash.new(0) }
left_out = 0
compared = 0
sets = Hash.new { |h, k| h[k] = [] }
n.times do |i|
  answers = runs.map { |r| split3(r[i]) }
  pat, subj = answers[0][0], answers[0][1]
  next if opts[:match] && pat !~ opts[:match]
  status = answers.each_with_index.map do |(_, _, ans), k|
    if (kd = kind(ans))
      messages[k]["#{kd}: #{ans.split(' ', 2)[1]}"] += 1
      kd
    elsif ans == answers[0][2]
      "same"
    else
      "differs"
    end
  end
  if kind(answers[0][2])
    left_out += 1
    next
  end
  compared += 1
  sets[status[1..]] << [pat, subj, answers.map { |a| a[2] }]
end

names.each_with_index do |name, k|
  next if messages[k].empty?
  total = messages[k].values.sum
  puts "#{name}: #{total} #{k == 0 ? 'left out' : 'not answered'}"
  messages[k].sort_by { |_, c| -c }.each { |msg, c| puts "  #{c}  #{msg}" }
end
puts "compared: #{compared}"

# "master ERR, branch differs from cruby": the runs grouped by what they do,
# in the order the runs were given, and "from <reference>" after a group that
# differs.
def label(status, names)
  return "same as #{names[0]} everywhere" if status.all? { |s| s == "same" }
  parts = (status.uniq - ["same"]).map do |s|
    who = status.each_index.select { |j| status[j] == s }.map { |j| names[j + 1] }
    if s == "differs"
      "#{who.join(', ')} #{who.size == 1 ? 'differs' : 'differ'} from #{names[0]}"
    else
      "#{who.join(', ')} #{s}"
    end
  end
  parts.join(", ")
end

keys = sets.keys.sort_by { |k| [k.count { |s| s != "same" }, k.map { |s| STATUS.index(s) }] }
keys.each do |k|
  cases = sets[k]
  puts "#{label(k, names)}: #{cases.size}"
  next if k.all? { |s| s == "same" }
  if opts[:all]
    set = k.each_index.reject { |j| k[j] == "same" }.map { |j| "#{names[j + 1]}=#{k[j]}" }.join(",")
    cases.each { |pat, subj, ans| puts ([set, pat, subj] + ans).join("\t") }
  else
    cases.first(opts[:show]).each do |pat, subj, ans|
      puts "  #{pat}\t#{subj}"
      ans.each_with_index { |a, j| puts "    #{names[j].ljust(names.map(&:size).max)}  #{a}" }
    end
  end
end
