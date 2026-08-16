#!/usr/bin/env ruby
# frozen_string_literal: true

# Index of mruby methods and the C functions behind them.
#
# This is the lookup half of tools/method_uftrace.rb, split out so that the
# index can be produced, inspected, and compared on its own.
#
# An index is a list of Registration records.  Four fields are always present
# and identify the method; the rest are best-effort and depend on where the
# index came from:
#
#   klass singleton name func   always
#   kind                        "cfunc" or "proc" (Ruby-level), when known
#   file line via alias_of      where the registration was written, when known
#
# One producer exists today: SourceScanner, which reads the C sources.  It
# fills in the registration site but cannot see a build's actual
# configuration.  Consumers must therefore treat the optional fields as
# absent-by-default rather than assume this producer.
#
# Usage:
#
#   ruby tools/mruby_method_index.rb --class String
#   ruby tools/mruby_method_index.rb --grep index
#
# As a library:
#
#   require_relative "mruby_method_index"
#   index = MRuby::MethodIndex.from_source
#   index.lookup("String#[]")     # => [Registration, ...]

module MRuby
  module MethodIndex
    ROOT = File.expand_path("..", __dir__)

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

    # klass/singleton/name/func are the identity; everything else is optional
    # and may be nil depending on the producer.
    Registration = Struct.new(
      :klass, :singleton, :name, :func, :kind,
      :file, :line, :via, :alias_of,
      keyword_init: true
    ) do
      def key
        "#{klass}#{singleton ? '.' : '#'}#{name}"
      end

      # Registration site as "path:line", or nil when the producer has none.
      def location
        "#{file}:#{line}" if file && line
      end

    end

    # A resolved set of registrations plus the queries the drivers need.
    class Index
      attr_reader :registrations, :producer

      def initialize(registrations, producer:)
        @registrations = registrations.sort_by { |r| [r.klass, r.singleton ? 1 : 0, r.name] }
        @producer = producer
      end

      # key => [Registration, ...]
      def by_key
        @by_key ||= @registrations.group_by(&:key)
      end

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
          rows = rows.select { |r| r.name =~ re || r.func =~ re }
        end
        rows
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


    # Shared by this CLI and method_uftrace.rb --list so the two cannot drift.
    def self.render_table(rows, out = $stdout)
      kw = rows.map { |r| r.key.length }.max
      fw = rows.map { |r| r.func.length }.max
      rows.each do |r|
        note = r.alias_of ? "  (alias of #{r.alias_of})" : ""
        where = r.location ? "  #{r.location}" : ""
        out.puts format("%-#{kw}s  %-#{fw}s%s%s", r.key, r.func, where, note).rstrip
      end
      out.puts "\n#{rows.size} methods, #{rows.map(&:klass).uniq.size} classes"
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

# ------------------------------------------------------------------- CLI

if $PROGRAM_NAME == __FILE__
  require "optparse"

  opts = { klass: nil, grep: nil, sources: nil }

  OptionParser.new do |o|
    o.banner = "Usage: ruby tools/mruby_method_index.rb [options]"
    o.on("--class=NAME", "only this class") { |v| opts[:klass] = v }
    o.on("--grep=PATTERN", "filter by method name or C function") { |v| opts[:grep] = v }
    o.on("--sources=GLOBS", "comma-separated C source globs") { |v| opts[:sources] = v.split(",") }
    o.on("-h", "--help") { puts o; exit 0 }
  end.parse!

  index =
    begin
      MRuby::MethodIndex.from_source(opts[:sources] || MRuby::MethodIndex::DEFAULT_SOURCES)
    rescue ArgumentError => e
      abort e.message
    end

  rows = index.select(klass: opts[:klass], grep: opts[:grep])
  if rows.empty?
    warn "nothing matched (known classes: #{index.classes.join(', ')})"
    exit 1
  end

  MRuby::MethodIndex.render_table(rows)
end
