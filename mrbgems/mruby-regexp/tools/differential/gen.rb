# Generate the cases for a differential run of this gem against CRuby: random
# patterns, each paired with a subject.
#
#   ruby mrbgems/mruby-regexp/tools/differential/gen.rb [options] > cases.txt
#
# One case per line: the pattern, a tab, the subject. run.rb reads the file
# under CRuby and under each mruby to compare, and compare.rb reads what they
# wrote. The generator runs under CRuby.
#
# The patterns are built over a small alphabet ("ab" unless -a says otherwise)
# from literals, groups, alternation and quantifiers, plus what the enabled
# features add; the subjects are every string over the alphabet up to a length.
# The same seed gives the same file, so a run can be repeated or narrowed.
#
# Options
#   -s, --seed N             seed of the random generator (1)
#   -n, --count N            patterns to generate (1000)
#   -d, --depth N            how deeply groups nest (2)
#   -q, --quantify P         probability that an atom is quantified (0.5)
#   -a, --alphabet CHARS     the characters literals, classes and subjects are
#                            drawn from ("ab")
#   -l, --subject-length N   subjects are every string over the alphabet of at
#                            most this length (4)
#       --all-subjects       pair each pattern with every subject rather than
#                            with one drawn at random
#   -f, --features LIST      exactly these features (comma-separated)
#   -w, --without LIST       the default features minus these
#       --list-features      print the features and exit
#
# Features
#   lazy                lazy quantifiers `*?` `+?` `??`
#   interval            interval quantifiers `{n}` `{n,}` `{n,m}` (lazy forms
#                       when lazy is on)
#   class               `.` `\w` `\W` and classes over the alphabet, negated too
#   anchor              `^` `$` `\A` `\z` `\Z` `\b` `\B`
#   empty               atoms that match empty on their own (`(?:)`, `(a|)`,
#                       `(|b)`, `(?:a|)`), and quantifiers on atoms that can
#                       already match empty, so repetitions of empty-matching
#                       bodies nest
#   lookahead           `(?=...)` and `(?!...)`
#   lookbehind          `(?<=...)` and `(?<!...)`, fixed-length bodies of one
#                       or two literals
#   lookaround-capture  a capture group may open inside a lookahead
#   backref             `\1`..`\9` to a group closed earlier in the pattern
#   atomic              `(?>...)`
#
# All features are on unless -f or -w says otherwise. Every pattern compiles
# under CRuby; what this gem refuses is reported by compare.rb.

require 'optparse'

FEATURES = %w[lazy interval class anchor empty lookahead lookbehind
              lookaround-capture backref atomic].freeze

opts = {
  seed: 1, count: 1000, depth: 2, quantify: 0.5, alphabet: "ab",
  subject_length: 4, all_subjects: false, features: FEATURES.dup,
}
OptionParser.new do |o|
  o.banner = "usage: ruby #{$0} [options] > cases.txt"
  o.on("-s", "--seed N", Integer) { |v| opts[:seed] = v }
  o.on("-n", "--count N", Integer) { |v| opts[:count] = v }
  o.on("-d", "--depth N", Integer) { |v| opts[:depth] = v }
  o.on("-q", "--quantify P", Float) { |v| opts[:quantify] = v }
  o.on("-a", "--alphabet CHARS") { |v| opts[:alphabet] = v }
  o.on("-l", "--subject-length N", Integer) { |v| opts[:subject_length] = v }
  o.on("--all-subjects") { opts[:all_subjects] = true }
  o.on("-f", "--features LIST") { |v| opts[:features] = v.split(",") }
  o.on("-w", "--without LIST") { |v| opts[:features] = FEATURES - v.split(",") }
  o.on("--list-features") { puts FEATURES; exit }
end.parse!

unknown = opts[:features] - FEATURES
abort "unknown feature: #{unknown.join(', ')}" unless unknown.empty?
abort "the alphabet needs at least two distinct characters" if opts[:alphabet].chars.uniq.size < 2
abort "the alphabet cannot hold a tab or a newline" if opts[:alphabet] =~ /[\t\n\r]/

srand(opts[:seed])
$features = opts[:features]
$alphabet = opts[:alphabet].chars.uniq
$quantify = opts[:quantify]

def on?(feature)
  $features.include?(feature)
end

# An atom is a piece of pattern and whether it can match empty. Whether it can
# decides if a quantifier may go on it when the empty feature is off, so that
# repetitions of empty-matching bodies appear only when asked for.
Atom = Struct.new(:src, :empty)

def literal
  Atom.new(Regexp.escape($alphabet.sample), false)
end

def char_class
  c = $alphabet.sample
  case rand(5)
  when 0 then Atom.new(".", false)
  when 1 then Atom.new(rand < 0.5 ? "\\w" : "\\W", false)
  when 2 then Atom.new("[^#{Regexp.escape(c)}]", false)
  else
    n = rand(1..$alphabet.size)
    Atom.new("[#{$alphabet.sample(n).map { |x| Regexp.escape(x) }.join}]", false)
  end
end

ANCHORS = %w[^ $ \A \z \Z \b \B].freeze

# Quantifiers, with the mark of whether the result can match empty. Intervals
# are drawn small so that a subject of a few characters can satisfy them.
def quantifier
  q, empty =
    case rand(on?("interval") ? 6 : 3)
    when 0 then ["*", true]
    when 1 then ["+", false]
    when 2 then ["?", true]
    when 3 then n = rand(0..2); ["{#{n}}", n == 0]
    when 4 then n = rand(0..2); ["{#{n},}", n == 0]
    else        n = rand(0..1); m = n + rand(1..2); ["{#{n},#{m}}", n == 0]
    end
  q += "?" if on?("lazy") && rand < 0.4
  [q, empty]
end

# groups: the number of capture groups opened so far (which is how they are
# numbered), and the numbers of those already closed, which is what a
# backreference may name.
class Groups
  attr_reader :opened, :closed
  def initialize; @opened = 0; @closed = []; end
  def open; @opened += 1; end
  def close(n); @closed << n; end
end

def atom(depth, groups, in_lookahead: false)
  r = rand
  if depth > 0 && r < 0.35
    group(depth, groups, in_lookahead: in_lookahead)
  elsif depth > 0 && r < 0.5 && (on?("lookahead") || on?("lookbehind"))
    lookaround(depth, groups, in_lookahead: in_lookahead)
  elsif r < 0.6 && on?("backref") && groups.closed.any? { |n| n <= 9 }
    Atom.new("\\#{groups.closed.select { |n| n <= 9 }.sample}", true)
  elsif r < 0.7 && on?("empty")
    empty_atom(groups)
  elsif r < 0.8 && on?("anchor")
    Atom.new(ANCHORS.sample, true)
  elsif r < 0.9 && on?("class")
    char_class
  else
    literal
  end
end

# An atom that matches empty on its own. Two of the forms are capture groups
# and take a number.
def empty_atom(groups)
  c = Regexp.escape($alphabet.sample)
  case rand(4)
  when 0 then Atom.new("(?:)", true)
  when 1 then Atom.new("(?:#{c}|)", true)
  else
    n = groups.open
    groups.close(n)
    Atom.new(rand < 0.5 ? "(#{c}|)" : "(|#{c})", true)
  end
end

def group(depth, groups, in_lookahead: false)
  # A capture group inside a lookahead is what lookaround-capture allows; a
  # capture cannot open inside a lookbehind, so a lookbehind body is never
  # built here.
  capture = rand < 0.4 && (!in_lookahead || on?("lookaround-capture"))
  n = groups.open if capture
  body = seq(depth - 1, groups, in_lookahead: in_lookahead)
  if rand < 0.4
    other = seq(depth - 1, groups, in_lookahead: in_lookahead)
    src = "#{body.src}|#{other.src}"; empty = body.empty || other.empty
  else
    src = body.src; empty = body.empty
  end
  if capture
    groups.close(n)
    Atom.new("(#{src})", empty)
  elsif on?("atomic") && rand < 0.3
    Atom.new("(?>#{src})", empty)
  else
    Atom.new("(?:#{src})", empty)
  end
end

def lookaround(depth, groups, in_lookahead: false)
  kinds = []
  kinds += ["(?=", "(?!"] if on?("lookahead")
  kinds += ["(?<=", "(?<!"] if on?("lookbehind")
  kind = kinds.sample
  if kind.start_with?("(?<")
    body = Array.new(rand(1..2)) { Regexp.escape($alphabet.sample) }.join
  else
    body = seq(depth - 1, groups, in_lookahead: true).src
  end
  Atom.new("#{kind}#{body})", true)
end

def seq(depth, groups, in_lookahead: false)
  atoms = Array.new(rand(1..3)) do
    a = atom(depth, groups, in_lookahead: in_lookahead)
    if rand < $quantify && (on?("empty") || !a.empty)
      q, qempty = quantifier
      Atom.new(a.src + q, a.empty || qempty)
    else
      a
    end
  end
  Atom.new(atoms.map(&:src).join, atoms.all?(&:empty))
end

subjects = (0..opts[:subject_length]).flat_map { |len| $alphabet.repeated_permutation(len).map(&:join) }

opts[:count].times do
  groups = Groups.new
  pat = seq(opts[:depth], groups).src
  if opts[:all_subjects]
    subjects.each { |s| puts "#{pat}\t#{s}" }
  else
    puts "#{pat}\t#{subjects.sample}"
  end
end
