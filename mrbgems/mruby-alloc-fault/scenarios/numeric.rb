# Numbers that do not fit a word: a big integer carries a limb array, and
# converting one to a String builds the digits in an allocation of its own.
big = 2 ** 300
begin
  100.times { |i| (big + i) * (big - i) }
rescue NoMemoryError
end
raise "the big integer was disturbed" unless big == 2 ** 300

begin
  20.times { |i| (big ** 2).to_s.size }
rescue NoMemoryError
end

begin
  200.times { |i| (i * 3.14159).to_s + (i / 7.0).round(3).to_s }
rescue NoMemoryError
rescue NoMethodError
end

begin
  100.times { |i| Integer("0x" + (i + 1).to_s(16), 16) }
rescue NoMemoryError
rescue ArgumentError
end
