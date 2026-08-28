# Only compiled into mrbtest when the build classifies characters by ASCII,
# whether by MRB_USE_ASCII_CTYPE or by reading its strings as bytes; see the
# gem's mrbgem.rake. Where it classifies by Unicode there are tables to read a
# general category and a script off, and unicode_prop.rb asserts what they
# answer.
assert("Regexp - a property escape holds the ASCII of its name") do
  # The names a POSIX bracket names too are answered off the bracket types,
  # which every build carries, so the escape holds their ASCII here as the
  # bracket does. What neither holds is a character above ASCII: this build
  # has no table to say what one is, and a build reading its strings by byte
  # has no character to ask about.
  assert_equal "a", "1a"[/\p{Alpha}/]
  assert_equal "1", "a1"[/\p{Digit}/]
  assert_equal "_", "-_"[/\p{Word}/]
  assert_equal "f", "-f"[/\p{XDigit}/]
  assert_true /\p{ASCII}/.match?("a")
  assert_false /\p{ASCII}/.match?("Ā")
  assert_equal "!", "a!"[/\p{Punct}/]
  # The nine ASCII symbols are where `[[:punct:]]` and `\p{Punct}` part
  # company on every build: the bracket takes them and the property is the
  # punctuation categories alone.
  assert_equal "$", "a$"[/[[:punct:]]/]
  assert_nil "a$"[/\p{Punct}/]

  ["あ", "Ā", "１"].each do |ch|
    %w[Alpha Alnum Upper Lower Word Print Graph Space Digit].each do |name|
      assert_false Regexp.new("\\p{#{name}}").match?(ch),
                   "\\p{#{name}} does not hold #{ch.inspect}"
    end
  end
end

assert("Regexp - a general category or a script needs the Unicode tables") do
  # Each of these is a name CRuby compiles and this gem compiles where it
  # carries the data. Without the tables there is no answer for it, and no way
  # to tell a name that would have had one from a name that would not, so the
  # complaint names the build rather than the name.
  ["L", "Lu", "Han", "Latin", "Assigned", "Bogus"].each do |name|
    assert_raise_with_message(RegexpError,
                              "character property {#{name}} needs Unicode character data: " \
                              "/\\p{#{name}}/",
                              name) do
      Regexp.new("\\p{#{name}}")
    end
  end
  assert_raise(RegexpError) { Regexp.new("[\\p{Han}]") }

  # `Any` needs no table: it is every character there is, and its negation
  # holds nothing.
  assert_equal "a", "a"[/\p{Any}/]
  assert_nil "a"[/\P{Any}/]
end
