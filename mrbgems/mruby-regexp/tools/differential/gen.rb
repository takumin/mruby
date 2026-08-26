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
#   -L, --long LENGTHS       add long subjects of these lengths (comma-
#                            separated): each character of the alphabet
#                            repeated to the length, and the alphabet cycled to
#                            it. A repetition crosses an iteration per
#                            character, so a subject of a few characters
#                            reaches no limit; this is what a run long enough
#                            to reach one is drawn from.
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
#   backref-name        the same reference spelled `\k<n>` or `\k'n'`, which
#                       no digit bounds, and the relative `\k<-n>`, which
#                       counts back over the groups opened so far and so may
#                       name the group it stands in
#   backref-forward     a reference to a group the pattern opens after it,
#                       `\1(a)` and `\k<1>(a)`: valid in CRuby, and no match
#                       until the group has captured, which is what a
#                       repetition such as `(?:\1|(a))+` is written for
#   named-group         some patterns declare their capture groups with
#                       `(?<gN>...)`. A plain (...) then captures nothing and
#                       takes no number, and a numbered reference is refused
#                       whatever its spelling, so references in such a pattern
#                       name a group instead. A name is never written before
#                       the group carrying it, which CRuby refuses.
#   atomic              `(?>...)`
#   possessive          `*+` `++` `?+`, in place of the lazy mark; an interval
#                       takes neither, since `{n,m}+` is a repeat of a repeat
#   inline-option       the option letters `i`, `m` and `x` turned on and off
#                       where the pattern stands, as the bare `(?i)` `(?-mix)`
#                       `(?i-x)`, which reach to the end of the group holding
#                       them, and as the scoped `(?i:...)`, which reaches its
#                       body alone. A toggle says at least one letter, since
#                       CRuby refuses `(?)`, and no quantifier lands on one,
#                       since CRuby reads `(?i)*` as a repeat of nothing.
#   call                subexpression calls. `\g<n>`, `\g'n'` and the relative
#                       `\g<-n>` run a closed group's body again where the
#                       call stands (a name in a named pattern), a capture
#                       group may close with an optional call to itself, and
#                       a pattern that consumes on every path may end with
#                       `\g<0>?`. What keeps every drawn pattern compiling is
#                       where a call may not stand: a recursive call reachable
#                       from its group's entry with nothing consumed, or one
#                       no path avoids, is `never ending recursion` in CRuby,
#                       so a self-call goes only at the tail of a body that
#                       consumes on every path, and only under a quantifier
#                       that admits zero. Calls to closed groups make no cycle
#                       at all, every body holding calls only to groups closed
#                       before it.
#
# All features are on unless -f or -w says otherwise. Every pattern compiles
# under CRuby; what this gem refuses is reported by compare.rb.

require 'optparse'

FEATURES = %w[lazy interval class anchor empty lookahead lookbehind
              lookaround-capture backref backref-name backref-forward
              named-group atomic possessive inline-option call].freeze

opts = {
  seed: 1, count: 1000, depth: 2, quantify: 0.5, alphabet: "ab",
  subject_length: 4, long: [], all_subjects: false, features: FEATURES.dup,
}
OptionParser.new do |o|
  o.banner = "usage: ruby #{$0} [options] > cases.txt"
  o.on("-s", "--seed N", Integer) { |v| opts[:seed] = v }
  o.on("-n", "--count N", Integer) { |v| opts[:count] = v }
  o.on("-d", "--depth N", Integer) { |v| opts[:depth] = v }
  o.on("-q", "--quantify P", Float) { |v| opts[:quantify] = v }
  o.on("-a", "--alphabet CHARS") { |v| opts[:alphabet] = v }
  o.on("-l", "--subject-length N", Integer) { |v| opts[:subject_length] = v }
  o.on("-L", "--long LENGTHS") { |v| opts[:long] = v.split(",").map { |x| Integer(x) } }
  o.on("--all-subjects") { opts[:all_subjects] = true }
  o.on("-f", "--features LIST") { |v| opts[:features] = v.split(",") }
  o.on("-w", "--without LIST") { |v| opts[:features] = FEATURES - v.split(",") }
  o.on("--list-features") { puts FEATURES; exit }
end.parse!

unknown = opts[:features] - FEATURES
abort "unknown feature: #{unknown.join(', ')}" unless unknown.empty?
abort "the alphabet needs at least two distinct characters" if opts[:alphabet].chars.uniq.size < 2
abort "the alphabet cannot hold a tab or a newline" if opts[:alphabet] =~ /[\t\n\r]/
# `\1` ends where its digits do, so a literal digit behind one would join it
# and name a different group, or none.
abort "the alphabet cannot hold a digit while `\\1` is generated" if opts[:alphabet] =~ /[0-9]/ &&
                                                                    (opts[:features] & %w[backref backref-forward]).any?
abort "a long subject needs a length of at least one" if opts[:long].any? { |l| l < 1 }

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
# `unrepeatable` marks the atoms no quantifier may follow at all, whatever the
# features say: a bare option toggle is not a target a repeat can take.
Atom = Struct.new(:src, :empty, :unrepeatable)

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

# The option letters a toggle may carry, in the order they are written.
OPTION_LETTERS = %w[i m x].freeze

# The letters of one toggle, `on`, `on-off` or `-off`. At least one letter is
# drawn, since CRuby refuses `(?)`, and the letters keep the order above so
# that the same set is always spelled the same way.
def inline_option
  letters = OPTION_LETTERS.sample(rand(1..OPTION_LETTERS.size))
  on = letters.sample(rand(0..letters.size))
  off = letters - on
  src = OPTION_LETTERS.select { |c| on.include?(c) }.join
  src += "-" + OPTION_LETTERS.select { |c| off.include?(c) }.join unless off.empty?
  src
end

# Quantifiers, with the mark of whether the result can match empty. Intervals
# are drawn small so that a subject of a few characters can satisfy them. A
# `?` after any of them makes it lazy; a `+` after `*`, `+` or `?` makes it
# possessive, and after an interval it is another repeat, so it is not drawn.
def quantifier
  q, empty, bare_interval =
    case rand(on?("interval") ? 6 : 3)
    when 0 then ["*", true]
    when 1 then ["+", false]
    when 2 then ["?", true]
    when 3 then n = rand(0..2); ["{#{n}}", n == 0, true]
    when 4 then n = rand(0..2); ["{#{n},}", n == 0]
    else        n = rand(0..1); m = n + rand(1..2); ["{#{n},#{m}}", n == 0]
    end
  if on?("lazy") && rand < 0.4
    q += "?"
    # `{n}` has no lazy form, so its `?` is a quantifier of its own: the
    # whole becomes optional, and can match empty whatever n says. The comma
    # forms take it as the lazy mark, which moves no bound.
    empty = true if bare_interval
  elsif on?("possessive") && %w[* + ?].include?(q) && rand < 0.4
    q += "+"
  end
  [q, empty]
end

# groups: the number of capture groups opened so far (which is how they are
# numbered), and the numbers of those already closed, which is what a
# backreference may name. `named` says the pattern declares its groups with
# names, and a plain (...) then captures nothing, so it takes no number here.
class Groups
  attr_reader :opened, :closed
  def initialize(named = false); @opened = 0; @closed = []; @names = {}; @named = named; end
  def named?; @named; end
  def open(name = nil)
    @opened += 1
    @names[@opened] = name if name
    @opened
  end
  def close(n); @closed << n; end
  def closed_names; @closed.filter_map { |n| @names[n] }; end
end

# A forward reference stands where the pattern's group count is not known yet,
# so it is written as this marker and filled in once the pattern is whole.
# A quantifier lands on the marker as it would on the reference itself.
FORWARD = "\0"

# A reference to a group, in whichever spellings are on, or nil when the
# pattern holds nothing this reference could name.
def backreference(groups)
  forms = []
  if groups.named?
    # A named pattern refuses a numbered reference whatever its spelling, so
    # only a name reaches a group here, and only one already written.
    names = groups.closed_names
    if on?("backref-name") && names.any?
      forms << "\\k<#{names.sample}>" << "\\k'#{names.sample}'"
    end
  else
    small = groups.closed.select { |n| n <= 9 }
    forms << "\\#{small.sample}" if on?("backref") && small.any?
    if on?("backref-name")
      forms << "\\k<#{groups.closed.sample}>" << "\\k'#{groups.closed.sample}'" if groups.closed.any?
      forms << "\\k<-#{rand(1..groups.opened)}>" if groups.opened > 0
    end
    forms << FORWARD if on?("backref-forward")
  end
  forms.sample
end

# A call to a group already closed, in whichever spellings apply, or nil when
# nothing is closed. A closed group's body holds calls only to groups closed
# before it, so these calls make no cycle and every pattern they stand in
# compiles; the recursive shapes are drawn in group() and at the top level,
# where what makes them compile can be seen to hold. The relative spelling
# counts back over every group opened so far, so the group it names is spelled
# from the count at the call.
def call_reference(groups)
  forms = []
  if groups.named?
    # A named pattern refuses a numbered call whatever its spelling, as it
    # refuses a numbered backreference.
    names = groups.closed_names
    forms << "\\g<#{names.sample}>" << "\\g'#{names.sample}'" if names.any?
  elsif groups.closed.any?
    k = groups.closed.sample
    forms << "\\g<#{k}>" << "\\g'#{k}'" << "\\g<-#{groups.opened + 1 - k}>"
  end
  forms.sample
end

# Fill in the forward references: any group the pattern has is one they may
# name, whether or not it stands before them. The `\N` spelling is held to one
# digit: a longer number standing before the groups it counts is an octal
# escape and not a reference at all, while `\k<n>` is bounded by its brackets.
def resolve_forward(pat, total)
  pat.gsub(FORWARD) do
    if total.zero?
      # The pattern opened no group after all, so there is nothing to name; a
      # literal leaves an atom where a quantifier may already have been drawn.
      Regexp.escape($alphabet.sample)
    elsif on?("backref-name") && rand < 0.5
      "\\k<#{rand(1..total)}>"
    else
      "\\#{rand(1..[total, 9].min)}"
    end
  end
end

def atom(depth, groups, in_lookahead: false)
  r = rand
  if depth > 0 && r < 0.35
    group(depth, groups, in_lookahead: in_lookahead)
  elsif depth > 0 && r < 0.5 && (on?("lookahead") || on?("lookbehind"))
    lookaround(depth, groups, in_lookahead: in_lookahead)
  elsif r < 0.6 && (ref = backreference(groups))
    Atom.new(ref, true)
  elsif r < 0.65 && on?("call") && (ref = call_reference(groups))
    # Marked as able to match empty although a call consumes what its body
    # does: CRuby's never-ending analysis reads a call's minimum as zero, and
    # marking it here keeps the self-call guard in group() on the same page.
    Atom.new(ref, true)
  elsif r < 0.7 && on?("empty")
    empty_atom(groups)
  elsif r < 0.8 && on?("anchor")
    Atom.new(ANCHORS.sample, true)
  elsif r < 0.85 && on?("inline-option")
    Atom.new("(?#{inline_option})", true, true)
  elsif r < 0.9 && on?("class")
    char_class
  else
    literal
  end
end

# An atom that matches empty on its own. Two of the forms are groups, and take
# a number unless the pattern is a named one and the group is written plain.
def empty_atom(groups)
  c = Regexp.escape($alphabet.sample)
  case rand(4)
  when 0 then Atom.new("(?:)", true)
  when 1 then Atom.new("(?:#{c}|)", true)
  else
    body = rand < 0.5 ? "#{c}|" : "|#{c}"
    name = (groups.named? && rand < 0.7) ? "g#{groups.opened + 1}" : nil
    groups.close(groups.open(name)) if name || !groups.named?
    Atom.new(name ? "(?<#{name}>#{body})" : "(#{body})", true)
  end
end

def group(depth, groups, in_lookahead: false)
  # A capture group inside a lookahead is what lookaround-capture allows; a
  # capture cannot open inside a lookbehind, so a lookbehind body is never
  # built here.
  wrap = rand < 0.4 && (!in_lookahead || on?("lookaround-capture"))
  # In a named pattern a plain (...) captures nothing and takes no number, so
  # a group written that way is still drawn, and no reference can name it.
  name = (wrap && groups.named? && rand < 0.7) ? "g#{groups.opened + 1}" : nil
  capture = wrap && (!groups.named? || !name.nil?)
  n = groups.open(name) if capture
  body = seq(depth - 1, groups, in_lookahead: in_lookahead)
  if rand < 0.4
    other = seq(depth - 1, groups, in_lookahead: in_lookahead)
    src = "#{body.src}|#{other.src}"; empty = body.empty || other.empty
  else
    src = body.src; empty = body.empty
  end
  if wrap
    # A capture group may close with a call to itself: recursion, the shape
    # the feature exists for. Two guards keep the pattern one CRuby compiles.
    # The body must consume on every path (`!empty`, which alternation makes
    # the or of its branches), or the call is reachable from the group's
    # entry with nothing consumed; and the quantifier must admit zero, or no
    # invocation completes without recursing. Either way lies `never ending
    # recursion`.
    if capture && on?("call") && !empty && rand < 0.3
      q = rand < 0.5 ? "?" : "*"
      q += "?" if on?("lazy") && rand < 0.4
      src += "\\g<#{name || n}>#{q}"
    end
    groups.close(n) if capture
    Atom.new(name ? "(?<#{name}>#{src})" : "(#{src})", empty)
  elsif on?("atomic") && rand < 0.3
    Atom.new("(?>#{src})", empty)
  elsif on?("inline-option") && rand < 0.3
    Atom.new("(?#{inline_option}:#{src})", empty)
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
    if rand < $quantify && !a.unrepeatable && (on?("empty") || !a.empty)
      q, qempty = quantifier
      Atom.new(a.src + q, a.empty || qempty)
    else
      a
    end
  end
  Atom.new(atoms.map(&:src).join, atoms.all?(&:empty))
end

subjects = (0..opts[:subject_length]).flat_map { |len| $alphabet.repeated_permutation(len).map(&:join) }
# A long subject is a run rather than another string of the alphabet: one
# character repeated is what a repetition crosses an iteration at a time, and
# the alphabet cycled is what an alternation does the same over.
subjects += opts[:long].flat_map do |len|
  $alphabet.map { |c| c * len } + [($alphabet.join * (len / $alphabet.size + 1))[0, len]]
end
subjects.uniq!

opts[:count].times do
  # Half the patterns declare their groups with names when named-group is on:
  # what a name changes reaches the whole pattern, a plain (...) capturing
  # nothing in one, so it is decided here rather than group by group.
  groups = Groups.new(on?("named-group") && rand < 0.5)
  top = seq(opts[:depth], groups)
  src = top.src
  # `\g<0>` runs the whole pattern again, and may end one under the guards
  # the self-call in group() states: a pattern that consumes on every path,
  # a quantifier that admits zero.
  src += "\\g<0>?" if on?("call") && !top.empty && rand < 0.1
  pat = resolve_forward(src, groups.opened)
  if opts[:all_subjects]
    subjects.each { |s| puts "#{pat}\t#{s}" }
  else
    puts "#{pat}\t#{subjects.sample}"
  end
end
