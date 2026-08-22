# The regexp engine allocates the compiled program, the capture buffers and
# every string it hands back.
subject = "the quick brown fox jumps over the lazy dog " * 2
begin
  8.times do |i|
    re = Regexp.new("(qu[a-z]+)\\s+(\\w{#{(i % 3) + 3},})")
    m = re.match(subject)
    m && m[1] + m[2]
  end
rescue NoMemoryError
rescue RegexpError
end
raise "the subject was disturbed" unless subject.start_with?("the quick")

begin
  5.times { subject.gsub(/o(\w)/) { "0#{$1.upcase}" } }
rescue NoMemoryError
end
raise "gsub disturbed the receiver" unless subject.start_with?("the quick")

begin
  5.times { subject.scan(/\b\w{4}\b/) }
rescue NoMemoryError
end

begin
  5.times { subject.split(/\s+/).map { |w| w =~ /^[a-z]+$/ ? w : nil } }
rescue NoMemoryError
end
