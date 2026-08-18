# Compare what run.rb wrote under CRuby with what it wrote under one or more
# mrubies, and say where they part.
#
#   ruby mrbgems/mruby-regexp/tools/differential/compare.rb [options] cruby.out mruby.out...
#
# The first file is the reference. Every file must come from the same cases
# file, so that line N is the same case in each; a run that ends early (killed,
# not resumed) is compared as far as it goes. A case is left out of the
# comparison when any run answered ERR or LIMIT for it, and the counts of
# those are printed per run, with the messages a run refused patterns with.
#
# The cases that remain are sorted by which runs disagree with the reference:
# none, or one set of the runs. With two mrubies, master and a branch, the
# set {branch} is what the branch changed away from CRuby and {master} what it
# changed towards CRuby; {master, branch} is what neither answers as CRuby
# does. Each set is printed with its count and, unless -a asks for every line,
# its first few cases with what each run answered.
#
# Options
#   -s, --show N       cases to print per set (5)
#   -a, --all          print every differing case, tab-separated:
#                      set, pattern, subject, reference answer, run answers...
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

def excluded?(ans)
  ans.start_with?("ERR", "LIMIT")
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

# Refusals and limits per run.
excluded = Array.new(runs.size) { Hash.new(0) }
compared = 0
sets = Hash.new { |h, k| h[k] = [] }
n.times do |i|
  answers = runs.map { |r| split3(r[i]) }
  pat, subj = answers[0][0], answers[0][1]
  next if opts[:match] && pat !~ opts[:match]
  out = false
  answers.each_with_index do |(_, _, ans), k|
    if excluded?(ans)
      kind, msg = ans.split(" ", 2)
      excluded[k]["#{kind}: #{msg}"] += 1
      out = true
    end
  end
  next if out
  compared += 1
  differing = (1...runs.size).select { |k| answers[k][2] != answers[0][2] }
  sets[differing] << [pat, subj, answers.map { |a| a[2] }]
end

names.each_with_index do |name, k|
  next if excluded[k].empty?
  total = excluded[k].values.sum
  puts "#{name}: #{total} left out"
  excluded[k].sort_by { |_, c| -c }.each { |msg, c| puts "  #{c}  #{msg}" }
end
puts "compared: #{compared}"

keys = sets.keys.sort_by { |k| [k.size, k] }
keys.each do |k|
  cases = sets[k]
  label = k.empty? ? "same as #{names[0]} everywhere" : "#{k.map { |j| names[j] }.join(', ')} differ from #{names[0]}"
  puts "#{label}: #{cases.size}"
  next if k.empty?
  if opts[:all]
    cases.each { |pat, subj, ans| puts ([k.map { |j| names[j] }.join(","), pat, subj] + ans).join("\t") }
  else
    cases.first(opts[:show]).each do |pat, subj, ans|
      puts "  #{pat}\t#{subj}"
      ans.each_with_index { |a, j| puts "    #{names[j].ljust(names.map(&:size).max)}  #{a}" }
    end
  end
end
