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

    # The file that fixes the order symbols are numbered in.
    #
    # It is append-only: a symbol keeps its place in it for as long as the
    # tree names that symbol at all, and a symbol new to the tree is written
    # after every symbol already there. A build numbers its own symbols by
    # this order, so a symbol added to one source leaves the number of every
    # other symbol where it was.
    #
    # That is what keeps an added symbol from costing a whole tree of
    # recompiles. With the numbers already handed out unchanged, and `id.h`
    # written as macros rather than as an enumerator list, a source that does
    # not name the new symbol preprocesses to the bytes it did before, and
    # `ccache` or `sccache` answers its compile from cache.
    REGISTRY_PATH = "presym.list"

    # The order the registry gives, read once for the tree rather than once
    # per build: every build of one run numbers against the same file.
    def self.registry
      @registry ||=
        begin
          path = File.join(MRUBY_ROOT, REGISTRY_PATH)
          File.exist?(path) ? File.readlines(path, mode: "r:binary").each(&:chomp!) : []
        end
    end

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
      order(presym_hash)
    end

    # The symbols this build scanned, in the order it numbers them.
    #
    # The registered ones come first, in the registry's order, and the rest
    # follow it sorted. A symbol the registry does not carry is one a gem
    # outside the tree contributed, and numbering those last keeps them from
    # moving a registered symbol; sorting them keeps the build reproducible
    # from its sources alone.
    def order(presym_hash)
      presym_hash = presym_hash.dup
      ordered = []
      self.class.registry.each {|sym| ordered << sym if presym_hash.delete(sym)}
      ordered.concat(presym_hash.keys.sort_by{|sym| [c_literal_size(sym), sym]})
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
        presyms.each.with_index(1) do |sym, num|
          if sym_re =~ sym && (affixes = SYMBOL_TO_MACRO[[$1, $3]])
            f.puts "#define MRB_#{affixes * 'SYM'}__#{$2} #{num}"
          elsif name = OPERATORS[sym]
            f.puts "#define MRB_OPSYM__#{name} #{num}"
          end
        end
        f.puts
        f.puts "#define MRB_PRESYM_MAX #{presyms.size}"
      end
    end

    def write_table_header(presyms)
      _pp "GEN", table_header_path.relative_path
      File.open(table_header_path, "w:binary") do |f|
        f.puts "static const uint16_t presym_length_table[] = {"
        presyms.each{|sym| f.puts "  #{sym.bytesize},\t/* #{sym} */"}
        f.puts "};"
        f.puts
        f.puts "static const char * const presym_name_table[] = {"
        presyms.each do |sym|
          sym = sym.gsub(/([\x01-\x1f\x7f-\xff])|("|\\)/n) {
            case
            when $1
              e = ESCAPE_SEQUENCE_MAP[$1]
              e ? "\\#{e}" : '\\x%02x""' % $1.ord
            when $2
              "\\#$2"
            end
          }
          f.puts %|  "#{sym}",|
        end
        f.puts "};"
        write_perfect_hash(f, presyms)
      end
    end

    # The lookup `presym_find` runs, as a perfect hash over the symbol names.
    #
    # The search used to be a binary search, which needed the tables sorted
    # by (length, bytes) and so pinned a symbol's number to where its name
    # sorted among all the others: one symbol added in the middle renumbered
    # everything after it. A hash asks nothing of the table's order, which is
    # what lets the registry decide it, and answers in a constant number of
    # probes instead of `log2(n)`.
    #
    # The construction is CHD. The names are drawn into `n/4` buckets by part
    # of their hash; taken largest bucket first, each bucket is given the
    # displacement that lands all of its names on slots still free. The slot
    # table is a power of two long so that the runtime side indexes it by
    # masking rather than by dividing, which is worth having on a target with
    # no divide instruction.
    def write_perfect_hash(f, presyms)
      return if presyms.empty?
      size, nbuckets, disp, slots = perfect_hash(presyms)
      slot_type = presyms.size < 0xffff ? "uint16_t" : "uint32_t"
      disp_type = disp.max < 0x100 ? "uint8_t" : (disp.max < 0x10000 ? "uint16_t" : "uint32_t")
      f.puts
      f.puts "#define MRB_PRESYM_MAX_LENGTH #{presyms.map(&:bytesize).max}"
      f.puts "#define MRB_PRESYM_HASH_SIZE #{size}"
      f.puts "#define MRB_PRESYM_BUCKETS #{nbuckets}"
      f.puts
      f.puts "static const #{disp_type} presym_disp_table[] = {"
      disp.each_slice(16) {|row| f.puts "  #{row.join(',')},"}
      f.puts "};"
      f.puts
      f.puts "static const #{slot_type} presym_slot_table[] = {"
      slots.each_slice(16) {|row| f.puts "  #{row.join(',')},"}
      f.puts "};"
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

    # The slot table, and the displacement of every bucket that fills it.
    #
    # The table starts at the smallest power of two that could hold the
    # symbols and is doubled if no displacement is found for some bucket at
    # that size, so that a set the construction cannot place densely costs
    # memory rather than the build.
    def perfect_hash(presyms)
      size = 1
      size <<= 1 while size < presyms.size
      loop do
        result = try_perfect_hash(presyms, size)
        return [size, *result] if result
        size <<= 1
      end
    end

    def try_perfect_hash(presyms, size)
      nbuckets = 1
      nbuckets <<= 1 while nbuckets * 4 < presyms.size
      hashes = presyms.map{|sym| self.class.hash32(sym)}
      buckets = Array.new(nbuckets) {[]}
      hashes.each_with_index{|h, i| buckets[h & (nbuckets - 1)] << i}
      slots = Array.new(size)
      disp = Array.new(nbuckets, 0)
      # Largest bucket first: the buckets that constrain the table most are
      # placed while the table is still empty enough to place them.
      buckets.each_index.sort_by{|b| [-buckets[b].size, b]}.each do |b|
        keys = buckets[b]
        next if keys.empty?
        d = 0
        loop do
          cand = keys.map{|i| slot_of(hashes[i], d, size)}
          if cand.uniq.size == cand.size && cand.none?{|s| slots[s]}
            cand.each_with_index{|s, j| slots[s] = keys[j] + 1}
            disp[b] = d
            break
          end
          d += 1
          return nil if d > (1 << 20)
        end
      end
      [nbuckets, disp, slots.map{|sym| sym || 0}]
    end

    def slot_of(hash, disp, size)
      ((hash >> 16) ^ ((disp * GOLDEN32) & MASK32)) & (size - 1)
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
