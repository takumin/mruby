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
    # dense numbering could index straight to it. An ID is a hash, so IDs
    # spread evenly over the range and the top `BUCKET_BITS` of one narrow the
    # search to the handful of entries sharing them: at the sizes this tree
    # reaches, about six, which the search settles in three steps. Widening
    # the index further buys a few percent for kilobytes, so it stays here.
    BUCKET_BITS = 8

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

    # The tables are read only by `src/symbol.c`. They are ordered by ID, so a
    # lookup narrows to a bucket and searches inside it. Names live in one
    # pool with an offset each, rather than in an array of pointers with a
    # length each: that drops one relocation and eight bytes per symbol, which
    # more than pays for the ID column the hash scheme adds.
    def write_table_header(presyms, ids)
      by_id = presyms.sort_by{|sym| ids[sym]}
      offsets = []
      pos = 0
      by_id.each do |sym|
        offsets << pos
        pos += sym.bytesize + 1  # each name is NUL-terminated
      end
      offsets << pos
      offset_type = pos <= 0xffff ? "uint16_t" : "uint32_t"
      index_type = by_id.size <= 0xffff ? "uint16_t" : "uint32_t"
      buckets = bucket_index(by_id, ids)

      _pp "GEN", table_header_path.relative_path
      File.open(table_header_path, "w:binary") do |f|
        f.puts "#define MRB_PRESYM_HASH_BITS #{HASH_BITS}"
        f.puts "#define MRB_PRESYM_BUCKET_BITS #{BUCKET_BITS}"
        f.puts "#define MRB_PRESYM_COUNT #{by_id.size}"
        f.puts "#define MRB_PRESYM_LEN_MAX #{by_id.map(&:bytesize).max || 0}"
        f.puts
        # PicoRuby builds the VM core as a gem, so its mrbc build scans zero
        # presyms. An empty array is invalid C, so leave one unread element.
        f.puts "static const uint32_t presym_id_table[] = {"
        if by_id.empty?
          f.puts "  0"
        else
          by_id.each{|sym| f.puts "  #{ids[sym]},\t/* #{sym} */"}
        end
        f.puts "};"
        f.puts
        f.puts "static const #{index_type} presym_bucket_table[] = {"
        buckets.each_slice(16){|slice| f.puts "  #{slice.join(', ')},"}
        f.puts "};"
        f.puts
        f.puts "static const #{offset_type} presym_offset_table[] = {"
        offsets.each_slice(12){|slice| f.puts "  #{slice.join(', ')},"}
        f.puts "};"
        f.puts
        f.puts "static const char presym_name_pool[] ="
        if by_id.empty?
          f.puts %|  ""|
        else
          # The NUL is written as its own literal so that it cannot be read
          # as part of an escape sequence closing the name, whatever
          # `c_escape` left at the end. Adjacent literals concatenate, and
          # only the last one brings a NUL of its own.
          by_id.each{|sym| f.puts %|  "#{c_escape(sym)}" "\\0"|}
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

    # `buckets[k]` is the index of the first entry whose ID reaches
    # `k << (HASH_BITS - BUCKET_BITS)`, so bucket `k` holds the entries
    # `buckets[k]...buckets[k+1]` and an ID outside them lands on the bound
    # the search wanted anyway.
    def bucket_index(by_id, ids)
      shift = HASH_BITS - BUCKET_BITS
      buckets = []
      k = 0
      by_id.each_with_index do |sym, i|
        b = ids[sym] >> shift
        while k <= b
          buckets << i
          k += 1
        end
      end
      while k <= (1 << BUCKET_BITS)
        buckets << by_id.size
        k += 1
      end
      buckets
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
