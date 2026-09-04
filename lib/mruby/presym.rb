module MRuby
  class Presym
    OPERATORS = {
      "!" => "not",
      "%" => "mod",
      "&" => "and",
      "*" => "mul",
      "+" => "add",
      "-" => "sub",
      "/" => "div",
      "<" => "lt",
      ">" => "gt",
      "^" => "xor",
      "`" => "tick",
      "|" => "or",
      "~" => "neg",
      "!=" => "neq",
      "!~" => "nmatch",
      "&&" => "andand",
      "**" => "pow",
      "+@" => "plus",
      "-@" => "minus",
      "<<" => "lshift",
      "<=" => "le",
      "==" => "eq",
      "=~" => "match",
      ">=" => "ge",
      ">>" => "rshift",
      "[]" => "aref",
      "||" => "oror",
      "<=>" => "cmp",
      "===" => "eqq",
      "[]=" => "aset",
    }.freeze

    SYMBOL_TO_MACRO = {
    #      Symbol      =>      Macro
    # [prefix, suffix] => [prefix, suffix]
      ["$"   , ""    ] => ["GV"  , ""    ],
      ["@@"  , ""    ] => ["CV"  , ""    ],
      ["@"   , ""    ] => ["IV"  , ""    ],
      [""    , "!"   ] => [""    , "_B"  ],
      [""    , "?"   ] => [""    , "_Q"  ],
      [""    , "="   ] => [""    , "_E"  ],
      [""    , ""    ] => [""    , ""    ],
    }.freeze

    C_STR_LITERAL_RE = /"(?:[^\\\"]|\\.)*"/

    ESCAPE_SEQUENCE_MAP = {
      "a" => "\a",
      "b" => "\b",
      "e" => "\e",
      "f" => "\f",
      "n" => "\n",
      "r" => "\r",
      "t" => "\t",
      "v" => "\v",
    }
    ESCAPE_SEQUENCE_MAP.keys.each { |k| ESCAPE_SEQUENCE_MAP[ESCAPE_SEQUENCE_MAP[k]] = k }

    # A presym ID is a hash of the symbol name folded into `HASH_BITS` bits,
    # so a name keeps its ID whatever else a build happens to scan. The
    # numbering used to run 1..n over the scanned set, which renumbered every
    # symbol whenever one was added, and `id.h` is included by way of
    # `mruby.h`: one new symbol invalidated every object a compiler cache
    # held. IDs derived from the name alone leave the other symbols where
    # they were, so ccache and sccache keep their entries.
    #
    # `HASH_BITS` and `name_hash` are the Ruby half of the scheme; the C half
    # is `MRB_PRESYM_BITS` in `include/mruby/presym.h` and `presym_hash()` in
    # `src/symbol.c`. The two have to agree, and `table.h` carries the width
    # this file used so a stale header is caught at compile time.
    HASH_BITS = 19
    HASH_MASK = (1 << HASH_BITS) - 1

    # Reading a name back from an ID means finding its entry, where the old
    # dense numbering could index straight to it. The set of IDs is settled
    # once the scan is done, so the entry each one lands on can be settled
    # then too: `perfect_hash` places every ID in a slot of its own, and the
    # lookup is a shift, a load, an xor, a mask and a conditional subtract.
    # Nothing is searched and nothing is compared but the ID the slot holds.
    #
    # The placement is the CHD construction: bucket the IDs by their high
    # bits, take the crowded buckets first, and give each one the smallest
    # displacement that drops its IDs on free slots.
    #
    # The slot count is not tied to a power of two. Rounding it up to one
    # would leave a table of 2049 symbols with 2047 slots empty, and an empty
    # slot still costs the six bytes of an ID and an offset. Any count works
    # instead, since a value masked to the next power of two is at most twice
    # the count and one conditional subtract brings it back inside.
    #
    # Which count to pick is measured rather than assumed: packing tight
    # spends fewer bytes on empty slots but needs larger displacements to
    # fill, and a displacement past 255 doubles the width of the table holding
    # them. `perfect_hash` tries these ratios of the symbol count against a
    # range of bucket counts and keeps the shape whose tables are smallest.
    SLOT_RATIOS = [1.00, 1.02, 1.05, 1.10, 1.20].freeze

    # `BUCKET_DIVISOR` sets how many IDs share a bucket to begin with. Around
    # four is where the search for a displacement stays short and the table of
    # displacements stays small; the shapes either side of it are tried too.
    BUCKET_DIVISOR = 4
    BUCKET_BITS_SPREAD = 2

    # How far the slot table may be grown past the widest ratio when no
    # placement is found at all. Growth has never been needed at this tree's
    # sizes; it is here so that no symbol set can fail to build.
    SLOT_GROWTH_MAX = 4

    # FNV-1a over the name's bytes, XOR-folded down to `HASH_BITS`. ID 0 is
    # reserved for "no such symbol", so a name that folds to it takes 1 and
    # meets the collision handling in `assign_ids` like any other clash.
    def self.name_hash(name)
      h = 0x811c9dc5
      name.each_byte {|b| h = ((h ^ b) * 0x01000193) & 0xffffffff}
      h = ((h >> HASH_BITS) ^ h) & HASH_MASK
      h == 0 ? 1 : h
    end

    def initialize(build)
      @build = build
    end

    def scan(paths)
      presym_hash = {}
      paths.each {|path| read_preprocessed(presym_hash, path)}
      presym_hash.keys.sort_by!{|sym| [c_literal_size(sym), sym]}
    end

    # Two names folding to the same ID is rare (about one build in four
    # hundred at the sizes this tree reaches) but has to be answered, and
    # answering it with a build error would leave the user nothing to do. The
    # later name takes the next free ID instead, which keeps the run of IDs a
    # probe walks contiguous, and `presym_find` in `src/symbol.c` walks that
    # run. Only the displaced name moves; every other symbol stays where its
    # own hash puts it.
    def assign_ids(presyms)
      ids = {}
      taken = {}
      presyms.each do |sym|
        id = Presym.name_hash(sym)
        while taken[id]
          id += 1
          if id > HASH_MASK
            raise "presym ID space exhausted while placing #{sym.inspect}"
          end
        end
        taken[id] = true
        ids[sym] = id
      end
      ids
    end

    def read_list
      File.readlines(list_path, mode: "r:binary").each(&:chomp!)
    end

    def write_list(presyms)
      _pp "GEN", list_path.relative_path
      File.binwrite(list_path, presyms.join("\n") << "\n")
    end

    # `id.h` reaches every object through `mruby.h`, so it holds nothing but
    # the `#define`s of the IDs themselves: a macro definition leaves no trace
    # in preprocessed output, and a translation unit that names none of the
    # new symbols preprocesses to the same bytes it did before. That is what a
    # compiler cache compares. The enum this used to write was real C text and
    # landed in every preprocessed source, so a single added symbol changed
    # them all.
    def write_id_header(presyms, ids)
      prefix_re = Regexp.union(*SYMBOL_TO_MACRO.keys.map(&:first).uniq)
      suffix_re = Regexp.union(*SYMBOL_TO_MACRO.keys.map(&:last).uniq)
      sym_re = /\A(#{prefix_re})?([\w&&\D]\w*)(#{suffix_re})?\z/o
      _pp "GEN", id_header_path.relative_path
      File.open(id_header_path, "w:binary") do |f|
        presyms.each do |sym|
          num = ids[sym]
          if sym_re =~ sym && (affixes = SYMBOL_TO_MACRO[[$1, $3]])
            f.puts "#define MRB_#{affixes * 'SYM'}__#{$2} #{num}"
          elsif name = OPERATORS[sym]
            f.puts "#define MRB_OPSYM__#{name} #{num}"
          end
        end
      end
    end

    # The tables are read only by `src/symbol.c`, and they are laid out by
    # slot, so that `presym_slot()` indexes straight into them. An empty slot
    # holds `MRB_PRESYM_NO_ID`, which no ID equals, so one comparison both
    # confirms the entry and rejects a name that was never stored.
    #
    # Names live in one pool with an offset each, rather than in an array of
    # pointers with a length each: that drops one relocation and eight bytes
    # per symbol, which pays for most of what the slot table costs.
    def write_table_header(presyms, ids)
      name_bytes = presyms.sum{|sym| sym.bytesize + 1}  # each name is NUL-terminated
      ph = presyms.empty? ? nil : perfect_hash(ids, name_bytes)
      slots = ph ? ph[:slots] : [nil]
      offsets = []
      pos = 0
      slots.each do |sym|
        offsets << pos
        pos += sym.bytesize + 1 if sym
      end
      offsets << pos
      offset_type = pos <= 0xffff ? "uint16_t" : "uint32_t"
      disp = ph ? ph[:disp] : [0]
      disp_type = disp.max <= 0xff ? "uint8_t" : "uint16_t"

      _pp "GEN", table_header_path.relative_path
      File.open(table_header_path, "w:binary") do |f|
        f.puts "#define MRB_PRESYM_HASH_BITS #{HASH_BITS}"
        f.puts "#define MRB_PRESYM_SLOT_BITS #{ph ? ph[:slot_bits] : 0}"
        f.puts "#define MRB_PRESYM_BUCKET_BITS #{ph ? ph[:bucket_bits] : 0}"
        f.puts "#define MRB_PRESYM_COUNT #{presyms.size}"
        f.puts "#define MRB_PRESYM_SLOTS #{slots.size}"
        f.puts "#define MRB_PRESYM_LEN_MAX #{presyms.map(&:bytesize).max || 0}"
        f.puts "#define MRB_PRESYM_NO_ID 0xffffffff"
        f.puts
        f.puts "static const #{disp_type} presym_disp_table[] = {"
        disp.each_slice(16){|slice| f.puts "  #{slice.join(', ')},"}
        f.puts "};"
        f.puts
        f.puts "static const uint32_t presym_id_table[] = {"
        slots.each do |sym|
          if sym
            f.puts "  #{ids[sym]},\t/* #{sym} */"
          else
            f.puts "  MRB_PRESYM_NO_ID,"
          end
        end
        f.puts "};"
        f.puts
        f.puts "static const #{offset_type} presym_offset_table[] = {"
        offsets.each_slice(12){|slice| f.puts "  #{slice.join(', ')},"}
        f.puts "};"
        f.puts
        f.puts "static const char presym_name_pool[] ="
        # PicoRuby builds the VM core as a gem, so its mrbc build scans zero
        # presyms. An empty initializer is invalid C, so write an empty name.
        named = slots.compact
        if named.empty?
          f.puts %|  ""|
        else
          # The NUL is written as its own literal so that it cannot be read
          # as part of an escape sequence closing the name, whatever
          # `c_escape` left at the end. Adjacent literals concatenate, and
          # only the last one brings a NUL of its own.
          named.each{|sym| f.puts %|  "#{c_escape(sym)}" "\\0"|}
        end
        f.puts "  ;"
      end
    end

    def list_path
      @list_path ||= "#{@build.build_dir}/presym".freeze
    end

    def header_dir
      @header_dir ||= "#{@build.build_dir}/include/mruby/presym".freeze
    end

    def id_header_path
      @id_header_path ||= "#{header_dir}/id.h".freeze
    end

    def table_header_path
      @table_header_path ||= "#{header_dir}/table.h".freeze
    end

    def headers_exist?
      File.exist?(id_header_path) && File.exist?(table_header_path)
    end

    private

    # Places every ID in a slot of its own and answers the shape that says
    # where: the slot count and the bits it is masked to, the bucket count,
    # the displacement per bucket, and the slots themselves, each holding a
    # symbol or nil. `src/symbol.c` recomputes the same slot in
    # `presym_slot()`, and `table.h` carries every number it needs.
    #
    # `name_bytes` is what the names themselves take, so that shapes are
    # ranked by the tables they actually produce.
    def perfect_hash(ids, name_bytes)
      count = ids.size
      offset_width = name_bytes <= 0xffff ? 2 : 4
      best = nil
      slot_counts(count).each do |slot_count|
        bucket_bits_candidates(slot_count).each do |bucket_bits|
          placed = place_ids(ids, slot_count, bucket_bits)
          next unless placed
          placed[:bytes] = slot_count * 4 + (slot_count + 1) * offset_width +
                           (1 << bucket_bits) * (placed[:max_disp] <= 0xff ? 1 : 2)
          best = placed if best.nil? || placed[:bytes] < best[:bytes]
        end
        # Every ratio is measured, not just the first that fits: a looser
        # table spends more on empty slots but settles for smaller
        # displacements, and which of the two wins is not decided in advance.
        # The doubling ladder past the ratios is only a way out when none of
        # them can be placed, so it stops as soon as one is.
        break if best && slot_count > [(count * SLOT_RATIOS.last).ceil,
                                       1 << [Math.log2([count, 2].max).ceil, 1].max].max
      end
      return best if best
      raise "no presym slot placement found for #{count} symbols"
    end

    # Slot counts to try, tightest first, then a doubling ladder for a symbol
    # set that somehow defeats every ratio.
    #
    # The next power of two is tried as well, whatever ratio of the count it
    # happens to be. A table that size needs no subtraction to stay inside it,
    # and it leaves the displacements so much room that the table holding them
    # can be the narrower one; where that wins, the ratios would have missed
    # it, and this keeps the shape chosen no worse than a power of two.
    def slot_counts(count)
      counts = SLOT_RATIOS.map{|r| [(count * r).ceil, 1].max}
      counts << (1 << [Math.log2([count, 2].max).ceil, 1].max)
      widest = counts.max
      SLOT_GROWTH_MAX.times{|i| counts << widest * (2 ** (i + 1))}
      counts.uniq.sort.select{|c| c <= (1 << HASH_BITS)}
    end

    def bucket_bits_candidates(slot_count)
      middle = [Math.log2([slot_count / BUCKET_DIVISOR, 1].max).round, 1].max
      lo = [middle - BUCKET_BITS_SPREAD, 1].max
      hi = [middle + BUCKET_BITS_SPREAD, HASH_BITS].min
      (lo..hi).to_a
    end

    # The CHD placement itself, or nil when this shape has no room for one.
    #
    # Two IDs in one bucket have to reach different slots whatever the
    # displacement, since a displacement moves a whole bucket at once. Folding
    # the ID down before the xor is what gives them the chance to: a bucket is
    # a run of high bits, so without the fold two IDs sharing one could differ
    # only above the slot mask, where nothing would tell them apart.
    def place_ids(ids, slot_count, bucket_bits)
      slot_bits = [Math.log2(slot_count).ceil, 1].max
      shift = HASH_BITS - bucket_bits
      return nil if shift < 0
      buckets = Hash.new{|h, k| h[k] = []}
      ids.each{|sym, id| buckets[id >> shift] << sym}
      slots = Array.new(slot_count)
      disp = Array.new(1 << bucket_bits, 0)
      max_disp = 0
      # Crowded buckets first: they have the fewest displacements left to them
      # once the table has filled up, so they are the ones that fail late.
      buckets.keys.sort_by{|b| [-buckets[b].size, b]}.each do |b|
        group = buckets[b]
        d = (0...(1 << slot_bits)).find do |cand|
          taken = group.map{|sym| slot_of(ids[sym], cand, slot_bits, slot_count)}
          taken.uniq.size == taken.size && taken.none?{|i| slots[i]}
        end
        return nil unless d
        group.each{|sym| slots[slot_of(ids[sym], d, slot_bits, slot_count)] = sym}
        disp[b] = d
        max_disp = d if d > max_disp
      end
      {slot_count: slot_count, slot_bits: slot_bits, bucket_bits: bucket_bits,
       disp: disp, slots: slots, max_disp: max_disp}
    end

    # The masked value is below twice the slot count, since the mask is the
    # next power of two, so one subtraction brings it inside.
    def slot_of(id, disp, slot_bits, slot_count)
      x = ((id ^ (id >> slot_bits)) ^ disp) & ((1 << slot_bits) - 1)
      x < slot_count ? x : x - slot_count
    end

    def c_escape(sym)
      sym.gsub(/([\x01-\x1f\x7f-\xff])|("|\\)/n) {
        case
        when $1
          e = ESCAPE_SEQUENCE_MAP[$1]
          e ? "\\#{e}" : '\\x%02x""' % $1.ord
        when $2
          "\\#$2"
        end
      }
    end

    def read_preprocessed(presym_hash, path)
      File.binread(path).scan(/<@! (.*?) !@>/) do |part,|
        literals = part.scan(C_STR_LITERAL_RE)
        unless literals.empty?
          literals = literals.map{|l| l[1..-2]}
          literals.each do |e|
            e.gsub!(/\\x([0-9A-Fa-f]{1,2})|\\(0[0-7]{,3})|\\([abefnrtv])|\\(.)/) do
              case
              when $1; $1.hex.chr(Encoding::BINARY)
              when $2; $2.oct.chr(Encoding::BINARY)
              when $3; ESCAPE_SEQUENCE_MAP[$3]
              when $4; $4
              end
            end
          end
          presym_hash[literals.join] = true
        end
      end
    end

    def c_literal_size(literal_without_quote)
      literal_without_quote.size  # TODO: consider escape sequence
    end
  end
end
