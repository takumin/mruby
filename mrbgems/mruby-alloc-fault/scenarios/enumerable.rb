# The Enumerable methods build a result as they walk, so a refusal lands
# with a half-built accumulator that the caller never sees.
src = (1..300).to_a
begin
  src.map { |i| i.to_s }.select { |s| s.size > 1 }.sort_by { |s| s.reverse }
rescue NoMemoryError
end
raise "the source was disturbed" unless src.size == 300 && src[0] == 1

begin
  src.group_by { |i| i % 7 }
rescue NoMemoryError
end

begin
  src.each_slice(9).to_a
  src.each_cons(3).to_a
rescue NoMemoryError
end

begin
  src.reduce([]) { |acc, i| acc << [i, i.to_s] }
rescue NoMemoryError
end

begin
  src.zip(src.reverse, src.map { |i| -i })
rescue NoMemoryError
end
raise "the source was disturbed by zip" unless src.size == 300
