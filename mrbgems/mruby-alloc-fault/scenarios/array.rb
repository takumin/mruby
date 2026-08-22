# Arrays: the element buffer is a plain allocation that is resized as the
# array grows, and a resize is the case gc.c refuses to drive a collection
# from, because the caller has not stored the new pointer yet.
a = [1, 2, 3]
begin
  800.times { |i| a.push(i) }
rescue NoMemoryError
end
raise "Array#push left a broken receiver" unless a.size >= 3 && a[0] == 1 && a[2] == 3

b = (1..50).to_a
begin
  8.times { b.concat(b) }
rescue NoMemoryError
end
raise "Array#concat left a broken receiver" unless b.size >= 50 && b[0] == 1

c = (1..200).map { |i| (i * 7919) % 211 }
begin
  8.times { c.sort.uniq.reverse.map { |v| [v, v] }.flatten }
rescue NoMemoryError
end
raise "the source array was disturbed" unless c.size == 200

d = [[1, [2, [3, [4]]]]] * 32
begin
  d.flatten
  d.flatten.each_slice(3).to_a
rescue NoMemoryError
end
raise "the nested array was disturbed" unless d.size == 32

e = (1..100).to_a
begin
  10.times { e.dup.shift(10) && e.dup.unshift(*e) }
rescue NoMemoryError
end
raise "unshift disturbed the receiver" unless e.size == 100 && e.first == 1
