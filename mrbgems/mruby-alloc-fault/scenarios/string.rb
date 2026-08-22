# Strings: the buffer behind a String is a plain allocation, so growing one,
# copying one and formatting into one all reach the allocator directly.
#
# Each part checks, after the refusal, that what it was working on is still
# the value it was: an operation that gives up halfway must not leave a
# String claiming a length its buffer does not have.
s = "seed"
begin
  80.times { s += "0123456789" }
rescue NoMemoryError
end
raise "String#+ left a broken receiver" unless s.start_with?("seed") && s.size >= 4

buf = "x" * 64
begin
  10.times { buf << buf }
rescue NoMemoryError
end
raise "String#<< left a broken receiver" unless buf.size >= 64 && buf[0] == "x"

parts = %w[alpha beta gamma delta]
begin
  200.times { |i| parts.join("-#{i}-").upcase.downcase.reverse }
rescue NoMemoryError
end
raise "the parts were disturbed" unless parts.size == 4 && parts[0] == "alpha"

fmt = "seed"
begin
  20.times { |i| fmt = sprintf("%s %d %s", fmt[0, 32], i, "pad" * i) }
rescue NoMemoryError
end
raise "sprintf left a broken String" unless fmt.size > 0

sub = "the quick brown fox" * 8
begin
  15.times { sub.sub("quick", "slow" * 16).gsub("o", "0") }
rescue NoMemoryError
end
raise "the subject was disturbed" unless sub.start_with?("the quick")
