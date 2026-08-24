# Only compiled into mrbtest when the build classifies characters by Unicode,
# which is where a build reading its strings as characters stands unless it
# says otherwise; see the gem's mrbgem.rake. Classifying by ASCII there is no
# table to read a general category or a script off, and every escape below
# that names one raises instead; ascii_prop.rb asserts that.

assert("Regexp - a property escape names a general category") do
  # Every character here lies above ASCII, so it is a character to classify
  # only where the pattern and the subject are read as characters.
  skip unless __ENCODING__ == "UTF-8"

  # The issue this answers: `\p{L}` was the letters `p{L}`, so a pattern
  # asking for a letter of any script matched the text of the request and not
  # the letter.
  assert_equal 0, ("Ā" =~ /\p{L}/)
  assert_equal 0, ("漢" =~ /\p{L}/)
  assert_nil ("１" =~ /\p{L}/)

  # A concrete category holds what it is named for and nothing beside it.
  members = {
    "Lu" => ["Ā", "Α", "Б"],
    "Ll" => ["ā", "α", "б"],
    "Lt" => ["ǅ", "ǈ"],
    "Lm" => ["ʰ", "ー"],
    "Lo" => ["あ", "漢", "א"],
    "Mn" => ["\u{300}", "\u{5B0}"],
    "Mc" => ["\u{903}"],
    "Me" => ["\u{488}"],
    "Nd" => ["１", "٣"],
    "Nl" => ["Ⅷ", "\u{3007}"],
    "No" => ["²", "½"],
    "Pc" => ["‿"],
    "Pd" => ["—"],
    "Ps" => ["「", "（"],
    "Pe" => ["」", "）"],
    "Pi" => ["«", "\u{2018}"],
    "Pf" => ["»", "\u{2019}"],
    "Po" => ["、", "。"],
    "Sm" => ["±", "≠"],
    "Sc" => ["€", "￥"],
    "Sk" => ["\u{2C2}", "\u{FF3E}"],
    "So" => ["©", "Ⓐ"],
    "Zs" => ["\u{A0}", "\u{2003}"],
    "Zl" => ["\u{2028}"],
    "Zp" => ["\u{2029}"],
    "Cc" => ["\u{85}", "\u{9F}"],
    "Cf" => ["\u{AD}", "\u{200D}"],
    "Co" => ["\u{E000}"],
    "Cn" => ["\u{378}", "\u{10FFFF}"],
  }
  members.each do |name, chars|
    re = Regexp.new("\\p{#{name}}")
    neg = Regexp.new("\\P{#{name}}")
    chars.each do |ch|
      assert_true re.match?(ch), "\\p{#{name}} holds #{ch.inspect}"
      assert_false neg.match?(ch), "\\P{#{name}} does not hold #{ch.inspect}"
    end
    # No two concrete categories share a character, so one category's members
    # belong to no other.
    members.each do |other, _|
      next if other == name
      chars.each do |ch|
        assert_false Regexp.new("\\p{#{other}}").match?(ch),
                     "\\p{#{other}} does not hold #{ch.inspect}"
      end
    end
  end

  # A one letter name is every category starting with that letter, and `LC`
  # the three cased ones.
  assert_true /\p{L}/.match?("漢")
  assert_true /\p{M}/.match?("\u{300}")
  assert_true /\p{N}/.match?("½")
  assert_true /\p{P}/.match?("、")
  assert_true /\p{S}/.match?("€")
  assert_true /\p{Z}/.match?("\u{2028}")
  assert_true /\p{C}/.match?("\u{AD}")
  assert_false /\p{L}/.match?("\u{300}")
  assert_true /\p{LC}/.match?("ǅ")
  assert_false /\p{LC}/.match?("ʰ")

  # `Any` is every character, `Assigned` every one that has a category, and
  # `Cn` the rest. `\P{Any}` holds nothing at all.
  assert_true /\p{Any}/.match?("漢")
  assert_true /\p{Any}/.match?("a")
  assert_false /\P{Any}/.match?("a")
  assert_true /\p{Assigned}/.match?("漢")
  assert_false /\p{Assigned}/.match?("\u{378}")
  assert_true /\P{Assigned}/.match?("\u{378}")

  # The long name and the short one are the same name, and so is either with
  # its separators written differently or left out.
  ["Lu", "Uppercase_Letter", "uppercase letter", "UPPERCASELETTER"].each do |name|
    assert_true Regexp.new("\\p{#{name}}").match?("Ā"), name
  end
  assert_true /\p{Letter}/.match?("漢")
  assert_true /\p{Cased_Letter}/.match?("ǅ")
  assert_true /\p{Unassigned}/.match?("\u{378}")
end

assert("Regexp - a property escape names a script") do
  skip unless __ENCODING__ == "UTF-8"

  {
    "Han" => "漢",
    "Hiragana" => "あ",
    "Katakana" => "ア",
    "Latin" => "A",
    "Greek" => "α",
    "Cyrillic" => "б",
    "Hebrew" => "א",
    "Canadian_Aboriginal" => "\u{1401}",
    "Egyptian_Hieroglyphs" => "\u{13000}",
  }.each do |name, ch|
    assert_true Regexp.new("\\p{#{name}}").match?(ch), "\\p{#{name}} holds #{ch.inspect}"
    assert_false Regexp.new("\\p{#{name}}").match?("漢" == ch ? "あ" : "漢"),
                 "\\p{#{name}} holds one script"
  end

  # The four letter name the database gives a script is a name for it too, and
  # so is a further alias where it lists one.
  assert_true /\p{Latn}/.match?("A")
  assert_true /\p{Hira}/.match?("あ")
  assert_true /\p{Zyyy}/.match?("1")
  assert_true /\p{Qaai}/.match?("\u{300}")

  # The two scripts that are not a writing system of their own: what any
  # script may use, and what takes the script of what it follows.
  assert_true /\p{Common}/.match?("1")
  assert_true /\p{Common}/.match?(" ")
  assert_true /\p{Inherited}/.match?("\u{300}")

  # A codepoint no script claims is Unknown, which is what an unassigned one
  # is; the name Zzzz is the same script.
  assert_true /\p{Unknown}/.match?("\u{378}")
  assert_true /\p{Zzzz}/.match?("\u{378}")
  assert_false /\p{Unknown}/.match?("漢")

  # A script is a set like any other, so it quantifies and it joins a class.
  assert_equal "漢字", "a漢字b".match(/\p{Han}+/)[0]
  assert_equal "あ漢", "あ漢b"[/[\p{Han}\p{Hiragana}]+/]
end

assert("Regexp - a property escape a build cannot answer is refused") do
  # The binary properties beyond the ones a POSIX bracket names are not
  # carried: each is a range list of its own and there are some ninety of
  # them. CRuby compiles every one of these, so the complaint says the name
  # rather than the build.
  ["Math", "Emoji", "Dash", "Hex_Digit", "Default_Ignorable_Code_Point"].each do |name|
    assert_raise_with_message(RegexpError,
                              "invalid character property name {#{name}}: /\\p{#{name}}/",
                              name) do
      Regexp.new("\\p{#{name}}")
    end
  end
  assert_raise_with_message(RegexpError,
                            "invalid character property name {}: /\\p{}/") do
    Regexp.new("\\p{}")
  end
end

assert("Regexp - a property escape negates as a member or as the class") do
  skip unless __ENCODING__ == "UTF-8"

  # Without /i the two are one set: a character is in the class when it is not
  # an X, whichever side carries the negation.
  ["\\P{Lu}", "[\\P{Lu}]", "[^\\p{Lu}]", "\\p{^Lu}"].each do |src|
    re = Regexp.new(src)
    assert_true re.match?("ā"), src
    assert_false re.match?("Ā"), src
    assert_true re.match?("1"), src
  end

  # Under /i they part company, as they do in CRuby. A class is closed under
  # folding and then negated, so `\P{Lu}` holds what no case of the character
  # is an uppercase letter for -- neither "ā" nor "Ā" -- where the member in
  # `[\P{Lu}]` holds what some case of it is not one for, which is both.
  outside = Regexp.new("\\P{Lu}", Regexp::IGNORECASE)
  inside = Regexp.new("[\\P{Lu}]", Regexp::IGNORECASE)
  assert_false outside.match?("Ā")
  assert_false outside.match?("ā")
  assert_true inside.match?("Ā")
  assert_true inside.match?("ā")
  # A character with no case at all is in both.
  assert_true outside.match?("漢")
  assert_true inside.match?("漢")
end

assert("Regexp - /i asks a property about every case of the character") do
  skip unless __ENCODING__ == "UTF-8"

  # A property under /i is asked of the character and of every character
  # sharing its folding, which is what makes `\p{Lu}` under /i hold a lower
  # case letter.
  upper = Regexp.new("\\p{Lu}", Regexp::IGNORECASE)
  assert_true upper.match?("ā")
  assert_true upper.match?("a")
  assert_false upper.match?("1")
  assert_false upper.match?("漢")

  lower = Regexp.new("\\p{Ll}", Regexp::IGNORECASE)
  assert_true lower.match?("Ā")
  assert_true lower.match?("A")

  # A script is asked the same way, so the Kelvin sign is Latin under /i
  # through the 'k' it folds with.
  assert_true Regexp.new("\\p{Latin}", Regexp::IGNORECASE).match?("\u{212A}")
  assert_true Regexp.new("\\p{Greek}", Regexp::IGNORECASE).match?("Α")
end

assert("Regexp - a property escape holds its characters above ASCII") do
  skip unless __ENCODING__ == "UTF-8"

  # The names a POSIX bracket names too hold above ASCII what the bracket
  # holds; unicode_ctype.rb pins the bracket, and this pins that the escape is
  # the same question.
  %w[alpha alnum blank cntrl digit graph lower print space upper word].each do |name|
    escape = Regexp.new("\\p{#{name}}")
    bracket = Regexp.new("[[:#{name}:]]")
    ["あ", "Ā", "ā", "１", "\u{A0}", "\u{85}", "\u{300}", "€", "\u{200D}"].each do |ch|
      assert_equal bracket.match?(ch), escape.match?(ch),
                   "\\p{#{name}} and [[:#{name}:]] agree on #{ch.inspect}"
    end
  end

  # `punct` is the one name where the two differ, and only over ASCII: the
  # bracket takes the nine ASCII symbols Onigmo gives it, where the escape is
  # the punctuation categories alone. Above ASCII they are one set.
  assert_true /[[:punct:]]/.match?("$")
  assert_false /\p{Punct}/.match?("$")
  assert_true /\p{Punct}/.match?("!")
  assert_true /\p{Punct}/.match?("、")
  assert_true /\p{S}/.match?("$")

  # `xdigit` and `ascii` are sets ASCII defines, so neither holds anything
  # above it and the negation of each holds everything there.
  assert_false /\p{XDigit}/.match?("ｆ")
  assert_false /\p{ASCII}/.match?("Ā")
  assert_true /\P{ASCII}/.match?("Ā")
end

assert("Regexp - a property escape joins a class and a match") do
  skip unless __ENCODING__ == "UTF-8"

  # A class holding several properties holds their union, and one written
  # twice is one question.
  re = /[\p{Hiragana}\p{Nd}]+/
  assert_equal "あい１", "aあい１b"[re]
  assert_equal "あ", "あ"[/[\p{Hiragana}\p{Hiragana}]/]

  # A property beside a literal, and a property under a quantifier, are atoms
  # like any other.
  assert_equal "aĀb", "aĀb"[/a\p{L}b/]
  assert_equal "Aa", "Aa"[/\p{Lu}\p{Ll}/]
  assert_equal "漢字", "1漢字"[/\p{L}{2}/]
  assert_nil "1"[/\p{L}*x/]

  # A negated class holding a property, and a property beside a range.
  assert_equal "1", "Ā1"[/[^\p{L}]/]
  assert_equal "x", "Āx"[/[\p{Nd}x]/]
end

assert("Regexp - a byte has no property to be asked about") do
  skip unless Object.const_defined?(:Encoding)

  # A byte-read subject holds bytes rather than characters, and a byte that
  # is no character belongs to no general category and no script. So it is in
  # a class through a negated property and not through a positive one, which
  # is how it reads a POSIX bracket too. Left to the codepoint of the same
  # number, the byte 0xB5 would be the letter µ rather than the byte it is.
  bin = "\xB5".dup.force_encoding("ASCII-8BIT")
  assert_nil (bin =~ /\p{L}/)
  assert_nil (bin =~ /[\p{L}]/)
  assert_equal 0, (bin =~ /\P{L}/)
  assert_equal 0, (bin =~ /[\P{L}]/)
  assert_equal 0, (bin =~ /\p{Any}/)
  assert_nil (bin =~ /[\p{Han}\p{Nd}]/)
end
