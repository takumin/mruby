# Generate the cases for a differential run over the references a pattern may
# make, the ones it may not among them: every sequence of groups up to a
# length, each paired with every spelling of a numbered, relative and named
# backreference and subexpression call, written after the groups, before
# them, and inside the last of them -- which for a call is where recursion
# stands, wanted or refused.
#
#   ruby mrbgems/mruby-regexp/tools/differential/refusals.rb [options] > cases.txt
#   CASES=cases.txt COMPARE_OPTS=-e bash \
#     mrbgems/mruby-regexp/tools/differential/differential.sh [name=]mruby...
#
# gen.rb draws only patterns CRuby compiles, since a case it refuses has no
# answer to compare with. Which message a pattern is refused with is a
# behaviour of its own, and this is the corpus for it: compare.rb's -e keeps
# the cases the reference refused and compares the message. The patterns that
# do compile are compared as any other case is, so the corpus is a
# backreference corpus either way.
#
# What a reference resolves to depends on how the groups before it are
# written, so the sequences are drawn over the four forms that number a group
# differently: a plain group, which takes a number; `(?:...)`, which takes
# none; a named group, which takes one and stops the plain ones from taking
# any; and a capture inside a lookahead, which takes its number where it
# stands. A name is drawn for a group the pattern opens and for one it does
# not.
#
# Beside the sequences drawn over the forms, the corpus holds runs of plain
# groups at the ceiling on how many a pattern may open, and references at the
# numbers around it. That is where a number stops being one any pattern could
# carry, and where the count of groups stops being one this gem compiles at
# all, so those rows differ from the reference by design and are here to hold
# the line where it is rather than to agree with CRuby.
#
# Options
#   -g, --groups N     how many groups a pattern opens at most (3)
#   -r, --refs N       the highest group number a reference names (4)
#   -s, --subject STR  the subject every pattern is paired with ("aaaa")

require 'optparse'

opts = { groups: 3, refs: 4, subject: "aaaa" }
OptionParser.new do |o|
  o.banner = "usage: ruby #{$0} [options] > cases.txt"
  o.on("-g", "--groups N", Integer) { |v| opts[:groups] = v }
  o.on("-r", "--refs N", Integer) { |v| opts[:refs] = v }
  o.on("-s", "--subject STR") { |v| opts[:subject] = v }
end.parse!
abort "a sequence holds no fewer than no groups" if opts[:groups] < 0
abort "a reference names no group below group 0" if opts[:refs] < 0
abort "the subject cannot hold a tab or a newline" if opts[:subject] =~ /[\t\n\r]/

# Each form as the text before the reference a case may write inside it and the
# text after, so that a sequence is the forms joined and the reference goes
# into whichever of them it stands in. `%d` numbers the names apart.
FORMS = [["(a", ")"], ["(?:a", ")"], ["(?<g%d>a", ")"], ["(?=(a", "))"]].freeze

# The number a reference names is written past what any group here carries and
# past what the parser holds a number in: RE_MAX_BACKREF_NUM is where a number
# stops being one that names no group and starts being too big, and both sides
# of that line are drawn.
BOUND = 2147483647

# The most capture groups this gem opens: RE_MAX_CAPTURES less the whole
# match. A reference above it names no group however many the pattern goes on
# to open, which is why the parser holds such a number there rather than
# letting it grow, and a pattern that opens more is refused whatever it says.
MAX_GROUPS = 31

refs = []
0.upto(opts[:refs]) do |n|
  # `\0` is a NUL rather than a reference, and `\10` names group 10 or is an
  # octal escape, so the bare spelling is drawn for the digits that are only
  # ever a reference.
  refs << "\\#{n}" if n.between?(1, 9)
  refs << "\\k<#{n}>" << "\\k'#{n}'" << "\\k<-#{n}>" << "\\k'-#{n}'"
  # A subexpression call resolves as the reference does and is refused where
  # one is, with lines of its own: `\g<0>` is the whole pattern where `\k<0>`
  # names no group, `\g<+n>` counts forward where `\k` has no such form, and
  # a call that re-enters the group it stands in may be `never ending
  # recursion`. Those rows differ from a backreference's by design; whether
  # each engine draws the lines in the same place is what the corpus asks.
  refs << "\\g<#{n}>" << "\\g'#{n}'" << "\\g<-#{n}>" << "\\g'-#{n}'" << "\\g<+#{n}>"
end
[BOUND, BOUND + 1].each { |n| refs << "\\k<#{n}>" << "\\k<-#{n}>" << "\\g<#{n}>" << "\\g<-#{n}>" }
# The last two numbers a group can carry and the two above them, which no
# pattern reaches however long it is.
(MAX_GROUPS - 1).upto(MAX_GROUPS + 2) { |n| refs << "\\k<#{n}>" << "\\k<-#{n}>" << "\\g<#{n}>" << "\\g<-#{n}>" }
1.upto(opts[:groups] + 1) { |n| refs << "\\k<g#{n}>" << "\\g<g#{n}>" }

seqs = [[]]
1.upto(opts[:groups]) { |len| seqs.concat(FORMS.repeated_permutation(len).to_a) }
# Sequences long enough for those numbers to name a group, and one past the
# ceiling. The permutations above stay short, so these are runs of the plain
# form rather than a walk over the four.
(MAX_GROUPS - 1).upto(MAX_GROUPS + 1) { |len| seqs << [FORMS[0]] * len }

cases = []
seqs.each do |seq|
  named = 0
  # A name is written once, so the named groups in a sequence are numbered
  # apart as they are opened; the references above name g1 and up in the same
  # order.
  parts = seq.map do |open, close|
    if open.include?("%d")
      named += 1
      [open % named, close]
    else
      [open, close]
    end
  end
  body = parts.map { |open, close| open + close }.join
  refs.each do |ref|
    cases << body + ref
    cases << ref + body
    unless parts.empty?
      head = parts[0..-2].map { |open, close| open + close }.join
      cases << head + parts[-1][0] + ref + parts[-1][1]
    end
  end
end

cases.uniq.each { |pat| puts "#{pat}\t#{opts[:subject]}" }
