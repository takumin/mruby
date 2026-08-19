# Print which codepoints each character class matches, so that CRuby and this
# gem can be held against each other over the whole of Unicode rather than
# over the alphabet a differential run draws from.
#
#   ruby  mrbgems/mruby-regexp/tools/differential/classes.rb [options] > cruby.out
#   mruby mrbgems/mruby-regexp/tools/differential/classes.rb [options] > mruby.out
#   ruby  mrbgems/mruby-regexp/tools/differential/classes.rb --compare cruby.out mruby.out...
#
# One line per class: its name, a tab, and the codepoints it matches as ranges
# in hex. A class the engine refuses is printed as `refused` with the message,
# which is an answer to compare like any other. classes.sh runs the three
# steps; --compare is the report, and reads the first file as the reference.
#
# The classes are the POSIX brackets and their negations, and the shorthands,
# each of them both on its own and inside a class, since a bracket expression
# and a shorthand are read by different code. Every one is matched anchored
# against the single character, so what is compared is membership alone.
#
# The mruby has to index its strings by character: mruby-encoding defines
# MRB_UTF8_STRING and the default gembox does not carry it, so a default build
# reads the bytes one at a time and answers every class within ASCII. The
# full-core gembox carries it.
#
# Options
#   -m, --max N        the highest codepoint to walk, in hex (10FFFF)
#   -s, --show N       ranges to print per class in the report (12)
#       --compare      read the files named as runs and report where they part

POSIX = %w[alpha alnum blank cntrl digit graph lower print punct space upper xdigit word]
SHORTHAND = %w[\\w \\W \\d \\D \\s \\S]

def classes
  list = []
  POSIX.each do |name|
    list << ["[[:#{name}:]]", "[[:#{name}:]]"]
    list << ["[[:^#{name}:]]", "[[:^#{name}:]]"]
    list << ["[x[:#{name}:]]", "[x[:#{name}:]]"]
  end
  SHORTHAND.each do |atom|
    list << [atom, atom]
    list << ["[#{atom}]", "[#{atom}]"]
  end
  list
end

# The character as UTF-8, spelled out rather than through Integer#chr, which
# takes no encoding under mruby. CRuby reads a class against the subject's
# encoding, so the bytes are told they are UTF-8 where a string carries one.
def utf8(cp)
  bytes =
    if cp < 0x80
      [cp]
    elsif cp < 0x800
      [0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)]
    elsif cp < 0x10000
      [0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)]
    else
      [0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)]
    end
  s = bytes.map { |b| b.chr }.join
  s.respond_to?(:force_encoding) ? s.force_encoding("UTF-8") : s
end

def hex(cp)
  sprintf("%04X", cp)
end

def spell(ranges)
  ranges.map { |a, b| a == b ? hex(a) : "#{hex(a)}-#{hex(b)}" }
end

# The codepoints a set holds, as ranges, from a sorted list.
def to_ranges(codepoints)
  ranges = []
  codepoints.each do |cp|
    if ranges.last && ranges.last[1] == cp - 1
      ranges.last[1] = cp
    else
      ranges << [cp, cp]
    end
  end
  ranges
end

def walk(max)
  compiled = classes.map do |name, source|
    begin
      [name, Regexp.new("\\A(?:#{source})\\z"), nil]
    rescue RegexpError => e
      [name, nil, e.message.split("\n")[0]]
    end
  end
  compiled.each do |name, re, refusal|
    if re.nil?
      puts "#{name}\trefused #{refusal}"
      next
    end
    hits = []
    open = nil
    (0..max).each do |cp|
      next if cp >= 0xD800 && cp <= 0xDFFF
      if re =~ utf8(cp)
        open ||= cp
      elsif open
        hits << [open, cp - 1]
        open = nil
      end
    end
    hits << [open, max] if open
    puts "#{name}\t#{spell(hits).join(' ')}"
  end
end

def read(path)
  File.readlines(path, chomp: true).map do |line|
    name, spec = line.split("\t", 2)
    spec = spec.to_s
    next [name, spec] if spec.start_with?("refused")
    members = {}
    spec.split(" ").each do |range|
      first, last = range.split("-")
      (first.to_i(16)..(last || first).to_i(16)).each { |cp| members[cp] = true }
    end
    [name, members]
  end
end

def compare(paths, show)
  runs = paths.map { |p| read(p) }
  names = paths.map { |p| File.basename(p, ".*") }
  reference = runs[0]
  parted = 0
  reference.each_with_index do |(name, wanted), i|
    (1...runs.size).each do |k|
      _, got = runs[k][i]
      next if wanted == got
      parted += 1
      if wanted.is_a?(String) || got.is_a?(String)
        puts "#{name}: #{names[0]} #{wanted.is_a?(String) ? wanted : 'matched'}, #{names[k]} #{got.is_a?(String) ? got : 'matched'}"
        next
      end
      only_reference = to_ranges(wanted.keys - got.keys)
      only_run = to_ranges(got.keys - wanted.keys)
      puts "#{name}: #{names[0]} only #{only_reference.sum { |a, b| b - a + 1 }}, #{names[k]} only #{only_run.sum { |a, b| b - a + 1 }}"
      puts "  #{names[0]} only: #{spell(only_reference).first(show).join(' ')}#{only_reference.size > show ? ' ...' : ''}" unless only_reference.empty?
      puts "  #{names[k]} only: #{spell(only_run).first(show).join(' ')}#{only_run.size > show ? ' ...' : ''}" unless only_run.empty?
    end
  end
  puts "#{reference.size} classes, #{parted} parted"
end

max = 0x10FFFF
show = 12
files = []
mode = :walk
args = ARGV.dup
while (arg = args.shift)
  case arg
  when "-m", "--max" then max = args.shift.to_i(16)
  when "-s", "--show" then show = args.shift.to_i
  when "--compare" then mode = :compare
  else files << arg
  end
end

if mode == :compare
  abort "usage: ruby #{$0} --compare reference.out run.out..." if files.size < 2
  compare(files, show)
else
  walk(max)
end
