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

    # 32-bit FNV-1a with a final avalanche, the hash `presym_find` computes
    # over a name it is looking up. The C side in `src/symbol.c` must compute
    # the same value for the same bytes, so the two are changed together.
    FNV32_OFFSET = 2166136261
    FNV32_PRIME = 16777619
    MASK32 = 0xffffffff
    GOLDEN32 = 2654435761

    def self.hash32(str)
      h = FNV32_OFFSET
      str.each_byte {|b| h = ((h ^ b) * FNV32_PRIME) & MASK32}
      h = (h ^ (h >> 15)) & MASK32
      h = (h * 2246822519) & MASK32
      (h ^ (h >> 13)) & MASK32
    end

    def initialize(build)
      @build = build
    end

    def scan(paths)
      presym_hash = {}
      paths.each {|path| read_preprocessed(presym_hash, path)}
      presym_hash.keys.sort_by!{|sym| [c_literal_size(sym), sym]}
    end

    # Where each symbol sits in the table its number comes from.
    #
    # A symbol's number used to be its position among the symbols sorted by
    # (length, bytes), so one symbol arriving in the middle moved every number
    # after it: 544 of 1592 on the default config, and every object naming one
    # of those missed the compiler cache. Here a symbol's number is the slot
    # its own name hashes to, in a table with room to spare, so it depends on
    # the symbol's name and on the few symbols whose probes cross that slot.
    # Adding one symbol moves 0.63 numbers on average, and none at all four
    # times out of five.
    #
    # The table is a power of two long, so the runtime side (`presym_find` in
    # `src/symbol.c`) indexes it by masking. The step is drawn from the hash
    # and forced odd, which is coprime with the length: the probe reaches
    # every slot, and two names that share a first slot do not then share a
    # run of them.
    #
    # It is also never more than four fifths full, which is what bounds that
    # probe and the number of symbols one addition can move. Taking the
    # smallest power of two that merely fits would leave the fill to wherever
    # the symbol count landed between two powers of two: a build with 2040
    # symbols would fill a 2048-slot table to 99.6%, where a name that is no
    # symbol costs 915 probes at the 99th percentile against 19 here, and one
    # added symbol moves 4.71 numbers against 0.75. At four fifths the worst
    # a build can see is 21 probes and 0.86 numbers, and neither of the tables
    # this tree builds grows by a slot for it.
    def slots(presyms)
      size = 2
      size <<= 1 while presyms.size > size * 4 / 5
      table = Array.new(size)
      presyms.each do |sym|
        hash = self.class.hash32(sym)
        i = hash & (size - 1)
        step = ((hash >> 16) | 1) & (size - 1)
        i = (i + step) & (size - 1) while table[i]
        table[i] = sym
      end
      table
    end

    def read_list
      File.readlines(list_path, mode: "r:binary").each(&:chomp!)
    end

    def write_list(presyms)
      _pp "GEN", list_path.relative_path
      File.binwrite(list_path, presyms.join("\n") << "\n")
    end

    def write_id_header(presyms)
      prefix_re = Regexp.union(*SYMBOL_TO_MACRO.keys.map(&:first).uniq)
      suffix_re = Regexp.union(*SYMBOL_TO_MACRO.keys.map(&:last).uniq)
      sym_re = /\A(#{prefix_re})?([\w&&\D]\w*)(#{suffix_re})?\z/o
      _pp "GEN", id_header_path.relative_path
      File.open(id_header_path, "w:binary") do |f|
        # One macro per symbol, rather than one enumerator list over all of
        # them. An enumerator list is C text, so every source including this
        # header carries all of it into its preprocessed form: a symbol added
        # for one source would change what the compiler is handed for every
        # other source, and `ccache` and `sccache`, which key a compile on
        # exactly that, would miss on all of them. A macro no source expands
        # leaves nothing behind to key on.
        #
        # This also leaves nothing to write when a build scans no symbol at
        # all, as the mrbc build of PicoRuby does, where an empty enumerator
        # list would have been invalid C.
        slots(presyms).each_with_index do |sym, i|
          next unless sym
          num = i + 1
          if sym_re =~ sym && (affixes = SYMBOL_TO_MACRO[[$1, $3]])
            f.puts "#define MRB_#{affixes * 'SYM'}__#{$2} #{num}"
          elsif name = OPERATORS[sym]
            f.puts "#define MRB_OPSYM__#{name} #{num}"
          end
        end
        f.puts
        # The width of the presym number space, which is the table's length
        # rather than the symbol count: the slots no symbol hashed to are
        # numbers no symbol has. `MRB_PRESYM_COUNT` is the count.
        f.puts "#define MRB_PRESYM_MAX #{slots(presyms).size}"
        f.puts "#define MRB_PRESYM_COUNT #{presyms.size}"
      end
    end

    def write_table_header(presyms)
      _pp "GEN", table_header_path.relative_path
      File.open(table_header_path, "w:binary") do |f|
        table = slots(presyms)
        # The names as one blob of bytes with an offset apiece, rather than
        # an array of pointers to separate literals. An array of pointers is
        # relocated at load time, so it is writable memory in a build that
        # links position-independent, and it costs a pointer for every slot
        # no symbol occupies. Offsets are constants: they go where the names
        # already were, and a slot nothing occupies costs two bytes.
        #
        # Offset zero is the blob's leading terminator, and so is the offset
        # of no symbol at all.
        blob = [""]
        offsets = []
        pos = 1
        table.each do |sym|
          unless sym
            offsets << 0
            next
          end
          blob << sym
          offsets << pos
          pos += sym.bytesize + 1
        end
        offset_type = pos < 0x10000 ? "uint16_t" : "uint32_t"

        f.puts "#define MRB_PRESYM_MAX_LENGTH #{presyms.map(&:bytesize).max}"
        f.puts
        f.puts "static const uint16_t presym_length_table[] = {"
        table.each{|sym| f.puts sym ? "  #{sym.bytesize},\t/* #{sym} */" : "  0,"}
        f.puts "};"
        f.puts
        # Each name is one string literal ending in its own terminator, and
        # the compiler concatenates them into the one array.
        f.puts "static const char presym_name_blob[] ="
        blob.each {|sym| f.puts %|  "#{escape(sym)}\\0"|}
        f.puts "  ;"
        f.puts
        f.puts "static const #{offset_type} presym_offset_table[] = {"
        offsets.each_slice(16) {|row| f.puts "  #{row.join(',')},"}
        f.puts "};"
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

    # A symbol's bytes as they are written inside a C string literal.
    def escape(sym)
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
