#!/usr/bin/env ruby
# frozen_string_literal: true

# Index of mruby methods and the C functions behind them.
#
# This is the lookup half of tools/method_uftrace.rb, split out so that the
# index can be produced, inspected, and compared on its own.
#
# An index is a list of Registration records.  Three fields identify a method
# and are always present; the rest are best-effort and depend on where the
# index came from:
#
#   klass singleton name         always
#   func                         C entry point, when the method has one
#   kind                         cfunc / proc / alias / undef, when known
#   file line via                where the registration was written, when known
#   alias_of                     the name this method was aliased from
#   def_file def_line            where the body is written, when known
#   shared_with                  other names of the same class reaching the
#                                same C function; derived, see Index
#
# Two producers exist:
#
#   SourceScanner  reads the C sources.  It knows where a registration was
#                  written, and guesses the rest: it cannot see which branches
#                  of a #if survive, which gems a gembox includes, or which
#                  class a ROM table is installed on.
#   RuntimeDump    reads a build's own method tables through
#                  mrbgems/mruby-bin-mtdump.  It knows exactly what exists and
#                  what each method dispatches to, and knows nothing about
#                  where any of it was written.
#
# The two are complements, not rivals; .merge takes the mapping from one and
# the provenance from the other.  Consumers should treat the optional fields
# as absent-by-default rather than assume a producer.
#
# Usage:
#
#   ruby tools/mruby_method_index.rb --class String
#   ruby tools/mruby_method_index.rb --grep index
#   ruby tools/mruby_method_index.rb --runtime --merge --class String
#   ruby tools/mruby_method_index.rb --format json > index.json
#
# As a library:
#
#   require_relative "mruby_method_index"
#   index = MRuby::MethodIndex.from_source
#   index.lookup("String#[]")     # => [Registration, ...]

require "json"

module MRuby
  module MethodIndex
    ROOT = File.expand_path("..", __dir__)

    # Format of the JSON produced by #to_h.  Bump when a field changes
    # meaning; adding an optional field does not need a bump.
    #
    # 2: func became optional.  A method written in Ruby has no C entry point
    #    to name, and version 1 had no way to say so.
    FORMAT_VERSION = 2

    # Prefer the build system's own table so the two never drift.
    OPERATORS =
      begin
        require File.join(ROOT, "lib", "mruby", "presym")
        MRuby::Presym::OPERATORS
      rescue LoadError, NameError
        {
          "!" => "not", "%" => "mod", "&" => "and", "*" => "mul", "+" => "add",
          "-" => "sub", "/" => "div", "<" => "lt", ">" => "gt", "^" => "xor",
          "`" => "tick", "|" => "or", "~" => "neg", "!=" => "neq", "!~" => "nmatch",
          "&&" => "andand", "**" => "pow", "+@" => "plus", "-@" => "minus",
          "<<" => "lshift", "<=" => "le", "==" => "eq", "=~" => "match",
          ">=" => "ge", ">>" => "rshift", "[]" => "aref", "||" => "oror",
          "<=>" => "cmp", "===" => "eqq", "[]=" => "aset",
        }
      end

    OPSYM_TO_OP = OPERATORS.each_with_object({}) { |(op, sym), h| h[sym] = op }.freeze

    DEFAULT_SOURCES = ["src/*.c", "mrbgems/*/src/*.c"].freeze

    # klass/singleton/name are the identity; everything else is optional and
    # may be nil depending on the producer.
    Registration = Struct.new(
      :klass, :singleton, :name, :func, :kind,
      :file, :line, :via, :alias_of,
      :def_file, :def_line, :rom, :shared_with,
      keyword_init: true
    ) do
      def key
        "#{klass}#{singleton ? '.' : '#'}#{name}"
      end

      # Registration site as "path:line", or nil when the producer has none.
      def location
        "#{file}:#{line}" if file && line
      end

      # Where the body itself is written, as "path:line".  For a C function
      # this comes from a binary's debug info, so it is nil until someone
      # annotates; for a method written in Ruby the runtime producer reads it
      # out of the irep.
      def definition
        "#{def_file}:#{def_line}" if def_file && def_line
      end

      # What to trace.  A method written in Ruby has no C entry point, and
      # saying so is more use than an empty column.
      def entry
        func || (kind == "proc" ? "(ruby)" : "-")
      end

      def to_h
        {
          "class" => klass, "singleton" => singleton, "name" => name,
          "func" => func, "kind" => kind,
          "file" => file, "line" => line, "via" => via, "alias_of" => alias_of,
          "def_file" => def_file, "def_line" => def_line, "rom" => rom,
          "shared_with" => shared_with,
        }
      end

      # The identifying fields are required.  A producer that omits one is
      # reporting a method it cannot actually name, and letting that through
      # only moves the failure to whoever reads the record.
      #
      # `func` is not among them: a method written in Ruby has no C function,
      # and demanding one would only invite a producer to invent it.
      REQUIRED = %w[class name].freeze

      def self.from_h(h)
        missing = REQUIRED.reject { |k| h[k] }
        raise ArgumentError, "index entry #{h.inspect} is missing #{missing.join(', ')}" unless missing.empty?

        new(
          klass: h["class"], singleton: !!h["singleton"], name: h["name"],
          func: h["func"], kind: h["kind"],
          file: h["file"], line: h["line"], via: h["via"], alias_of: h["alias_of"],
          def_file: h["def_file"], def_line: h["def_line"], rom: h["rom"],
          shared_with: h["shared_with"]
        )
      end
    end

    # A resolved set of registrations plus the queries the drivers need.
    class Index
      attr_reader :registrations, :producer

      def initialize(registrations, producer:)
        @registrations = registrations.sort_by { |r| [r.klass, r.singleton ? 1 : 0, r.name] }
        @producer = producer
        mark_shared_entries!
      end

      # key => [Registration, ...]
      def by_key
        @by_key ||= @registrations.group_by(&:key)
      end

      # Which other methods of the same class reach the same C function.
      #
      # A method table holds a function pointer, and several names in one
      # class can hold the same one.  Three different things produce that,
      # and none of them can be told from the others by the pointer alone:
      #
      #   alias append push          # mrb_alias_method copies the entry
      #                              # verbatim for a cfunc (src/class.c),
      #                              # so no alias proc records the fact
      #   Array#<< and Array#push    # two registrations, one implementation
      #   Module#included, #extended # six distinct hooks, all mrb_do_nothing
      #
      # So this says what is true of all three -- these names arrive at one
      # C function -- and does not guess a direction.  For the tracing tools
      # that is the operative fact anyway: a trace of mrb_hash_has_key cannot
      # tell you whether Hash#key?, #has_key?, #include? or #member? was
      # called, and a method whose registration site is missing because it
      # was aliased in mrblib is explained by the name it shares.
      #
      # Derived rather than reported by a producer, so it is recomputed for
      # every index.  It unions with what a record already carries: .merge
      # adds the names only the sources know about, and a file written after
      # that carries them where a fresh scan of these records would not.
      def mark_shared_entries!
        @registrations
          .select { |r| r.func && r.kind != "proc" }
          .group_by { |r| [r.klass, r.singleton, r.func] }
          .each_value do |group|
            names = group.map(&:name).uniq.sort
            next if names.size < 2

            group.each do |r|
              others = names - [r.name]
              r.shared_with = ((r.shared_with || []) | others).sort unless others.empty?
            end
          end
      end
      private :mark_shared_entries!

      def classes
        @classes ||= @registrations.map(&:klass).uniq.sort
      end

      # Accepts "String#[]", "String.new", or a bare "rindex".
      def lookup(spec, klass: nil)
        return by_key.fetch(spec, []) if spec =~ /\A.+?[#.].+\z/

        if klass
          return by_key["#{klass}##{spec}"] || by_key["#{klass}.#{spec}"] || []
        end
        @registrations.select { |r| r.name == spec }
      end

      def similar(spec)
        needle = spec.sub(/\A.*[#.]/, "").downcase
        @registrations
          .select { |r| r.name.downcase.include?(needle) }
          .map(&:key).uniq.sort.first(20)
      end

      def select(klass: nil, grep: nil)
        rows = @registrations
        rows = rows.select { |r| r.klass == klass } if klass
        if grep
          re = grep.is_a?(Regexp) ? grep : Regexp.new(grep, Regexp::IGNORECASE)
          rows = rows.select { |r| r.name =~ re || r.func.to_s =~ re }
        end
        rows
      end

      def to_h
        {
          "version" => FORMAT_VERSION,
          "producer" => @producer,
          "methods" => @registrations.map(&:to_h),
        }
      end

      def self.from_h(h)
        version = h["version"]
        unless version == FORMAT_VERSION
          raise ArgumentError, "unsupported index version #{version.inspect} (want #{FORMAT_VERSION})"
        end

        methods = h["methods"]
        raise ArgumentError, "index has no methods array" unless methods.is_a?(Array)

        new(methods.map { |m| Registration.from_h(m) }, producer: h["producer"])
      end
    end

    # Resolve source globs relative to the tree unless already absolute.
    def self.source_files(globs = DEFAULT_SOURCES)
      globs
        .flat_map { |g| Dir.glob(File.absolute_path?(g) ? g : File.join(ROOT, g)) }
        .uniq.sort
    end

    def self.from_source(globs = DEFAULT_SOURCES)
      files = source_files(globs)
      raise ArgumentError, "no source files matched #{globs.join(', ')}" if files.empty?

      Index.new(SourceScanner.new(files).registrations, producer: "c-source-scan")
    end

    def self.from_json(path)
      Index.from_h(JSON.parse(File.read(path)))
    end

    # Default location of the dumper built by build_config/host-gprof.rb.
    DEFAULT_MTDUMP = File.join(ROOT, "build", "host", "bin", "mruby-mtdump")

    def self.from_runtime(binary = DEFAULT_MTDUMP)
      dump = RuntimeDump.new(binary)
      unless dump.unresolved.empty?
        warn "#{dump.unresolved.size} C entry points have no symbol at their address:"
        dump.unresolved.first(10).each { |u| warn "  #{u}" }
      end
      Index.new(dump.registrations, producer: "vm-method-table")
    end

    # Take the mapping from one index and the provenance from another.
    #
    # The runtime dump is authoritative about what exists and what it
    # dispatches to; it reads the tables the VM will use.  The source scan is
    # the only producer that knows where a registration was written, because
    # a method table holds a function pointer and not a source line.
    #
    # A scan record is grafted on only when it agrees about the C function.
    # Where it does not, it is a record about some other registration -- the
    # method being #ifdef'd out of this build, say -- and its file and line
    # would send a reader to the wrong place.  Those disagreements are what
    # a differential check between the two producers is for; this merge
    # simply declines to paper over them.
    def self.merge(runtime, source)
      scanned = source.registrations.group_by { |r| [r.klass, r.singleton, r.name] }
      by_func = source.registrations.group_by { |r| [r.klass, r.singleton, r.func] }

      merged = runtime.registrations.map do |r|
        m = r.dup

        # A name the sources register this C function under, other than this
        # one.  The runtime pass finds most of these on its own, but not when
        # the name it was aliased from has since been replaced: mruby-regexp
        # takes `alias __aref []` and then defines String#[] in Ruby, so the
        # table has no other cfunc entry left holding mrb_str_aref_m.  The
        # scan still remembers which name it was written under.
        if m.func
          others = (by_func[[m.klass, m.singleton, m.func]] || [])
                   .map(&:name).uniq - [m.name]
          m.shared_with = ((m.shared_with || []) | others).sort unless others.empty?
        end

        s = (scanned[[r.klass, r.singleton, r.name]] || []).find { |c| c.func == r.func }
        next m unless s

        m.file, m.line, m.via = s.file, s.line, s.via
        m.alias_of ||= s.alias_of
        m
      end

      Index.new(merged, producer: "#{runtime.producer}+#{source.producer}")
    end

    # func name => "path:line" for every C function the binary has debug info
    # for.  build_config/host-gprof.rb enables debug info, so a -pg build
    # carries this.
    #
    # A function's own location is not the same fact as where its method was
    # registered -- in src/string.c the two sit 2300 lines apart -- and it is
    # not a property of the index's producer either.  It comes from the
    # build, which is why this is an annotation rather than a field any
    # producer fills in.
    #
    # A static function name can occur in more than one file.  Such names are
    # dropped rather than resolved to whichever came first: for a tool whose
    # output is "go read this line", a confident wrong file is worse than no
    # file at all.
    def self.definition_sites(binary)
      return {} unless binary && File.file?(binary)

      out = IO.popen(["nm", "-l", "--defined-only", binary], err: File::NULL, &:read)
      sites = {}
      ambiguous = []
      out.to_s.each_line do |line|
        head, where = line.chomp.split("\t", 2)
        next if where.nil? || where.empty?

        name = head.to_s.split(/\s+/).last
        path, _, lineno = where.rpartition(":")
        next if name.nil? || path.empty? || lineno !~ /\A\d+\z/

        site = [path.sub(%r{\A#{Regexp.escape(ROOT)}/}, ""), lineno.to_i]
        ambiguous << name if sites.key?(name) && sites[name] != site
        sites[name] = site
      end
      ambiguous.uniq.each { |n| sites.delete(n) }
      sites
    rescue SystemCallError
      {}
    end

    # Fill in def_file/def_line from a binary.  Returns the index.
    def self.annotate_definitions!(index, binary)
      sites = definition_sites(binary)
      index.registrations.each do |r|
        site = sites[r.func]
        r.def_file, r.def_line = site if site
      end
      index
    end

    # Shared by this CLI and method_uftrace.rb --list so the two cannot drift.
    def self.render_table(rows, out = $stdout)
      kw = rows.map { |r| r.key.length }.max
      fw = rows.map { |r| r.entry.length }.max
      # The registration column is only padded once there is a column after
      # it to line up against.
      defs = rows.any?(&:definition)
      lw = defs ? rows.map { |r| r.location.to_s.length }.max : 0
      rows.each do |r|
        note = +""
        note << "  (aliased from #{r.alias_of})" if r.alias_of
        note << "  (shared with #{r.shared_with.join(', ')})" if r.shared_with
        where = defs ? "  #{r.location.to_s.ljust(lw)}" : (r.location ? "  #{r.location}" : "")
        defn = r.definition ? "  #{r.definition}" : ""
        out.puts format("%-#{kw}s  %-#{fw}s%s%s%s", r.key, r.entry, where, defn, note).rstrip
      end
      out.puts "\n#{rows.size} methods, #{rows.map(&:klass).uniq.size} classes"
    end

    def self.render_tsv(rows, out = $stdout)
      rows.each do |r|
        out.puts [r.key, r.entry, r.kind, r.location, r.definition, r.via,
                  r.alias_of, r.shared_with&.join(" ")].map(&:to_s).join("\t")
      end
    end
  end
end

# --------------------------------------------------------------- C scanner

module MRuby
  module MethodIndex
    # Builds an index by scanning the C sources for method registrations.
    #
    # This is a deliberate approximation: the sources are read as text, with
    # no preprocessor and no scope analysis.  It cannot see which branches of
    # a #if survive, and it infers the owning class of a ROM table by
    # following the local `struct RClass *` variable back to its origin.  The
    # class a table is installed on only truly exists at run time, so this
    # producer is the one place in the tool that guesses.
    #
    # Both registration styles are understood:
    #
    #   MRB_MT_ENTRY(mrb_str_aref_m, MRB_OPSYM(aref), MRB_ARGS_ANY())  ROM tables
    #   mrb_define_method_id(mrb, cls, MRB_SYM(match), regexp_match, ...)
    class SourceScanner
      # struct mrb_state fields that hold a well-known class.
      MRB_STATE_CLASSES = {
        "object_class"         => "Object",
        "class_class"          => "Class",
        "module_class"         => "Module",
        "proc_class"           => "Proc",
        "string_class"         => "String",
        "array_class"          => "Array",
        "hash_class"           => "Hash",
        "range_class"          => "Range",
        "float_class"          => "Float",
        "integer_class"        => "Integer",
        "true_class"           => "TrueClass",
        "false_class"          => "FalseClass",
        "nil_class"            => "NilClass",
        "symbol_class"         => "Symbol",
        "kernel_module"        => "Kernel",
        "eException_class"     => "Exception",
        "eStandardError_class" => "StandardError",
      }.freeze

      # (mrb, MRB_SYM(Name), ...) -> Name
      TOPLEVEL_CTORS = %w[
        mrb_define_class_id mrb_define_class
        mrb_define_module_id mrb_define_module
        mrb_class_get_id mrb_class_get
        mrb_module_get_id mrb_module_get
      ].freeze

      # (mrb, outer, MRB_SYM(Name), ...) -> Outer::Name
      NESTED_CTORS = %w[
        mrb_define_class_under_id mrb_define_class_under
        mrb_define_module_under_id mrb_define_module_under
        mrb_class_get_under_id mrb_class_get_under
        mrb_module_get_under_id mrb_module_get_under
      ].freeze

      # (mrb, cls, sym, func, aspec)
      INSTANCE_DEFINERS = %w[
        mrb_define_method_id mrb_define_method
        mrb_define_private_method_id mrb_define_private_method
      ].freeze

      CTORS = (TOPLEVEL_CTORS + NESTED_CTORS + %w[mrb_singleton_class_ptr]).freeze

      SINGLETON_DEFINERS = %w[
        mrb_define_class_method_id mrb_define_class_method
        mrb_define_module_function_id mrb_define_module_function
        mrb_define_singleton_method_id mrb_define_singleton_method
      ].freeze

      attr_reader :registrations

      def initialize(files)
        @registrations = []
        files.each { |f| scan_file(f) }
      end

      private

      def scan_file(path)
        text = File.read(path, encoding: Encoding::UTF_8)
        return unless text.include?("MRB_MT_ENTRY") || text.include?("mrb_define_")

        @path = path.sub(%r{\A#{Regexp.escape(ROOT)}/}, "")
        @text = text
        @line_starts = [0]
        text.scan(/\n/) { @line_starts << Regexp.last_match.end(0) }

        @bindings = Hash.new { |h, k| h[k] = [] }   # var => [[line, class_desc]]
        collect_bindings
        tables = collect_rom_tables
        apply_rom_tables(tables)
        collect_definers
        collect_aliases
      rescue Errno::ENOENT, ArgumentError => e
        warn "skipping #{path}: #{e.message}"
      end

      def add(klass:, singleton:, name:, func:, line:, via:, alias_of: nil)
        @registrations << Registration.new(
          klass: klass, singleton: singleton, name: name, func: func,
          kind: "cfunc", file: @path, line: line, via: via, alias_of: alias_of
        )
      end

      def line_at(offset)
        @line_starts.bsearch_index { |s| s > offset } || @line_starts.size
      end

      # Split the arguments of the call whose '(' sits at open_idx.
      def call_args(text, open_idx)
        depth = 0
        args = []
        cur = +""
        i = open_idx
        while i < text.length
          ch = text[i]
          case ch
          when "("
            depth += 1
            cur << ch if depth > 1
          when ")"
            depth -= 1
            if depth.zero?
              args << cur.strip
              return [args, i]
            end
            cur << ch
          when ","
            if depth == 1
              args << cur.strip
              cur = +""
            else
              cur << ch
            end
          when '"'
            j = i + 1
            j += 1 while j < text.length && !(text[j] == '"' && text[j - 1] != "\\")
            cur << text[i..j]
            i = j
          else
            cur << ch
          end
          i += 1
        end
        [nil, nil]
      end

      # MRB_SYM(foo) / MRB_SYM_B(foo!) / MRB_OPSYM(aref) / "foo" -> Ruby name
      def sym_name(arg)
        case arg.strip
        when /\AMRB_SYM\(\s*([^)\s]+)\s*\)\z/   then Regexp.last_match(1)
        when /\AMRB_SYM_B\(\s*([^)\s]+)\s*\)\z/ then "#{Regexp.last_match(1)}!"
        when /\AMRB_SYM_Q\(\s*([^)\s]+)\s*\)\z/ then "#{Regexp.last_match(1)}?"
        when /\AMRB_SYM_E\(\s*([^)\s]+)\s*\)\z/ then "#{Regexp.last_match(1)}="
        when /\AMRB_OPSYM\(\s*([^)\s]+)\s*\)\z/ then OPSYM_TO_OP[Regexp.last_match(1)]
        when /\A"((?:[^"\\]|\\.)*)"\z/          then Regexp.last_match(1)
        end
      end

      # Record `var = <class producing call>` so later uses of var resolve.
      def collect_bindings
        CTORS.each do |ctor|
          scan_calls(ctor) do |args, name_idx, open_idx, _close_idx|
            desc = decode_ctor(ctor, args, line_at(open_idx))
            next unless desc

            bind_lhs(name_idx, desc)
          end
        end

        # `struct RClass *s = mrb->string_class;`
        @text.scan(/mrb->(\w+)/) do
          name = MRB_STATE_CLASSES[Regexp.last_match(1)]
          next unless name

          bind_lhs(Regexp.last_match.begin(0), { name: name, singleton: false })
        end

        # `mrb->object_class = obj;` -- the class field is filled in from a local.
        @text.scan(/mrb->(\w+)\s*=\s*([A-Za-z_]\w*)\s*;/) do
          name = MRB_STATE_CLASSES[Regexp.last_match(1)]
          next unless name

          @bindings[Regexp.last_match(2)] <<
            [line_at(Regexp.last_match.begin(0)), { name: name, singleton: false }]
        end

        # Bootstrap classes come from helpers with no symbol argument
        # (`bob = boot_defclass(...)`), so fall back to the declaration comment:
        #   struct RClass *bob;   /* BasicObject */
        # Declarations sit above every real binding, so this never shadows one.
        @text.scan(%r{struct\s+RClass\s*\*+\s*([A-Za-z_]\w*)\s*;[^\S\n]*/\*[^\S\n]*([A-Z]\w*)[^\S\n]*\*/}) do
          @bindings[Regexp.last_match(1)] <<
            [line_at(Regexp.last_match.begin(0)),
             { name: Regexp.last_match(2), singleton: false }]
        end

        # Stable: same-line bindings keep the order they were discovered in.
        @bindings.each_value do |v|
          v.replace(v.each_with_index.sort_by { |(line, _), i| [line, i] }.map(&:first))
        end
      end

      # Bind the assignment targets of `a = b = <expr>` when <expr> starts at
      # rhs_idx.  Anything that is merely an argument of a larger call has
      # non-assignment text in front of it and is ignored.
      def bind_lhs(rhs_idx, desc)
        bol = (@text.rindex("\n", rhs_idx) || -1) + 1
        prefix = @text[bol...rhs_idx]
        return unless prefix =~ /=\s*\z/

        line = line_at(rhs_idx)
        prefix.scan(/([A-Za-z_]\w*)\s*=(?!=)/) { @bindings[Regexp.last_match(1)] << [line, desc] }
      end

      def scan_calls(name)
        pos = 0
        pattern = /\b#{Regexp.escape(name)}\s*\(/
        while (m = pattern.match(@text, pos))
          open_idx = m.end(0) - 1
          args, close_idx = call_args(@text, open_idx)
          if args
            yield args, m.begin(0), open_idx, close_idx
            pos = close_idx + 1
          else
            pos = m.end(0)
          end
        end
      end

      # ctor(...) already split into args -> {name:, singleton:}
      def decode_ctor(ctor, args, line, depth = 0)
        if ctor == "mrb_singleton_class_ptr"
          inner = (args[1] || "").sub(/\Amrb_obj_value\s*\((.*)\)\z/m, '\1')
          desc = resolve_class(inner, line, depth + 1)
          return desc && { name: desc[:name], singleton: true }
        end

        if TOPLEVEL_CTORS.include?(ctor)
          name = sym_name(args[1].to_s)
          return name && { name: name, singleton: false }
        end

        outer = resolve_class(args[1].to_s, line, depth + 1)
        name = sym_name(args[2].to_s)
        return nil unless outer && name

        { name: "#{outer[:name]}::#{name}", singleton: false }
      end

      def resolve_class(expr, line, depth = 0)
        return nil if depth > 4

        expr = expr.to_s.strip
        return nil if expr.empty?

        if expr =~ /\Amrb->(\w+)\z/
          name = MRB_STATE_CLASSES[Regexp.last_match(1)]
          return name && { name: name, singleton: false }
        end

        CTORS.each do |ctor|
          next unless expr =~ /\A#{Regexp.escape(ctor)}\s*\(/

          args, = call_args(expr, expr.index("("))
          return args && decode_ctor(ctor, args, line, depth)
        end

        return nil unless expr =~ /\A[A-Za-z_]\w*\z/

        candidates = @bindings[expr]
        return nil if candidates.empty?

        # Nearest binding at or above the use site; otherwise the first one.
        before = candidates.select { |l, _| l <= line }
        (before.last || candidates.first)[1]
      end

      # name => [{func:, name:, line:, ...}]
      def collect_rom_tables
        tables = {}
        @text.scan(/static\s+const\s+mrb_mt_entry\s+(\w+)\s*\[\s*\]\s*=\s*\{/) do
          table = Regexp.last_match(1)
          body_start = Regexp.last_match.end(0)
          depth = 1
          i = body_start
          while i < @text.length && depth > 0
            case @text[i]
            when "{" then depth += 1
            when "}" then depth -= 1
            end
            i += 1
          end
          body = @text[body_start...(i - 1)]
          offset = body_start

          entries = []
          body.scan(/MRB_MT_ENTRY\s*\(\s*([A-Za-z_]\w*)\s*,\s*(MRB_\w+\([^)]*\))\s*,/) do
            func = Regexp.last_match(1)
            name = sym_name(Regexp.last_match(2))
            next unless name

            entries << {
              func: func,
              name: name,
              line: line_at(offset + Regexp.last_match.begin(0)),
            }
          end
          tables[table] = entries
        end
        tables
      end

      def apply_rom_tables(tables)
        used = {}
        scan_calls("MRB_MT_INIT_ROM") do |args, _name_idx, open_idx, _|
          table = args[2].to_s.strip
          entries = tables[table]
          next unless entries

          desc = resolve_class(args[1].to_s, line_at(open_idx))
          next unless desc

          used[table] = true
          entries.each do |e|
            add(klass: desc[:name], singleton: desc[:singleton],
                name: e[:name], func: e[:func], line: e[:line], via: "MRB_MT_ENTRY")
          end
        end

        (tables.keys - used.keys).each do |table|
          warn "#{@path}: ROM table #{table} is never installed by MRB_MT_INIT_ROM"
        end
      end

      def collect_definers
        (INSTANCE_DEFINERS + SINGLETON_DEFINERS).each do |definer|
          singleton = SINGLETON_DEFINERS.include?(definer)
          scan_calls(definer) do |args, _name_idx, open_idx, _|
            next if args.size < 4

            line = line_at(open_idx)
            desc = resolve_class(args[1].to_s, line)
            name = sym_name(args[2].to_s)
            func = args[3].to_s.strip
            next unless desc && name && func =~ /\A[A-Za-z_]\w*\z/

            add(klass: desc[:name], singleton: singleton || desc[:singleton],
                name: name, func: func, line: line, via: definer)
          end
        end
      end

      def collect_aliases
        %w[mrb_define_alias_id mrb_define_alias].each do |definer|
          scan_calls(definer) do |args, _name_idx, open_idx, _|
            next if args.size < 4

            line = line_at(open_idx)
            desc = resolve_class(args[1].to_s, line)
            new_name = sym_name(args[2].to_s)
            old_name = sym_name(args[3].to_s)
            next unless desc && new_name && old_name

            target = @registrations.find do |r|
              r.klass == desc[:name] && r.singleton == desc[:singleton] && r.name == old_name
            end
            next unless target

            add(klass: desc[:name], singleton: desc[:singleton],
                name: new_name, func: target.func, line: line,
                via: definer, alias_of: old_name)
          end
        end
      end
    end
  end
end

# -------------------------------------------------------- runtime producer

module MRuby
  module MethodIndex
    # Builds an index from a build's own method tables.
    #
    # mrbgems/mruby-bin-mtdump opens an mrb_state and writes out every class's
    # table; this reads that back.  Nothing here is inferred: the owning class
    # is the table the method is in, a method exists only if this build
    # registered it, and an alias is an entry rather than a name to match up.
    #
    # The one thing that does need work is turning a function pointer into a
    # name.  The dump reports runtime addresses, which say nothing on their
    # own once the loader has placed a position-independent executable
    # somewhere, so it also reports the runtime address of one named anchor
    # function.  The difference between the two is the load bias, and
    # subtracting it puts every other address back where `nm` can name it.
    #
    # Addresses are resolved against the dumper's own binary, not against
    # bin/mruby: they are two links of the same library and place the same
    # functions differently.  Only the resulting names cross between them.
    class RuntimeDump
      DUMP_VERSION = 2

      # nm's letters for code: text, and weak text.
      CODE_TYPES = "tTwW"

      ESCAPES = { "t" => "\t", "n" => "\n", "r" => "\r", "\\" => "\\" }.freeze

      attr_reader :registrations, :stats, :unresolved

      # `dump` is for feeding in text that was captured earlier; by default
      # the binary is run.
      def initialize(binary, dump: nil)
        @binary = binary
        @registrations = []
        @unresolved = []
        @stats = {}
        parse(dump || capture(binary))
      end

      private

      def capture(binary)
        unless File.file?(binary) && File.executable?(binary)
          raise ArgumentError, <<~MSG.chomp
            method table dumper not found: #{binary}
            Build one with:  MRUBY_CONFIG=host-gprof rake
          MSG
        end

        out = IO.popen([binary], &:read)
        raise ArgumentError, "#{binary} exited #{$?.exitstatus}" unless $?.success?

        out
      end

      def unescape(s)
        s.to_s.gsub(/\\(.)/) { ESCAPES[Regexp.last_match(1)] || Regexp.last_match(1) }
      end

      def relative(path)
        path.sub(%r{\A#{Regexp.escape(ROOT)}/}, "")
      end

      def parse(text)
        version = nil
        anchor = nil
        rows = []

        text.each_line do |line|
          fields = line.chomp.split("\t", -1)
          case fields[0]
          when "!mtdump"
            version = fields[1].to_i
          when "!anchor"
            anchor = [fields[1], Integer(fields[2], 16)]
          when "!stats"
            fields.drop(1).each do |f|
              k, _, v = f.partition("=")
              @stats[k] = v.to_i
            end
          when "m"
            rows << fields
          end
        end

        unless version == DUMP_VERSION
          raise ArgumentError,
                "unsupported mtdump version #{version.inspect} (want #{DUMP_VERSION})"
        end
        raise ArgumentError, "#{@binary}: dump has no !anchor line" unless anchor
        raise ArgumentError, "#{@binary}: dump has no methods" if rows.empty?

        by_addr, by_name = symbols
        base = by_name[anchor[0]]
        unless base
          raise ArgumentError,
                "#{@binary}: anchor symbol #{anchor[0]} is not in the symbol table; " \
                "the binary is stripped, or is not the one that produced the dump"
        end
        bias = anchor[1] - base

        rows.each { |fields| @registrations << record(fields, by_addr, bias) }
      end

      def record(fields, by_addr, bias)
        _, klass, sep, name, kind, target, origin, alias_of = fields
        reg = Registration.new(
          klass: unescape(klass), singleton: sep == ".", name: unescape(name),
          kind: kind, rom: origin == "rom" ? true : origin == "heap" ? false : nil,
          alias_of: alias_of == "-" ? nil : unescape(alias_of)
        )
        target = unescape(target)

        # An alias is already reported as the body it resolves to, so these
        # are the only kinds that carry one.
        case kind
        when "cfunc"
          sym = by_addr[Integer(target, 16) - bias]
          if sym
            reg.func = sym
          else
            # Every entry in a -g build should be named.  Keep the record --
            # the method does exist -- and say so rather than drop it.
            @unresolved << "#{reg.key} #{target}"
          end
        when "proc"
          if target =~ /\A(.*):(\d+)\z/
            reg.def_file = relative(Regexp.last_match(1))
            reg.def_line = Regexp.last_match(2).to_i
          end
        end

        reg
      end

      # [addr => name, name => addr] for the code symbols of @binary.
      def symbols
        by_addr = {}
        by_name = {}
        out = IO.popen(["nm", "--defined-only", @binary], err: File::NULL, &:read)
        out.to_s.each_line do |line|
          addr, type, name = line.split(" ", 3)
          next unless name && addr =~ /\A\h+\z/ && type.length == 1 && CODE_TYPES.include?(type)

          name = name.strip
          a = addr.to_i(16)
          by_name[name] ||= a
          cur = by_addr[a]
          by_addr[a] = name if cur.nil? || better_symbol?(name, cur)
        end
        [by_addr, by_name]
      rescue SystemCallError => e
        raise ArgumentError, "cannot read the symbol table of #{@binary}: #{e.message}"
      end

      # Two names can sit at one address -- a weak alias, or an optimizer
      # clone next to the function it came from.  Prefer the plain name, and
      # otherwise pick by a rule rather than by whichever nm printed first, so
      # that two runs agree.
      def better_symbol?(a, b)
        rank = ->(n) { [n.include?(".") ? 1 : 0, n.length, n] }
        (rank.call(a) <=> rank.call(b)) < 0
      end
    end
  end
end

# ------------------------------------------------------------------- CLI

if $PROGRAM_NAME == __FILE__
  require "optparse"

  opts = {
    format: "table", klass: nil, grep: nil, sources: nil, index: nil,
    binary: nil, producer: false, runtime: nil, merge: false,
  }

  OptionParser.new do |o|
    o.banner = "Usage: ruby tools/mruby_method_index.rb [options]"
    o.on("--format=FMT", %w[table json tsv], "table (default), json, or tsv") { |v| opts[:format] = v }
    o.on("--class=NAME", "only this class") { |v| opts[:klass] = v }
    o.on("--grep=PATTERN", "filter by method name or C function") { |v| opts[:grep] = v }
    o.on("--sources=GLOBS", "comma-separated C source globs") { |v| opts[:sources] = v.split(",") }
    o.on("--index=PATH", "read a previously written JSON index instead of scanning") { |v| opts[:index] = v }
    o.on("--runtime[=PATH]", "build the index from a build's method tables",
         "(default: #{MRuby::MethodIndex::DEFAULT_MTDUMP.sub(MRuby::MethodIndex::ROOT + '/', '')})") do |v|
      opts[:runtime] = v || MRuby::MethodIndex::DEFAULT_MTDUMP
    end
    o.on("--merge", "take the mapping from --runtime and the registration sites from the source scan") do
      opts[:merge] = true
      opts[:runtime] ||= MRuby::MethodIndex::DEFAULT_MTDUMP
    end
    o.on("--producer", "print which producer made the index, then exit") { opts[:producer] = true }
    o.on("--binary=PATH", "add each C function's definition site from this build's debug info") { |v| opts[:binary] = v }
    o.on("-h", "--help") { puts o; exit 0 }
  end.parse!

  abort "--index already holds a built index; --runtime would discard it" if opts[:index] && opts[:runtime]

  index =
    begin
      sources = opts[:sources] || MRuby::MethodIndex::DEFAULT_SOURCES
      if opts[:index]
        MRuby::MethodIndex.from_json(opts[:index])
      elsif opts[:merge]
        MRuby::MethodIndex.merge(
          MRuby::MethodIndex.from_runtime(opts[:runtime]),
          MRuby::MethodIndex.from_source(sources)
        )
      elsif opts[:runtime]
        MRuby::MethodIndex.from_runtime(opts[:runtime])
      else
        MRuby::MethodIndex.from_source(sources)
      end
    rescue ArgumentError, Errno::ENOENT, JSON::ParserError => e
      abort e.message
    end

  if opts[:producer]
    puts "#{index.producer || 'unknown'}\t#{index.registrations.size} methods"
    exit 0
  end

  if opts[:binary]
    abort "no such binary: #{opts[:binary]}" unless File.file?(opts[:binary])

    MRuby::MethodIndex.annotate_definitions!(index, opts[:binary])
  end

  rows = index.select(klass: opts[:klass], grep: opts[:grep])

  if opts[:format] == "json"
    puts JSON.pretty_generate(MRuby::MethodIndex::Index.new(rows, producer: index.producer).to_h)
    exit 0
  end

  if rows.empty?
    warn "nothing matched (known classes: #{index.classes.join(', ')})"
    exit 1
  end

  case opts[:format]
  when "tsv"   then MRuby::MethodIndex.render_tsv(rows)
  else              MRuby::MethodIndex.render_table(rows)
  end
end
