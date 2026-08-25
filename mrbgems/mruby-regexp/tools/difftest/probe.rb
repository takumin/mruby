# What one engine makes of the pattern corpus, as a line per pattern.
#
# Runs under CRuby and under an mruby built with this gem, and is written to
# the intersection of the two: no require, no stdlib beyond what mruby's core
# gems carry, and nothing that reads a string as anything but bytes. compare.rb
# runs it in each engine and diffs the two outputs; a line that differs is a
# pattern the two engines do not agree about.
#
# A line is
#
#   <pattern>/<flags>  <match signature>  <capture signature>
#
# with the pattern rendered ASCII-safe, so that a subject or a pattern holding
# a byte that spells no character still prints the same in both engines. The
# signatures hold one field per subject, in the order SUBJECTS lists them, so
# a difference in any one of them shows up as a difference in the line.

# ---------------------------------------------------------------- rendering

PRINTABLE = {}
(0x20..0x7e).each { |b| PRINTABLE[b] = true }

# A string as ASCII, every byte that is not printable ASCII spelled \xNN. Both
# engines walk the same bytes and write the same digits, which a subject read
# as characters in one engine and as bytes in the other would not.
def render(str)
  out = ""
  i = 0
  while i < str.bytesize
    b = str.getbyte(i)
    if b == 0x5c
      out << "\\\\"   # doubled, so that `\x5c` in a pattern cannot be read
    elsif PRINTABLE[b] # as the rendering of a byte that is not a backslash
      out << b.chr
    else
      h = b.to_s(16)
      h = "0" + h if h.size < 2
      out << "\\x" << h
    end
    i += 1
  end
  out
end

# ----------------------------------------------------------------- subjects

# What every pattern is asked about. Kept small, since each one costs a field
# in every line, and stable: a character whose Unicode changed release to
# release would make the corpus disagree about the database rather than about
# the engine. Each is spelled by codepoint or by byte so the file itself is
# ASCII.
SUBJECTS = [
  "",
  "a",
  "A",
  "ab",
  "abc",
  "aa",
  "a1",
  "1",
  "_",
  "-",
  " ",
  "\t",
  "\n",
  "a\nb",
  "\u{100}",      # LATIN CAPITAL LETTER A WITH MACRON
  "\u{101}",      # its lower case
  "\u{3042}",     # HIRAGANA LETTER A
  "\u{6f22}",     # CJK IDEOGRAPH "kan"
  "\u{ff11}",     # FULLWIDTH DIGIT ONE
  "a\u{301}",     # a and a combining acute
  "\u{212a}",     # KELVIN SIGN, which folds to 'k'
  "\xb5",         # a byte that starts no character
  # The runs that make an escape discriminating: `\!` and `!` agree on every
  # subject holding no `!`, so a subject holding each of the punctuation, the
  # digits and both cases is what asks the question at all.
  "!\"\#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
  "0123456789",
  "xyzXYZ",
]

# ----------------------------------------------------------------- the corpus

# Each entry is [pattern, flags], flags out of "imx". The corpus is built
# rather than listed wherever an axis has a shape to walk, so that adding a
# case to an axis is adding it once.

ASCII_CHARS = (0x21..0x7e).map { |b| b.chr }

def escape_patterns
  out = []
  ASCII_CHARS.each do |c|
    # An escape means what it means in four places: on its own, beside a
    # literal, inside a class, and as an end of a range in one.
    out << "\\" + c
    out << "a\\" + c
    out << "[\\" + c + "]"
    out << "[a-\\" + c + "]"
    out << "[\\" + c + "-z]"
  end
  # The escapes that carry a name or a number after them.
  %w[p P k g u x c C M o N].each do |c|
    out << "\\" + c + "{61}"
    out << "\\" + c + "<x>"
    out << "\\" + c + "1"
    out << "[\\" + c + "{61}]"
  end
  out
end

def class_patterns
  fixed = [
    "[]", "[^]", "[a]", "[^a]", "[ab]", "[a-c]", "[c-a]", "[-a]", "[a-]",
    "[]a]", "[^]a]", "[a\\]b]", "[[]", "[[a]", "[a[]b]", "[[.a.]]", "[[=a=]]",
    "[a&&b]", "[a&&]", "[&&a]", "[\\w&&\\d]", "[^a&&b]",
    "[\\d]", "[\\D]", "[\\w]", "[\\W]", "[\\s]", "[\\S]", "[\\h]", "[\\H]",
    "[\\d-z]", "[a-\\d]", "[\\d-]", "[-\\d]",
    "[\\x41]", "[\\x41-\\x43]", "[\\101]", "[\\u0041]", "[\\u{41 42}]",
    "[\\u{41}-\\u{43}]", "[\\x80]", "[\\x80-\\xbf]",
    "[a-\\u{100}]", "[\\u{100}-\\u{200}]", "[^\\u{100}]",
    "[\\n]", "[\\t]", "[\\b]", "[\\a]", "[\\e]", "[\\cA]", "[\\C-A]", "[\\M-A]",
    "[^^]", "[\\^]", "[$]", "[.]", "[*]", "[+]", "[?]", "[(]", "[)]",
    "[{]", "[}]", "[|]", "[\\\\]", "[/]",
  ]
  posix = []
  %w[alpha digit alnum upper lower space blank xdigit word cntrl print graph
     ascii punct bogus].each do |name|
    posix << "[[:" + name + ":]]"
    posix << "[[:^" + name + ":]]"
    posix << "[^[:" + name + ":]]"
    posix << "[[:" + name + ":]a]"
  end
  # A bracket that never closes, and one whose name does not.
  posix += ["[[:alpha]", "[[:alpha:", "[[:", "[[:alpha:]", "[:alpha:]"]
  fixed + posix
end

def quantifier_patterns
  atoms = ["a", "\\d", "[ab]", "(a)", "(?:ab)", "(?<n>a)", "\\p{L}", ".",
           "\\b", "^", "(?=a)", "(?>a)", "\\u{41}", "\\1"]
  quants = ["*", "+", "?", "{2}", "{1,2}", "{2,}", "{0}", "{,2}", "{2,1}",
            "*?", "+?", "??", "{1,2}?", "{2}?", "*+", "++", "?+", "{1,2}+",
            "**", "*{2}", "{2}{3}", "{", "{a}", "{1", "{1,", "}"]
  out = []
  atoms.each { |a| quants.each { |q| out << a + q } }
  out
end

def group_patterns
  [
    "(a)", "(?:a)", "(?<n>a)", "(?'n'a)", "(?<>a)", "(?<1>a)", "(?<n)a)",
    "(?#c)a", "(?#c", "a(?#c)*", "(?#a(?#b))",
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(?<=a*)", "(?<=ab|c)",
    "(?>a)", "(?>a*)b", "(?>a|b)",
    "(?i)a", "(?i:a)", "(?-i)a", "(?im-x:a)", "(?x)a b", "(?x:a b)",
    "(?)a", "(?y)a", "(?<n>a)(?<n>b)",
    "(", ")", "(a", "a)", "()", "(|)", "(a|)", "(|a)",
    "(a)(b)", "((a))", "(?:(a))", "(a)|(b)",
    "(?<n>a)\\k<n>", "(?<n>a)\\k'n'", "(?<n>a)\\k<m>", "(a)\\g<1>",
    "(a)(?<n>b)", "\\1(a)", "(a)\\1", "(a)\\2",
  ]
end

def anchor_patterns
  ["^a", "a$", "\\Aa", "a\\z", "a\\Z", "\\ba", "a\\b", "\\Ba", "a\\B",
   "^", "$", "^$", "\\A\\z", "\\G", "\\Ga", "a\\K", "\\Kb", "\\R", "\\X",
   "^a$", "(?m:^a$)", "a\\Z\\z"]
end

def property_patterns
  names = %w[L Lu Ll Lt Lm Lo M N Nd P S Z C Cn LC Any Assigned
             Alpha Alnum Blank Cntrl Digit Graph Lower Print Punct Space
             Upper Word XDigit ASCII Alphabetic Uppercase Lowercase
             White_Space Latin Han Hiragana Common Inherited Unknown
             Latn Zyyy Zzzz Math Emoji Dash Bogus]
  out = []
  names.each do |n|
    out << "\\p{" + n + "}"
    out << "\\P{" + n + "}"
    out << "\\p{^" + n + "}"
    out << "[\\p{" + n + "}]"
    out << "[^\\p{" + n + "}]"
  end
  out += ["\\p{}", "\\p{^}", "\\p{", "\\p{L", "[\\p{L]", "\\p{ l a t i n }",
          "\\p{UPPERCASE_LETTER}", "\\p{uppercaseletter}", "\\pL", "\\p", "\\P"]
  out
end

def alternation_patterns
  ["a|b", "|a", "a|", "|", "a||b", "(a|b)|c", "a|b|c", "ab|cd",
   "\\p{L}|\\d", "[a]|[b]", "a*|b"]
end

# The axes overlap -- `\G` is an escape and an anchor, `\p{L}` an escape and a
# property -- and a pattern asked twice is a line written twice, which the
# comparison would read as one. Held to the first time each is named, in the
# order the axes give them.
seen = {}
PATTERNS = (escape_patterns + class_patterns + quantifier_patterns +
            group_patterns + anchor_patterns + property_patterns +
            alternation_patterns).select { |p| seen[p] ? false : (seen[p] = true) }

FLAGS = ["", "i", "x"]

# ------------------------------------------------------------------ running

def flag_value(flags)
  v = 0
  v |= Regexp::IGNORECASE if flags.include?("i")
  v |= Regexp::EXTENDED if flags.include?("x")
  v |= Regexp::MULTILINE if flags.include?("m")
  v
end

# What one search comes to, as the pair of fields it contributes.
#
# The first is where the match starts, one character wide: a digit for a start
# under ten, `+` for one at or past it, `.` for no match, `X` for a search that
# raised. What raised is not spelled -- the two engines are free to disagree
# about a message, and this asks whether they agree about the answer.
#
# The second is what the match captured: `begin-end` a group, `-` for a group
# that captured nothing, the groups joined by `,`. A subject that did not match
# leaves it empty.
def search(re, subject)
  md = re.match(subject)
  return [".", ""] unless md
  b = md.begin(0)
  out = []
  i = 0
  n = md.size
  while i < n
    gb = md.begin(i)
    out << (gb ? gb.to_s + "-" + md.end(i).to_s : "-")
    i += 1
  end
  [b < 10 ? b.to_s : "+", out.join(",")]
rescue StandardError
  ["X", "X"]
end

# Which build this is, in the two terms that decide what the engine can
# answer: whether a string is indexed by character, and whether a character
# above ASCII is classified by Unicode. A baseline taken against one build
# describes that build and no other -- without the tables a word boundary, a
# POSIX bracket and a property all answer differently, and every one of those
# would read as a regression against the wrong baseline. CRuby answers yes to
# both, which is why it is the side to compare against.
puts "#build\tchars=" + ("\u{100}".size == 1 ? "1" : "0") +
     " unicode=" + (Regexp.new("[[:alpha:]]").match?("\u{100}") ? "1" : "0")

# ------------------------------------------------------- the character axis
#
# The corpus above asks what the engine makes of a pattern; this asks what it
# makes of a character. The two want different shapes: a pattern is worth
# asking about a handful of subjects, where a character is worth asking every
# way there is to classify one. So a line here is a character and the answers
# are the columns, which is the other way round, and each is a bit.
#
# Both the characters and the property names come out of the database, by the
# rule tools/unicode/corpus_data.rb states; corpus.rb is what that rule wrote,
# and compare.rb puts it in front of this file.

# The codepoint as the bytes that spell it, built rather than asked for, so
# that both engines are handed the same bytes whatever their literals do.
#
# The bytes are then said to be UTF-8 where the build has anything to say it
# to. CRuby's `Integer#chr` hands back a binary string, and a binary subject
# is one CRuby's engine refuses a property escape against -- the answer would
# be about the encoding and not about the character. A build with no encodings
# reads the bytes the way it reads every string, which is the answer wanted
# there.
def utf8(cp)
  s = if cp < 0x80
        cp.chr
      elsif cp < 0x800
        (0xc0 | (cp >> 6)).chr + (0x80 | (cp & 0x3f)).chr
      elsif cp < 0x10000
        (0xe0 | (cp >> 12)).chr + (0x80 | ((cp >> 6) & 0x3f)).chr +
          (0x80 | (cp & 0x3f)).chr
      else
        (0xf0 | (cp >> 18)).chr + (0x80 | ((cp >> 12) & 0x3f)).chr +
          (0x80 | ((cp >> 6) & 0x3f)).chr + (0x80 | (cp & 0x3f)).chr
      end
  s = s.force_encoding("UTF-8") if s.respond_to?(:force_encoding)
  s
end

POSIX_NAMES = %w[alpha digit alnum upper lower space blank xdigit word cntrl
                 print graph ascii punct]

# Every way to ask what a character is, in the order the columns come. A
# pattern one engine will not compile is a column of `E`, which is an answer
# like any other and one the two can differ about.
def classifiers
  out = []
  CORPUS_PROPS.each do |name|
    out << ["\\p{" + name + "}", 0]
    out << ["\\P{" + name + "}", 0]
  end
  POSIX_NAMES.each do |name|
    out << ["[[:" + name + ":]]", 0]
    out << ["[[:^" + name + ":]]", 0]
  end
  %w[d D w W s S h H].each { |c| out << ["\\" + c, 0] }
  out << ["\\b", 0]
  out << ["\\B", 0]
  out << [".", 0]
  # The same questions under /i, which reads a character through every case
  # of it and is the one flag that changes what a class holds.
  ["\\p{Lu}", "\\p{Ll}", "[[:upper:]]", "[[:lower:]]", "[[:^upper:]]",
   "\\p{L}"].each { |src| out << [src, Regexp::IGNORECASE] }
  out
end

CLASSIFIERS = classifiers.map do |src, opt|
  begin
    [src, Regexp.new(src, opt)]
  rescue StandardError
    [src, nil]
  end
end

CORPUS.each do |cp|
  s = utf8(cp)
  row = ""
  CLASSIFIERS.each do |_, re|
    if re.nil?
      row << "E"
    else
      begin
        row << (re.match?(s) ? "1" : "0")
      rescue StandardError
        row << "X"
      end
    end
  end
  h = cp.to_s(16).upcase
  h = "0" + h while h.size < 4
  puts "#char U+" + h + "\t" + row + "\t"
end

PATTERNS.each do |src|
  FLAGS.each do |flags|
    label = render(src) + "/" + flags
    begin
      re = Regexp.new(src, flag_value(flags))
    rescue StandardError => e
      # A pattern one engine refuses is a line of its own, and the class is
      # part of the answer: refusing with RegexpError and refusing with
      # ArgumentError are different answers.
      puts label + "\tE:" + e.class.to_s + "\t"
      next
    end
    sig = ""
    caps = []
    SUBJECTS.each do |s|
      where, groups = search(re, s)
      sig << where
      caps << groups
    end
    puts label + "\t" + sig + "\t" + caps.join(";")
  end
end
