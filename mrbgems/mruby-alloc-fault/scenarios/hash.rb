# Hashes: the table behind a Hash is a plain allocation that is rebuilt as
# the hash grows, and a rebuild moves every entry.
h = {}
begin
  400.times { |i| h["key-#{i}"] = i }
rescue NoMemoryError
end
raise "the hash lost the entries it had" if h.size > 0 && h["key-0"] != 0

base = {}
64.times { |i| base[i] = i.to_s }
begin
  10.times { |i| base.merge("m#{i}" => i * 2) }
rescue NoMemoryError
end
raise "Hash#merge disturbed the receiver" unless base.size == 64 && base[0] == "0"

begin
  5.times { base.dup.each { |k, v| base.dup[k] = v } }
rescue NoMemoryError
end
raise "Hash#dup disturbed the receiver" unless base.size == 64

begin
  d = base.dup
  d.keys.each { |k| d.delete(k) }
  raise "delete left entries behind" unless d.size == 0
rescue NoMemoryError
end
raise "the original hash was disturbed" unless base.size == 64
