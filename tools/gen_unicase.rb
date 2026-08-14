# Generate the Unicode case mapping table from the data the host CRuby carries.
#
#   ruby tools/gen_unicase.rb src
#
# unicase.h holds what `String#downcase`, `#upcase`, `#capitalize`,
# `#swapcase` and `#casecmp?` answer for a character above ASCII, and is
# compiled only into a build that defines MRB_UTF8_STRING, which is the build
# that reads a string as characters rather than bytes. ASCII is folded inline
# by the callers and is not in the table.
#
# The mappings are the full ones, so a source can map to more than one
# character ("ß" upcases to "SS"). Those go in a second table beside the
# run-length encoded 1:1 ones, spelled as the UTF-8 bytes they produce.
#
# Title case is stored as its difference from upper case, which is 181 entries
# against the 1,479 it would take in full. The difference has to be able to say
# "this one does not change" as well: `U+10D0` upcases to `U+1C90` and
# capitalizes to itself, so a run of delta 0 stands for that.
#
# Swap case is stored the same way, against the rule that a character with a
# lower case swaps down and one without swaps up. What the rule misses is the
# title case characters, which CRuby swaps to something neither case spells:
# `U+01C5` upcases to `U+01C4` and downcases to `U+01C6`, and swaps to "dŽ".

require 'rbconfig'

outdir = ARGV[0] or abort "usage: #{$0} OUTDIR"

# ------------------------------------------------------------------- gather

lower = {}
upper = {}
title = {}
fold  = {}
swap_diff = {}

(0x80..0x10FFFF).each do |cp|
  next if cp.between?(0xD800, 0xDFFF)
  c = begin; cp.chr("UTF-8"); rescue RangeError; next; end
  l = c.downcase
  u = c.upcase
  t = c.capitalize          # one character capitalized is its title case
  f = c.downcase(:fold)
  lower[cp] = l if l != c
  upper[cp] = u if u != c
  title[cp] = t if t != c
  fold[cp] = f if f != c
  # What the swap rule would answer, against what swapping actually answers.
  s = c.swapcase
  swap_diff[cp] = s if s != (l != c ? l : u)
end

# Title case rides on upper case, so only what the two disagree about is
# stored. A source the two disagree about maps to itself under title case as
# often as it maps to something, and both have to be said.
title_diff = {}
(upper.keys | title.keys).each do |cp|
  next if upper[cp] == title[cp]
  title_diff[cp] = title[cp] || cp.chr("UTF-8")
end

# Simple case folding is the run table read on its own, so the runs have to
# hold what simple folding answers rather than the 1:1 half of the full one.
# The two differ over a source whose full folding spells several characters
# and which still has a single counterpart to fold to: U+1E9E folds to "ss"
# and lower cases to U+00DF, which folds to "ss" as well, so simple folding
# pairs the two and /ß/i reaching "ẞ" needs nothing wider. A source with no
# such counterpart (U+FB00 to "ff") stays out of the runs, which is exactly
# what simple folding leaves alone.
fold_extra = {}
fold.each do |cp, f|
  next if f.unpack("U*").size == 1
  c = cp.chr("UTF-8")
  lo = c.downcase
  fold_extra[cp] = lo if lo.length == 1 && lo != c && lo.downcase(:fold) == f
end

# ---------------------------------------------------------------- encode

# Run-length encode the 1:1 mappings: consecutive sources stepping by a fixed
# stride and sharing one delta collapse into a single entry. Stride is 1 or 2
# in practice, the latter being the interleaved upper/lower blocks.
def runs_of(map)
  pairs = map.select { |_, s| s.unpack("U*").size == 1 }
             .map { |cp, s| [cp, s.unpack("U*")[0]] }.sort
  runs = []
  pairs.each do |cp, to|
    d = to - cp
    r = runs.last
    if r && r[:delta] == d
      stride = cp - r[:last]
      if stride <= 2 && (r[:stride].nil? || r[:stride] == stride)
        r[:stride] ||= stride
        r[:count] += 1
        r[:last] = cp
        next
      end
    end
    runs << {start: cp, last: cp, count: 1, stride: nil, delta: d}
  end
  runs.each { |r| r[:stride] ||= 1 }
  runs
end

def multis_of(map)
  map.select { |_, s| s.unpack("U*").size > 1 }.sort.to_h
end

# One pool of UTF-8 bytes behind every multi character mapping. Identical
# spellings share an offset, which is what the three tables having "SS" and the
# like in common comes to.
pool = []
pool_at = {}
place = lambda do |str|
  bytes = str.bytes
  pool_at[bytes] ||= begin
    off = pool.size
    pool.concat(bytes)
    off
  end
end

# The runs and the multi table of one name may come from different maps, which
# is what tells simple folding from full folding.
TABLES = [
  ['lower', lower,                    lower,      'the lowercase mapping'],
  ['upper', upper,                    upper,      'the uppercase mapping'],
  ['title', title_diff,               title_diff, 'where title case differs from upper case'],
  ['swap',  swap_diff,                swap_diff,  'where swapping differs from the down-then-up rule'],
  ['fold',  fold.merge(fold_extra),   fold,       'the case folding, simple in the runs and full in the multi'],
]

encoded = TABLES.map do |name, run_map, multi_map, _|
  runs = runs_of(run_map)
  multi = multis_of(multi_map).map { |cp, s| [cp, place.call(s), s.bytesize] }
  [name, runs, multi]
end

widest = ([lower, upper, title, fold].flat_map { |m| m.values.map(&:bytesize) }).max

# ------------------------------------------------------------------- emit

def hex(cp) = "0x%05X" % cp

File.open(File.join(outdir, 'unicase.h'), 'w') do |out|
  out.puts <<~HEAD
    /*
    ** unicase.h - Unicode case mapping tables
    **
    ** Generated by tools/gen_unicase.rb from Unicode #{RbConfig::CONFIG['UNICODE_VERSION'] || 'data'}
    ** as carried by ruby #{RUBY_VERSION}. Do not edit by hand.
    **
    ** Sources below 128 are not in the tables: the callers fold ASCII inline.
    ** A source that maps to several characters is in the multi table beside
    ** each run table, spelled as the UTF-8 bytes it produces.
    **
    ** Title case is stored as its difference from upper case; a run of delta 0
    ** in it means the source upper cases to something and title cases to
    ** itself.
    **
    ** See Copyright Notice in mruby.h
    */

    /* One run of sources start, start+stride, ... (count entries), each
       mapping to the source plus delta. */
    typedef struct uni_case_run {
      uint32_t start;
      uint16_t count;
      uint8_t stride;
      int32_t delta;
    } uni_case_run;

    /* One source mapping to the `len` bytes of uni_case_pool at `off`. */
    typedef struct uni_case_multi {
      uint32_t cp;
      uint16_t off;
      uint8_t len;
    } uni_case_multi;

    /* The widest mapping any of the three produces, in UTF-8 bytes. */
    #define UNI_CASE_MAX_BYTES #{widest}
  HEAD

  out.puts
  out.puts "static const unsigned char uni_case_pool[] = {"
  pool.each_slice(12) do |slice|
    out.puts "  " + slice.map { |b| "0x%02X," % b }.join(" ")
  end
  out.puts "};"

  encoded.each do |name, runs, multi|
    up = name.upcase
    lo = [runs.map { |r| r[:start] }.min, multi.map(&:first).min].compact.min
    hi = [runs.map { |r| r[:start] + (r[:count] - 1) * r[:stride] }.max,
          multi.map(&:first).max].compact.max
    covered = runs.sum { |r| r[:count] }

    out.puts
    out.puts "/* #{TABLES.find { |t| t[0] == name }[3]}: " \
             "#{covered} sources in #{runs.size} runs, #{multi.size} multi. */"
    # A table with nothing in it is spelled as a null pointer rather than as an
    # array of no elements, which C does not have.
    if runs.empty?
      out.puts "#define UNI_#{up}_RUNS NULL"
      out.puts "#define UNI_#{up}_RUN_COUNT 0"
    else
      out.puts "static const uni_case_run uni_#{name}_runs[] = {"
      runs.each do |r|
        out.puts "  { %s, %4d, %d, %7d }," % [hex(r[:start]), r[:count], r[:stride], r[:delta]]
      end
      out.puts "};"
      out.puts "#define UNI_#{up}_RUNS uni_#{name}_runs"
      out.puts "#define UNI_#{up}_RUN_COUNT (sizeof(uni_#{name}_runs) / sizeof(uni_#{name}_runs[0]))"
    end

    out.puts
    if multi.empty?
      out.puts "#define UNI_#{up}_MULTI NULL"
      out.puts "#define UNI_#{up}_MULTI_COUNT 0"
    else
      out.puts "static const uni_case_multi uni_#{name}_multi[] = {"
      multi.each do |cp, off, len|
        out.puts "  { %s, %4d, %d }," % [hex(cp), off, len]
      end
      out.puts "};"
      out.puts "#define UNI_#{up}_MULTI uni_#{name}_multi"
      out.puts "#define UNI_#{up}_MULTI_COUNT (sizeof(uni_#{name}_multi) / sizeof(uni_#{name}_multi[0]))"
    end

    out.puts
    out.puts "/* Lowest and highest source either table holds, so a lookup that"
    out.puts "   cannot hit anything costs one comparison. */"
    out.puts "#define UNI_#{up}_MIN #{hex(lo)}"
    out.puts "#define UNI_#{up}_MAX #{hex(hi)}"
  end
end

$stderr.puts "wrote #{File.join(outdir, 'unicase.h')}: pool #{pool.size} bytes, " +
             encoded.map { |name, runs, multi| "#{name} #{runs.size} runs / #{multi.size} multi" }.join(", ")
