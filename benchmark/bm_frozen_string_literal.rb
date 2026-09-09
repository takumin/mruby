# frozen_string_literal: true
# String literals reached over and over from the same source position. The
# first two phases isolate the two allocation shapes: a short literal fits in
# the embedded area of `struct RString`, a long one needs a separate buffer.
# The last two phases spend the string as soon as they build it, as a hash key
# and as the right-hand side of `==`, which is where literals turn up most in
# real code and where nothing keeps the result alive afterwards.

REPEAT = 10_000_000

short = nil
i = 0
while i < REPEAT
  short = "a short literal"
  i += 1
end

long = nil
i = 0
while i < REPEAT
  long = "SELECT id, name, created_at FROM records WHERE owner = ? AND state IN (?, ?) AND created_at BETWEEN ? AND ? ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?"
  i += 1
end

table = {"created_at" => 1}
sum = 0
i = 0
while i < REPEAT
  sum += table["created_at"]
  i += 1
end

text = "created_at".dup
hits = 0
i = 0
while i < REPEAT
  hits += 1 if text == "created_at"
  i += 1
end

raise "unexpected short literal" unless short.size == 15
raise "unexpected long literal" unless long.size == 158
raise "unexpected sum" unless sum == REPEAT
raise "unexpected hits" unless hits == REPEAT
