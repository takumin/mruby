# Generate the cases for a differential run over the backreferences a pattern
# may make, the ones it may not among them: every sequence of groups up to a
# length, each paired with every spelling of a numbered, relative and named
# reference, written after the groups, before them, and inside the last of
# them.
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

refs = []
0.upto(opts[:refs]) do |n|
  # `\0` is a NUL rather than a reference, and `\10` names group 10 or is an
  # octal escape, so the bare spelling is drawn for the digits that are only
  # ever a reference.
  refs << "\\#{n}" if n.between?(1, 9)
  refs << "\\k<#{n}>" << "\\k'#{n}'" << "\\k<-#{n}>" << "\\k'-#{n}'"
end
[BOUND, BOUND + 1].each { |n| refs << "\\k<#{n}>" << "\\k<-#{n}>" }
1.upto(opts[:groups] + 1) { |n| refs << "\\k<g#{n}>" }

seqs = [[]]
1.upto(opts[:groups]) { |len| seqs.concat(FORMS.repeated_permutation(len).to_a) }

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
