# Repeated lookups of the same keys in the same hashes.
#
# Every `[]` and `[]=` here searches the entry from scratch: the hash code of
# the key is recomputed and the index buckets (or the entry array) are probed
# again. The three phases cover the shapes that search cost shows up in:
# string keys (hashing walks the whole string), symbol keys (hashing is free,
# probing is not), and read-modify-write (one search for the read and another
# for the write).

REPEAT = 300

STR_KEYS = (0...64).map { |i| "key#{i}" }
SYM_KEYS = STR_KEYS.map { |s| s.to_sym }

str_hash = {}
STR_KEYS.each_with_index { |k, i| str_hash[k] = i }

sym_hash = {}
SYM_KEYS.each_with_index { |k, i| sym_hash[k] = i }

sum = 0

REPEAT.times do
  100.times do
    STR_KEYS.each { |k| sum += str_hash[k] }
  end
end

REPEAT.times do
  100.times do
    SYM_KEYS.each { |k| sum += sym_hash[k] }
  end
end

counts = {}
STR_KEYS.each { |k| counts[k] = 0 }

REPEAT.times do
  100.times do
    STR_KEYS.each { |k| counts[k] += 1 }
  end
end

raise "unexpected sum" unless sum == 2 * REPEAT * 100 * 2016
raise "unexpected count" unless counts[STR_KEYS[0]] == REPEAT * 100
