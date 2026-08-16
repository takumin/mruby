# What a match answers, and where it says the match is.
#
# A match position follows the same rule the rest of the string methods do:
# it counts characters where MRB_UTF8_STRING is on and bytes where it is off,
# so the cases over a non-ASCII subject carry both answers. A case that wants
# a MatchData asks it for an array rather than returning it, since a byte
# answer has to be something a case file can spell.

group 'where the match is', needs: :regexp do
  check '"あいう" =~ /い/', bytes: 3
  check '"あいう".index(/う/)', bytes: 6
  check '"あいう".match("い").begin(0)', bytes: 3
  check '"あいう".match("い").end(0)', bytes: 6
  check 'm = "あいう".match("い"); [m[0], m.pre_match, m.post_match]'
  check '"あいう".match("え")'
end

group 'what a MatchData holds', needs: :regexp do
  check '"hello world".match(/(\w+)\s(\w+)/)',
        note: 'an ASCII subject, so the offsets are the same under either model'
  check '"hello".match(/(x)?(l)/).captures'
  check '"hello".match(/(x)?(l)/).begin(1)'
  check 'm = "John Smith".match(/(?<first>\w+) (?<last>\w+)/); [m[:first], m[:last]]'
  check '"hello".match(/l/, 3)[0]'
end

group 'the pieces a match cuts out', needs: :regexp do
  check '"あいう".split(//)', bytes: ["\xE3", "\x81", "\x82", "\xE3", "\x81", "\x84", "\xE3", "\x81", "\x86"]
  check '"a1b22c".scan(/\d+/)'
  check '"a1b22c".gsub(/\d+/) { |d| "<" + d + ">" }'
  check '"あいう".sub(/い/, "X")'
  check '"あいう".gsub(/./) { |c| c }', bytes: 'あいう'
  check '"a,b,,c".split(",", -1)'
end

group 'case is ASCII unless the build says otherwise', needs: :regexp do
  check '"ABC" =~ /abc/i'
  check '"ÄÖÜ" =~ /äöü/i', bytes: nil, needs: :regexp_unicode_case,
        note: 'only a build that carries the case foldings folds above ASCII'
  check 'Regexp.new("straße", Regexp::IGNORECASE) =~ "STRASSE"',
        want: nil, needs: :regexp_unicode_case,
        note: 'CRuby folds "ß" to "ss" here and answers 0; mruby folds 1:1 and does not match'
end

group 'a pattern the engine has to refuse or answer', needs: :regexp do
  check 'Regexp.new("(").source rescue "refused"',
        want: 'refused',
        note: 'CRuby and mruby word the refusal differently; that it refuses is the case'
  check '"あいう".match("\xC3")',
        want: nil,
        note: 'CRuby refuses a pattern that spells no character; mruby reads it as bytes and finds nothing'
  check 'Regexp.new("あ").source'
end
