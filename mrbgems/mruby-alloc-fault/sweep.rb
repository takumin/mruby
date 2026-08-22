#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Sweep the allocations a scenario makes, refusing one of them at a time.
#
# The driver (mruby-alloc-fault) reports how many allocations a scenario
# asks for; this walks that range and starts the driver once per index, so
# that every allocation in the scenario is, in one run or another, the one
# that fails.  A run counts as a finding when the driver reports an outcome
# it does not accept, when a sanitizer reports on it, when it dies of a
# signal, or when it stops answering.
#
# Runs are separate processes on purpose: a refusal leaves state behind that
# the next run must not inherit, and a crash has to be survivable.

require 'etc'
require 'optparse'

HERE = File.expand_path(File.dirname(__FILE__))

options = {
  bin: nil,
  scenario_dir: File.join(HERE, 'scenarios'),
  known: File.join(HERE, 'known_failures.txt'),
  only: nil,
  modes: %w[sticky once],
  jobs: [Etc.nprocessors - 1, 4].max,
  timeout: 30,
  max_index: 0,
  shard: nil,
  log_dir: nil,
  ignore_known: false,
  emit_known: false,
  list: false,
  verbose: false,
}

OptionParser.new do |o|
  o.banner = "usage: ruby #{File.basename(__FILE__)} --bin PATH [options]"
  o.on('--bin PATH', 'the mruby-alloc-fault executable to drive') { |v| options[:bin] = v }
  o.on('--scenario-dir DIR', 'where the Ruby scenarios live') { |v| options[:scenario_dir] = v }
  o.on('--only NAMES', 'sweep these scenarios only (comma separated)') { |v| options[:only] = v.split(',') }
  o.on('--mode MODE', %w[sticky once both], 'sticky, once or both (default both)') do |v|
    options[:modes] = v == 'both' ? %w[sticky once] : [v]
  end
  o.on('--jobs N', Integer, 'runs to keep in flight (default: processors)') { |v| options[:jobs] = [v, 1].max }
  o.on('--timeout SEC', Integer, 'give up on a run after this long (default 30)') { |v| options[:timeout] = v }
  o.on('--max-index N', Integer, 'stop each scenario at this allocation index') { |v| options[:max_index] = v }
  o.on('--shard I/N', 'sweep the I-th of N even slices of the work') { |v| options[:shard] = v }
  o.on('--log-dir DIR', 'write the output of every finding here') { |v| options[:log_dir] = v }
  o.on('--known PATH', 'the known-failures file') { |v| options[:known] = v }
  o.on('--ignore-known', 'report the known failures as findings too') { options[:ignore_known] = true }
  o.on('--emit-known', 'print what was found in the known-failures format') { options[:emit_known] = true }
  o.on('--list', 'list the scenarios with their allocation counts') { options[:list] = true }
  o.on('-v', '--verbose', 'name every finding as it happens') { options[:verbose] = true }
end.parse!

abort "sweep: --bin is required" unless options[:bin]
abort "sweep: no such executable: #{options[:bin]}" unless File.executable?(options[:bin])

ENV['ASAN_OPTIONS'] ||= 'detect_leaks=1'
ENV['UBSAN_OPTIONS'] ||= 'print_stacktrace=1'

# ---------------------------------------------------------------------------

# What one run of the driver came to.  The output is read whole, because a
# sanitizer that does not abort -- an undefined-behaviour report -- leaves
# its say there and exits 0 all the same.
Result = Struct.new(:kind, :signature, :output, :command)

SANITIZER_REPORT = /ERROR: (?:Address|Leak|Memory|Thread)Sanitizer|runtime error:/

# The allocator plumbing every refusal passes through.  A signature names the
# code that was holding the memory, so the frames that merely handed it over
# are stepped past.
PLUMBING = %w[
  realloc malloc calloc free
  __interceptor_realloc __interceptor_malloc __interceptor_calloc
  mrb_basic_alloc_func
  mrb_realloc mrb_realloc_simple mrb_malloc mrb_malloc_simple mrb_calloc
].freeze

def report_slug(output)
  case output
  when /ERROR: LeakSanitizer: detected memory leaks/ then 'leak'
  when /ERROR: AddressSanitizer: ([A-Za-z-]+)/ then Regexp.last_match(1).downcase
  when /runtime error:/ then 'ub'
  else 'sanitizer'
  end
end

# The first frame of the first backtrace that is neither the allocator
# plumbing nor the sanitizer's own machinery.
def frame_of(output)
  output.lines.each do |line|
    next unless (m = line.match(/^\s+#\d+ 0x\h+ in (\S+)\b(.*)$/))
    name, rest = m[1], m[2]
    next if PLUMBING.include?(name)
    next if rest.include?('libsanitizer') || rest.include?('sanitizer_common')
    return name
  end
  nil
end

# What a finding is, in a form that survives the allocation index moving.
# An unrelated edit shifts every index in a scenario, so an index is no way
# to say "this one is known"; the code that was holding the memory is.
def signature_of(kind, output)
  case kind
  when :timeout then 'timeout'
  when :usage   then 'usage'
  when :crash   then "crash:#{frame_of(output) || '?'}"
  when :sanitizer then "#{report_slug(output)}:#{frame_of(output) || '?'}"
  else
    outcome = output[/^outcome: (\S+)/, 1] || 'unknown'
    exception = output[/^exception: (\S+)/, 1]
    exception ? "#{outcome}:#{exception}" : outcome
  end
end

def run_once(bin, args, timeout)
  reader, writer = IO.pipe
  pid = Process.spawn(bin, *args, out: writer, err: writer)
  writer.close

  output = +''
  pump = Thread.new { output << reader.read rescue nil }

  status = nil
  timed_out = false
  deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + timeout
  loop do
    _, status = Process.waitpid2(pid, Process::WNOHANG)
    break if status
    if Process.clock_gettime(Process::CLOCK_MONOTONIC) > deadline
      timed_out = true
      Process.kill('KILL', pid) rescue nil
      begin
        _, status = Process.waitpid2(pid)
      rescue SystemCallError
      end
      break
    end
    sleep 0.005
  end
  pump.join(2)
  reader.close

  kind =
    if timed_out then :timeout
    elsif status.nil? || status.signaled? then :crash
    elsif SANITIZER_REPORT.match?(output) then :sanitizer
    elsif status.exitstatus == 2 then :usage
    elsif status.exitstatus != 0 then :finding
    else :ok
    end

  signature = kind == :ok ? nil : signature_of(kind, output)
  Result.new(kind, signature, output, ([bin] + args).join(' '))
end

# One run of the sweep: a scenario, the mode it is refused in, and which of
# its allocations to refuse.  A Struct rather than an array the worker
# unpacks, because a name a worker assigns to has to belong to the worker: a
# local the main script already introduced would be shared by every thread,
# and the runs would report each other's indices.
Job = Struct.new(:scenario, :mode, :flag, :index)

# A scenario is either one of the driver's own C scenarios, which cover what
# happens around mrb_open(), or a Ruby file.
Scenario = Struct.new(:name, :args) do
  def with(extra)
    args + extra
  end
end

def scenarios_for(options)
  list = [
    Scenario.new('open', ['-c', 'open']),
    Scenario.new('open-core', ['-c', 'open-core']),
  ]
  Dir[File.join(options[:scenario_dir], '*.rb')].sort.each do |path|
    list << Scenario.new(File.basename(path, '.rb'), ['-f', path])
  end
  list.select! { |s| options[:only].include?(s.name) } if options[:only]
  abort "sweep: no scenario matched" if list.empty?
  list
end

# scenario -> the signatures already understood, `any` standing for all of
# them.
def read_known(path)
  known = Hash.new { |h, k| h[k] = [] }
  return known unless File.exist?(path)
  File.foreach(path) do |line|
    line = line.sub(/#.*/, '').strip
    next if line.empty?
    name, signature, = line.split(/\s+/, 3)
    known[name] << (signature || 'any')
  end
  known
end

def known?(known, name, signature)
  signatures = known[name]
  signatures.include?('any') || signatures.include?(signature)
end

def count_allocations(bin, scenario, timeout)
  result = run_once(bin, scenario.with(['--count']), timeout)
  unless result.kind == :ok
    warn "sweep: #{scenario.name}: the scenario does not run cleanly on its own"
    warn result.output
    return nil
  end
  result.output[/^allocations: (\d+)$/, 1]&.to_i
end

# ---------------------------------------------------------------------------

bin = options[:bin]
scenarios = scenarios_for(options)

counts = {}
scenarios.each do |s|
  n = count_allocations(bin, s, options[:timeout])
  exit 2 if n.nil?
  counts[s.name] = n
end

if options[:list]
  counts.each { |name, n| puts format('%-12s %6d', name, n) }
  exit 0
end

work = []
scenarios.each do |s|
  n = counts[s.name]
  n = options[:max_index] if options[:max_index] > 0 && options[:max_index] < n
  options[:modes].each do |mode|
    flag = mode == 'sticky' ? '--fail-at' : '--fail-once'
    (1..n).each { |i| work << Job.new(s, mode, flag, i) }
  end
end

if options[:shard]
  shard_index, shard_total = options[:shard].split('/', 2).map(&:to_i)
  unless shard_total.to_i > 0 && shard_index.between?(1, shard_total)
    abort "sweep: --shard wants I/N with 1 <= I <= N"
  end
  work = work.each_with_index.select { |_, i| i % shard_total == shard_index - 1 }.map(&:first)
end

puts "sweep: #{scenarios.size} scenarios, #{work.size} runs, #{options[:jobs]} at a time"
counts.each { |name, n| puts format('  %-12s %6d allocations', name, n) }
puts

if options[:log_dir]
  require 'fileutils'
  FileUtils.mkdir_p(options[:log_dir])
end

queue = Queue.new
work.each { |item| queue << item }
findings = Queue.new
done = 0
done_lock = Mutex.new
total_runs = work.size

workers = Array.new(options[:jobs]) do
  Thread.new do
    while (job = (queue.pop(true) rescue nil))
      outcome = run_once(bin, job.scenario.with([job.flag, job.index.to_s]), options[:timeout])
      if outcome.kind != :ok
        findings << [job.scenario.name, job.mode, job.index, outcome]
        warn "#{job.scenario.name} #{job.mode} ##{job.index}: #{outcome.signature}" if options[:verbose]
      end
      done_lock.synchronize do
        done += 1
        if done % 250 == 0 || done == total_runs
          print "\r  #{done}/#{total_runs} runs"
          $stdout.flush
        end
      end
    end
  end
end
workers.each(&:join)
puts
puts

known = options[:ignore_known] ? Hash.new { |h, k| h[k] = [] } : read_known(options[:known])

# scenario -> signature -> the runs that ended there
grouped = Hash.new { |h, k| h[k] = Hash.new { |g, s| g[s] = [] } }
until findings.empty?
  name, mode, index, result = findings.pop
  grouped[name][result.signature] << [mode, index, result]
end

unexpected = 0
emitted = []
grouped.keys.sort.each do |name|
  grouped[name].keys.sort.each do |signature|
    group = grouped[name][signature].sort_by { |mode, index, _| [mode, index] }
    quarantined = known?(known, name, signature)
    unexpected += group.size unless quarantined
    emitted << format('%-12s %-36s # %d run(s)', name, signature, group.size)

    where = group.map { |mode, index, _| "#{mode}##{index}" }
    where = where.first(8).join(', ') + (group.size > 8 ? ', ...' : '')
    puts format('%-12s %-36s %5d run(s)%s', name, signature, group.size,
                quarantined ? '  [known]' : '')
    puts "  #{where}"
    puts "  #{group.first[2].command}"

    next unless options[:log_dir]
    group.first(20).each do |mode, index, result|
      path = File.join(options[:log_dir], "#{name}-#{mode}-#{index}.log")
      File.write(path, "$ #{result.command}\n\n#{result.output}")
    end
  end
end

if grouped.empty?
  puts "sweep: every one of the #{work.size} runs was accepted"
else
  puts
  if options[:emit_known]
    puts "# for known_failures.txt:"
    emitted.sort.each { |line| puts line }
    puts
  end
  puts "sweep: #{unexpected} unexpected run(s) of #{work.size}"
end

exit(unexpected.zero? ? 0 : 1)
