# Run the pattern corpus through CRuby and through an mruby built with this
# gem, and report where the two engines disagree.
#
#   ruby mrbgems/mruby-regexp/tools/difftest/compare.rb [MRUBY] [--update]
#
# MRUBY is the binary to ask, and defaults to whichever build/*/bin/mruby is
# there. `--update` rewrites the baseline instead of checking against it.
#
# The baseline beside this file holds the disagreements that are meant: a
# construct this engine refuses rather than answers wrongly, a property whose
# data it does not carry, a byte CRuby settles with the pattern's encoding and
# this engine reads as a byte. Every one of them is in README.md's limitations.
# A disagreement that is not in the baseline is the thing this tool is for: an
# escape that went back to being its own letter, a class that stopped holding
# what it holds, a construct a newer CRuby gives a meaning this engine does not
# know about yet.
#
# The answers depend on the CRuby that runs it -- its Onigmo, and the Unicode
# release behind its tables -- so the baseline records which one it was taken
# with, and a run under another may differ for reasons that are not this
# engine's. See README.md before wiring it into a build that does not pin one.

require 'rbconfig'
require 'open3'

ROOT = File.expand_path('../../../..', __dir__)
PROBE = File.join(__dir__, 'probe.rb')
BASELINE = File.join(__dir__, 'baseline.txt')

update = ARGV.delete('--update')
mruby = ARGV.shift || Dir[File.join(ROOT, 'build/*/bin/mruby')].sort.first
mruby or abort "no mruby binary: build one first, or name it as an argument"
File.executable?(mruby) or abort "#{mruby}: not an executable"

# Each engine's answers as {label => [match signature, capture signature]}.
# stderr is dropped: CRuby warns about the patterns it accepts under protest
# ("invalid Unicode Property \\p"), which is not an answer either engine gives
# the caller.
def run(cmd, what)
  out, status = Open3.capture2(*cmd)
  status.success? or abort "#{what}: exited #{status.exitstatus}"
  answers = {}
  build = nil
  out.each_line do |line|
    label, sig, caps = line.chomp.split("\t", 3)
    if label == '#build'
      build = sig
      next
    end
    answers[label] = [sig, caps.to_s]
  end
  answers.empty? and abort "#{what}: no answers"
  [answers, build]
end

cruby, = run([RbConfig.ruby, '-W0', PROBE], 'cruby')
theirs, build = run([mruby, PROBE], File.basename(mruby))

version = "ruby #{RUBY_VERSION}p#{RUBY_PATCHLEVEL} #{RUBY_PLATFORM}"

# The labels are generated from the same corpus by the same code, so a label
# only one engine has means the two ran different corpora -- a stale binary,
# or a probe one of them could not finish.
missing = (cruby.keys - theirs.keys) | (theirs.keys - cruby.keys)
unless missing.empty?
  abort "the two runs do not cover the same patterns (#{missing.size}), " \
        "e.g. #{missing.first(3).join(', ')}"
end

diverging = cruby.keys.select { |label| cruby[label] != theirs[label] }

# A baseline line is the two answers and then the pattern, separated by single
# spaces. An answer holds no space and a pattern may hold several, so the
# pattern goes last and is what is left of the line -- and the file holds no
# tab, which is what the repository's own hooks ask of a file that is not a
# Makefile.
def entry(label, cruby, theirs)
  "#{cruby[label].join('|')} #{theirs[label].join('|')} #{label}"
end

def parse(line)
  want_cruby, want_theirs, label = line.chomp.split(/ /, 3)
  [label, [want_cruby, want_theirs]]
end

if update
  File.open(BASELINE, 'w') do |f|
    f.puts "# Where mruby-regexp answers a pattern differently from CRuby, as"
    f.puts "# compare.rb beside this file reads it. Regenerate with --update;"
    f.puts "# every line is a difference that is meant, and README.md says why."
    f.puts "# taken with #{version}"
    f.puts "# against a build with #{build}"
    diverging.sort.each { |label| f.puts entry(label, cruby, theirs) }
  end
  puts "wrote #{BASELINE}: #{diverging.size} of #{cruby.size} patterns differ"
  exit 0
end

File.exist?(BASELINE) or abort "#{BASELINE} not found; take one with --update"
known = {}
taken_with = nil
against = nil
File.foreach(BASELINE) do |line|
  taken_with = $1 if line =~ /\A# taken with (.*)$/
  against = $1 if line =~ /\A# against a build with (.*)$/
  next if line.start_with?('#')
  label, answers = parse(line)
  known[label] = answers
end

# A build that reads its strings as bytes, or classifies them by ASCII, answers
# differently everywhere the tables are read, and every one of those would be
# reported as a regression against this baseline. That is a build to take a
# baseline of its own against, not one to check with this one.
if against && build && against != build
  abort "the baseline describes a build with #{against}, and this one has " \
        "#{build}.\nThose builds answer differently by design; take a " \
        "baseline against this one with --update, or point the tool at the " \
        "build the baseline is for."
end

if taken_with && taken_with != version
  warn "note: the baseline was taken with #{taken_with}, this is #{version};"
  warn "      a difference below may be that CRuby's rather than this engine's"
end

new_ones = diverging.reject { |label| known.key?(label) }
changed = diverging.select do |label|
  known.key?(label) &&
    known[label] != [cruby[label].join('|'), theirs[label].join('|')]
end
gone = known.keys - diverging

new_ones.each { |label| puts "NEW      #{entry(label, cruby, theirs)}" }
changed.each { |label| puts "CHANGED  #{entry(label, cruby, theirs)}" }
gone.each { |label| puts "GONE     #{label} agrees now" }

bad = new_ones.size + changed.size + gone.size
if bad.zero?
  puts "#{cruby.size} patterns, #{diverging.size} known differences, no new ones"
  exit 0
end
warn ""
warn "#{bad} pattern(s) the baseline does not describe."
warn "A NEW or CHANGED line is this engine answering where it used to agree,"
warn "or refusing where it used to answer. A GONE line is a difference that"
warn "has been fixed: take a new baseline with --update to prune it."
exit 1
