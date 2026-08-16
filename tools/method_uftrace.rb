#!/usr/bin/env ruby
# frozen_string_literal: true

# Trace the native C call tree under one mruby method using uftrace.
#
# The Ruby method name is resolved to its C entry point through
# tools/mruby_method_index.rb, which is where all knowledge of how methods are
# registered lives.  Methods are indexed per class, so `String#[]` and
# `MatchData#[]` stay distinct.
#
# This driver only consumes the index, and treats everything but the class and
# method name as optional -- an index built from a running VM knows no
# registration site, and a method written in Ruby has no C function at all.
# Pass --index to read one that was written out earlier.
#
# By default the index is built by reading the C sources, which describe every
# build at once and so describe none of them exactly.  --runtime asks this
# build's own method tables instead, and --merge takes the mapping from those
# and the registration sites from the sources.  Either needs
# build/host/bin/mruby-mtdump, which MRUBY_CONFIG=host-gprof rake builds.
#
# Examples:
#
#   ruby tools/method_uftrace.rb --list --class String
#
#   ruby tools/method_uftrace.rb --method 'String#[]' --expr '"あいう"[1]'
#
#   ruby tools/method_uftrace.rb --merge --method 'String#__aref' \
#     --expr '"あいう"[1]'
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
require_relative "mruby_method_index"

MethodIndex = MRuby::MethodIndex

MRUBY_ROOT = MethodIndex::ROOT

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
  sources: MethodIndex::DEFAULT_SOURCES,
  index: nil,
  runtime: nil,
  merge: false,
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
  o.on("--sources=GLOBS", "comma-separated C source globs") { |v| opts[:sources] = v.split(","); opts[:sources_given] = true }
  o.on("--index=PATH", "use a JSON index (tools/mruby_method_index.rb --format json)") { |v| opts[:index] = v }
  o.on("--runtime[=PATH]", "resolve through this build's method tables",
       "(default: #{MethodIndex::DEFAULT_MTDUMP.sub(MRUBY_ROOT + '/', '')})") do |v|
    opts[:runtime] = v || MethodIndex::DEFAULT_MTDUMP
  end
  o.on("--merge", "take the mapping from --runtime and the registration sites from the sources") do
    opts[:merge] = true
    opts[:runtime] ||= MethodIndex::DEFAULT_MTDUMP
  end
  o.on("--grep=PATTERN", "filter --list output by method name") { |v| opts[:grep] = v }
  o.on("--list", "list resolved methods and their C entry points") { opts[:list] = true }
  o.on("-h", "--help") { puts o; exit 0 }
end
parser.parse!

abort "--sources has no effect with --index; the index is already built" if opts[:index] && opts[:sources_given]
abort "--sources has no effect with --runtime; the tables are read, not scanned" if opts[:runtime] && !opts[:merge] && opts[:sources_given]
abort "--index already holds a built index; --runtime would discard it" if opts[:index] && opts[:runtime]

index =
  begin
    if opts[:index]
      MethodIndex.from_json(opts[:index])
    elsif opts[:merge]
      MethodIndex.merge(MethodIndex.from_runtime(opts[:runtime]),
                        MethodIndex.from_source(opts[:sources]))
    elsif opts[:runtime]
      MethodIndex.from_runtime(opts[:runtime])
    else
      MethodIndex.from_source(opts[:sources])
    end
  rescue ArgumentError, Errno::ENOENT, JSON::ParserError => e
    abort e.message
  end

if opts[:list]
  rows = index.select(klass: opts[:klass], grep: opts[:grep])
  if rows.empty?
    warn "nothing matched (known classes: #{index.classes.join(', ')})"
    exit 1
  end
  MethodIndex.render_table(rows)
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
funcs = matches.map(&:func).compact.uniq
if funcs.size > 1
  warn "#{matches.first.key} maps to multiple C functions: #{funcs.join(', ')}"
  warn "tracing the first; narrow with --sources if that is wrong"
end
info = matches.find(&:func) || matches.first

# uftrace records C functions.  A method that has none is not a failure to
# resolve, and saying "not found" about it would send the reader looking for a
# symbol that was never supposed to exist.
unless info.func
  warn "#{info.key} has no C entry point."
  case info.kind
  when "proc"
    warn "It is written in Ruby#{info.definition ? ", in #{info.definition}" : ''}, so the VM runs"
    warn "bytecode for it and there is no function for uftrace to record.  Trace"
    warn "one of the methods it calls, or --list the class to find the C half:"
    warn "mruby-regexp, for one, keeps String#[]'s C function under String#__aref."
  when "undef"
    warn "It is undefined in this build; the table entry exists only to stop lookup."
  else
    warn "The index does not name one."
  end
  exit 2
end

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
      if index.producer == "c-source-scan"
        warn "         This index is built from the C sources, which do not say what"
        warn "         this build configured: the function may be excluded by a #if,"
        warn "         or belong to a gem this gembox leaves out.  Kernel#p, for one,"
        warn "         is compiled out whenever mruby-io is present.  Pass --runtime"
        warn "         to resolve through this build's own method tables instead."
      else
        warn "         The index was built from a different binary than the one being"
        warn "         traced; rebuild both with the same config."
      end
      warn "         Failing that it was inlined, which an -O0 build (enable_debug) keeps."
    else
      warn "note: #{root} was cloned by the optimizer: #{clones.join(', ')}"
      warn "      tracing #{clones.first} instead"
      root = clones.first
    end
  end
end

# Where the C function is written, as opposed to where the method was
# registered.  In src/string.c the two sit 2300 lines apart, and this is the
# one you want open while reading a trace.
MethodIndex.annotate_definitions!(index, bin) if have_bin

safe_class = info.klass.gsub("::", ".").gsub(/[^A-Za-z0-9_.]+/, "_")
# Operators get the same directory name mruby's own presym table gives them,
# so String#[] lands in aref and Integer#** in pow.
# Underscores the name itself carries are kept: gems park the C half of a
# method under a __-prefixed name -- String#rindex is Ruby in a full-core
# build and its C function answers to String#__rindex -- and trimming them
# would drop the two traces in the same directory.
safe_method = MethodIndex::OPERATORS[info.name] ||
  info.name
    .gsub("!", "_bang").gsub("?", "_p").gsub("=", "_set")
    .gsub(/[^A-Za-z0-9_.-]+/, "_")
safe_method = "s_#{safe_method}" if info.singleton
safe_method = "method" if safe_method.empty?

outdir = File.expand_path(File.join(opts[:out], safe_class, safe_method))
datadir = File.join(outdir, "data")
runner = File.join(outdir, "case.rb")

record_cmd = ["uftrace", "record"]
# --srcline is a record-time option: it decides whether the source line of
# each call is stored at all.  Passing it only to replay, as this once did,
# leaves the SOURCE column empty no matter what replay asks for.
record_cmd << "--srcline"
record_cmd << "-P." if opts[:patch_all]
record_cmd << "-l" if opts[:nest_libcall]
record_cmd << "-a" if opts[:auto_args]
record_cmd += ["-D", opts[:depth].to_s] if opts[:depth]
record_cmd += ["-t", opts[:time_filter]] if opts[:time_filter]
record_cmd += ["-F", root, "-d", datadir, "--", bin, runner]

puts "Method      : #{info.key}"
puts "C root      : #{root}#{root == info.func ? '' : " (was #{info.func})"}"
puts "Defined     : #{info.definition}" if info.definition
puts "Registered  : #{info.location} via #{info.via}" if info.location
puts "Aliased from: #{info.alias_of}" if info.alias_of
# The recording is of a C function, not of a method: every method reaching it
# lands in the same trace, and nothing in the output says which one was
# called.  Worth knowing before reading a graph of mrb_do_nothing.
puts "Shared with : #{info.shared_with.join(', ')}" if info.shared_with
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
  ("definition=#{info.definition}" if info.definition),
  ("registration=#{info.location}" if info.location),
  ("via=#{info.via}" if info.via),
  ("alias_of=#{info.alias_of}" if info.alias_of),
  ("shared_with=#{info.shared_with.join(' ')}" if info.shared_with),
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
  warn "  - another registration won: a gem's mrblib can redefine a method the C"
  warn "    sources register, which only --runtime or --merge can see"
  warn "  - #{root} was inlined away; rebuild with enable_debug (-O0)"
end

puts
puts "Wrote:"
%w[replay.txt graph.txt report.txt meta.txt case.rb].each { |n| puts "  #{File.join(outdir, n)}" }
