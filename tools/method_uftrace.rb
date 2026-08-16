#!/usr/bin/env ruby
# frozen_string_literal: true

# Trace the native C call tree under one mruby method using uftrace.
#
# The Ruby method name is resolved to its C entry point by indexing the method
# registrations in the C sources.  Both registration styles are understood:
#
#   MRB_MT_ENTRY(mrb_str_aref_m, MRB_OPSYM(aref), MRB_ARGS_ANY())  ROM tables
#   mrb_define_method_id(mrb, cls, MRB_SYM(match), regexp_match, ...)
#
# Methods are indexed per class, so `String#[]` and `MatchData#[]` stay
# distinct.
#
# Examples:
#
#   ruby tools/method_uftrace.rb --list --class String
#
#   ruby tools/method_uftrace.rb --method 'String#[]' --expr '"あいう"[1]'
#
#   ruby tools/method_uftrace.rb \
#     --method 'String#rindex' \
#     --expr '("a" * 100000).rindex("z")' \
#     --runs 20 --depth 8
#
# Output (under <out>/<class>/<method>/):
#   replay.txt  chronological nested calls
#   graph.txt   aggregated call graph
#   report.txt  per-function time and call count
#   meta.txt    what was traced, and how
#
# Requirements:
#   - Linux, uftrace in PATH
#   - an mruby binary built with -pg, unstripped.  build_config/host-gprof.rb
#     does this:  MRUBY_CONFIG=host-gprof rake
#
# Note on timings: a -pg/-O0 build is what makes the call tree visible, but its
# per-function times do not reflect an -O2 build (no inlining).  Use this to
# understand *which* code runs; use `perf record` on an -O2 build to decide
# what is worth optimizing.

require "optparse"
require "fileutils"
require "shellwords"

MRUBY_ROOT = File.expand_path("..", __dir__)

# ------------------------------------------------------------------ symbols

# Prefer the build system's own table so the two never drift.
OPERATORS =
  begin
    require File.join(MRUBY_ROOT, "lib", "mruby", "presym")
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

Registration = Struct.new(
  :klass, :singleton, :name, :func, :file, :line, :via, :alias_of,
  keyword_init: true
) do
  def key
    "#{klass}#{singleton ? '.' : '#'}#{name}"
  end
end

# ------------------------------------------------------------------ C index

class CIndex
  attr_reader :registrations

  def initialize(files)
    @registrations = []
    files.each { |f| scan_file(f) }
    @registrations.sort_by! { |r| [r.klass, r.singleton ? 1 : 0, r.name] }
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

  private

  def scan_file(path)
    text = File.read(path, encoding: Encoding::UTF_8)
    return unless text.include?("MRB_MT_ENTRY") || text.include?("mrb_define_")

    @path = path
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
        @registrations << Registration.new(
          klass: desc[:name], singleton: desc[:singleton],
          name: e[:name], func: e[:func],
          file: @path, line: e[:line], via: "MRB_MT_ENTRY"
        )
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

        @registrations << Registration.new(
          klass: desc[:name],
          singleton: singleton || desc[:singleton],
          name: name, func: func, file: @path, line: line, via: definer
        )
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

        @registrations << Registration.new(
          klass: desc[:name], singleton: desc[:singleton],
          name: new_name, func: target.func, file: @path, line: line,
          via: definer, alias_of: old_name
        )
      end
    end
  end
end

# ------------------------------------------------------------------- driver

opts = {
  bin: File.join(MRUBY_ROOT, "build", "host", "bin", "mruby"),
  method: nil,
  klass: nil,
  expr: nil,
  script: nil,
  out: File.join(MRUBY_ROOT, "uftrace-out"),
  runs: 1,
  warmup: 0,
  depth: nil,
  time_filter: nil,
  auto_args: false,
  nest_libcall: false,
  patch_all: true,
  force: false,
  dry_run: false,
  grep: nil,
  sources: ["src/*.c", "mrbgems/*/src/*.c"],
  list: false,
}

parser = OptionParser.new do |o|
  o.banner = "Usage: ruby tools/method_uftrace.rb [options]"

  o.on("--method=SPEC", "method to trace, e.g. 'String#[]', 'Regexp.escape', 'rindex'") { |v| opts[:method] = v }
  o.on("--class=NAME", "class scope for a bare --method / --list filter") { |v| opts[:klass] = v }
  o.on("--expr=RUBY", "Ruby expression to execute under the trace") { |v| opts[:expr] = v }
  o.on("--script=PATH", "Ruby script to execute instead of --expr") { |v| opts[:script] = v }
  o.on("--bin=PATH", "mruby executable (default: #{opts[:bin].sub(MRUBY_ROOT + '/', '')})") { |v| opts[:bin] = v }
  o.on("--out=DIR", "output root (default: #{opts[:out].sub(MRUBY_ROOT + '/', '')})") { |v| opts[:out] = v }
  o.on("--runs=N", Integer, "executions of the expression inside the trace (default: 1)") { |v| opts[:runs] = v }
  o.on("--warmup=N", Integer, "untraced executions in a separate process beforehand") { |v| opts[:warmup] = v }
  o.on("--depth=N", Integer, "limit nesting depth (uftrace -D)") { |v| opts[:depth] = v }
  o.on("--time-filter=T", "hide calls faster than T, e.g. 500ns (uftrace -t)") { |v| opts[:time_filter] = v }
  o.on("--auto-args", "record function arguments where possible") { opts[:auto_args] = true }
  o.on("--[no-]libcalls", "trace nested library calls such as malloc (default: no)") { |v| opts[:nest_libcall] = v }
  o.on("--[no-]patch-all", "use uftrace dynamic full patching -P. (default: yes)") { |v| opts[:patch_all] = v }
  o.on("--force", "record even if the binary looks uninstrumented") { opts[:force] = true }
  o.on("--dry-run", "resolve and print the uftrace command without running it") { opts[:dry_run] = true }
  o.on("--sources=GLOBS", "comma-separated C source globs") { |v| opts[:sources] = v.split(",") }
  o.on("--grep=PATTERN", "filter --list output by method name") { |v| opts[:grep] = v }
  o.on("--list", "list resolved methods and their C entry points") { opts[:list] = true }
  o.on("-h", "--help") { puts o; exit 0 }
end
parser.parse!

files = opts[:sources]
  .flat_map { |g| Dir.glob(File.absolute_path?(g) ? g : File.join(MRUBY_ROOT, g)) }
  .uniq.sort
abort "no source files matched #{opts[:sources].join(', ')}" if files.empty?

index = CIndex.new(files)

if opts[:list]
  rows = index.registrations
  rows = rows.select { |r| r.klass == opts[:klass] } if opts[:klass]
  if opts[:grep]
    re = Regexp.new(opts[:grep], Regexp::IGNORECASE)
    rows = rows.select { |r| r.name =~ re || r.func =~ re }
  end
  if rows.empty?
    warn "nothing matched (known classes: #{index.classes.join(', ')})"
    exit 1
  end
  kw = rows.map { |r| r.key.length }.max
  fw = rows.map { |r| r.func.length }.max
  rows.each do |r|
    note = r.alias_of ? "  (alias of #{r.alias_of})" : ""
    puts format("%-#{kw}s  %-#{fw}s  %s:%d%s",
                r.key, r.func, r.file.sub(MRUBY_ROOT + "/", ""), r.line, note)
  end
  puts "\n#{rows.size} methods, #{rows.map(&:klass).uniq.size} classes"
  exit 0
end

abort "--method is required (or use --list)" unless opts[:method]

matches = index.lookup(opts[:method], klass: opts[:klass])
if matches.empty?
  warn "method not resolved: #{opts[:method].inspect}"
  similar = index.similar(opts[:method])
  warn "similar: #{similar.join(', ')}" unless similar.empty?
  exit 2
end

unless matches.map(&:key).uniq.size == 1
  warn "#{opts[:method].inspect} is ambiguous; qualify it with a class:"
  matches.map(&:key).uniq.sort.each { |k| warn "  #{k}" }
  exit 2
end

# Several registrations can share a key (e.g. re-registered in a gem); the
# distinct C functions are what matter.
funcs = matches.map(&:func).uniq
if funcs.size > 1
  warn "#{matches.first.key} maps to multiple C functions: #{funcs.join(', ')}"
  warn "tracing the first; narrow with --sources if that is wrong"
end
info = matches.first
root = info.func

if opts[:expr] && opts[:script]
  abort "use only one of --expr or --script"
elsif !opts[:expr] && !opts[:script]
  abort "one of --expr or --script is required"
end

bin = File.expand_path(opts[:bin])
have_bin = File.file?(bin) && File.executable?(bin)
unless have_bin
  msg = <<~MSG.chomp
    mruby executable not found: #{bin}
    Build one with symbols and mcount instrumentation:
      MRUBY_CONFIG=host-gprof rake
  MSG
  abort msg unless opts[:dry_run]
  warn "warning: #{msg}"
end

# uftrace needs the symbol in the binary; -O2 can inline or clone it away.
def nm_symbols(bin)
  return nil unless system("command -v nm > /dev/null 2>&1")

  out = IO.popen(["nm", "-a", bin], err: File::NULL, &:read)
  # Drop glibc version suffixes: "mcount@GLIBC_2.2.5" -> "mcount".
  out.to_s.lines.filter_map { |l| l.split(/\s+/).last&.strip&.split("@")&.first }
rescue SystemCallError
  nil
end

symbols = have_bin ? nm_symbols(bin) : nil
if symbols
  if symbols.none? { |s| s == "mcount" || s == "_mcount" || s == "__fentry__" }
    msg = "#{opts[:bin]} has no mcount/__fentry__ symbols; it was not built with -pg."
    abort "#{msg}\nRebuild with: MRUBY_CONFIG=host-gprof rake  (or pass --force)" unless opts[:force]
    warn "warning: #{msg}"
  end
  unless symbols.include?(root)
    clones = symbols.grep(/\A#{Regexp.escape(root)}[.]/).uniq
    if clones.empty?
      warn "warning: #{root} is not in the symbol table of #{opts[:bin]}."
      warn "         It is probably inlined; an -O0 build (enable_debug) keeps it."
    else
      warn "note: #{root} was cloned by the optimizer: #{clones.join(', ')}"
      warn "      tracing #{clones.first} instead"
      root = clones.first
    end
  end
end

safe_class = info.klass.gsub("::", ".").gsub(/[^A-Za-z0-9_.]+/, "_")
safe_method = info.name
  .gsub("[]=", "aset").gsub("[]", "aref")
  .gsub("<=>", "cmp").gsub("===", "eqq").gsub("==", "eq").gsub("=~", "match")
  .gsub("<<", "lshift").gsub(">>", "rshift")
  .gsub("!", "_bang").gsub("?", "_p").gsub("=", "_set")
  .gsub(/[^A-Za-z0-9_.-]+/, "_")
  .gsub(/\A_+|_+\z/, "")
safe_method = info.singleton ? "s_#{safe_method}" : safe_method
safe_method = "method" if safe_method.empty?

outdir = File.expand_path(File.join(opts[:out], safe_class, safe_method))
datadir = File.join(outdir, "data")
runner = File.join(outdir, "case.rb")

record_cmd = ["uftrace", "record"]
record_cmd << "-P." if opts[:patch_all]
record_cmd << "-l" if opts[:nest_libcall]
record_cmd << "-a" if opts[:auto_args]
record_cmd += ["-D", opts[:depth].to_s] if opts[:depth]
record_cmd += ["-t", opts[:time_filter]] if opts[:time_filter]
record_cmd += ["-F", root, "-d", datadir, "--", bin, runner]

puts "Method      : #{info.key}"
puts "C root      : #{root}#{root == info.func ? '' : " (was #{info.func})"}"
puts "Registered  : #{info.file.sub(MRUBY_ROOT + '/', '')}:#{info.line} via #{info.via}"
puts "Alias of    : #{info.alias_of}" if info.alias_of
puts "Binary      : #{bin}"
puts "Expression  : #{opts[:expr]}" if opts[:expr]
puts "Output      : #{outdir}"

if opts[:dry_run]
  puts "Would run   : #{record_cmd.shelljoin}"
  exit 0
end

abort "uftrace not found in PATH" unless system("command -v uftrace > /dev/null 2>&1")

FileUtils.rm_rf(outdir)
FileUtils.mkdir_p(datadir)

# The runner is kept next to the trace so the run can be repeated or
# disassembled afterwards.
body =
  if opts[:script]
    File.read(opts[:script])
  elsif opts[:runs] == 1
    "#{opts[:expr]}\n"
  else
    "#{opts[:runs]}.times do\n#{opts[:expr]}\nend\n"
  end
File.write(runner, body)

# Untraced and in a separate process: this warms the page cache and the
# dynamic linker only.  Nothing inside the traced process is warmed by it;
# use --runs for that.
if opts[:warmup] > 0 && !opts[:script]
  warmup = File.join(outdir, "warmup.rb")
  File.write(warmup, "#{opts[:warmup]}.times do\n#{opts[:expr]}\nend\n")
  puts "Warm-up     : #{opts[:warmup]} untraced executions"
  abort "warm-up run failed" unless system(bin, warmup)
end

puts "Recording   : #{record_cmd.shelljoin}"
abort "uftrace record failed" unless system(*record_cmd)

{
  "replay.txt" => ["uftrace", "replay", "-d", datadir, "-F", root, "--srcline"],
  "graph.txt"  => ["uftrace", "graph", "-d", datadir, root],
  "report.txt" => ["uftrace", "report", "-d", datadir],
}.each do |name, c|
  path = File.join(outdir, name)
  err = "#{path}.stderr"
  warn "#{c.shelljoin} failed" unless system(*c, out: path, err: err)
  File.delete(err) if File.exist?(err) && File.size(err).zero?
end

File.write(File.join(outdir, "meta.txt"), [
  "method=#{info.key}",
  "c_root=#{root}",
  "c_func=#{info.func}",
  "registration=#{info.file.sub(MRUBY_ROOT + '/', '')}:#{info.line}",
  "via=#{info.via}",
  ("alias_of=#{info.alias_of}" if info.alias_of),
  ("expr=#{opts[:expr]}" if opts[:expr]),
  ("script=#{opts[:script]}" if opts[:script]),
  "runs=#{opts[:runs]}",
  "binary=#{bin}",
  "record=#{record_cmd.shelljoin}",
].compact.join("\n") + "\n")

replay = File.join(outdir, "replay.txt")
if File.exist?(replay) && File.size(replay) < 64
  mrbc = File.join(File.dirname(bin), "mrbc")
  warn ""
  warn "replay.txt is empty: #{root} was never entered.  Likely causes:"
  warn "  - the VM handles the operation with a dedicated opcode and never"
  warn "    sends the method.  String#[] with an integer, for example, is"
  warn "    served by OP_GETIDX in src/vm.c.  Check the bytecode with:"
  warn "      #{mrbc.sub(MRUBY_ROOT + '/', '')} -v #{runner.sub(MRUBY_ROOT + '/', '')}"
  warn "  - the expression dispatches to a different class than #{info.klass}"
  warn "  - #{root} was inlined away; rebuild with enable_debug (-O0)"
end

puts
puts "Wrote:"
%w[replay.txt graph.txt report.txt meta.txt case.rb].each { |n| puts "  #{File.join(outdir, n)}" }
