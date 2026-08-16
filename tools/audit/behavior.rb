# Audit what String and Regexp answer, against CRuby and across builds.
#
#   ruby tools/audit/behavior.rb                              # every case file
#   ruby tools/audit/behavior.rb tools/audit/cases/regexp.rb  # some of them
#   ruby tools/audit/behavior.rb --format md                  # a table for a PR
#   ruby tools/audit/behavior.rb --list                       # what it found
#
# A case is an expression, and the answer to compare it against is whatever
# the host CRuby answers, so the usual case is one line with nothing else on
# it. A case that means to pin an answer says so with `want:`, and one whose
# answer moves when a string is indexed by byte says that with `bytes:`; a
# byte-indexed build that diverges without such a declaration is a failure
# rather than something to read past.
#
# Every mruby under build/*/bin is audited, and each is asked what it is
# rather than being read off its build name: whether it indexes by character,
# whether it has Regexp, whether it knows the case of a character above ASCII.
# A case can require any of those with `needs:` and is skipped where they are
# not there.
#
# See tools/audit/README.md for the case-file vocabulary.

require 'open3'
require 'optparse'
require 'rbconfig'
require 'tempfile'

module Audit
  ROOT = File.expand_path('../..', __dir__)
  CASE_DIR = File.join(__dir__, 'cases')
  PROBE = File.read(File.join(__dir__, 'probe.rb'), encoding: Encoding::UTF_8)

  # What a case did not say. Nil cannot stand for it: nil is an answer.
  UNSET = :__audit_unset__

  Raises = Struct.new(:klass, :message)
  Expect = Struct.new(:kind, :payload, :pin_message)
  Answer = Struct.new(:kind, :payload, :detail)
  Oracle = Struct.new(:expect, :source, :declared)
  Verdict = Struct.new(:status, :answer, :oracle)

  # The traits a build is asked about, as expressions it answers for itself.
  # None of them may be a regexp literal: mruby compiles a literal against the
  # Regexp class and a build without mruby-regexp fails to compile the script
  # rather than answering false.
  TRAITS = [
    [:model, '"あ".length == 1 ? "chars" : "bytes"'],
    [:regexp, 'begin; !!Regexp; rescue Exception; false; end'],
    [:unicode_case, '"Ä".downcase == "ä"'],
    [:regexp_unicode_case,
     'begin; !!(Regexp.new("ā", Regexp::IGNORECASE) =~ "Ā"); rescue Exception; false; end'],
  ].freeze

  TRAIT_LABELS = {
    regexp: 'regexp',
    unicode_case: 'unicode-case',
    regexp_unicode_case: '/i-unicode',
  }.freeze

  class Case < Struct.new(:index, :expr, :want, :bytes, :needs, :note, :group, :file, :line)
    def where
      "#{Audit.relative(file)}:#{line}"
    end

    def title
      line = expr.strip
      line.include?("\n") ? "#{line.lines.first.strip} ..." : line
    end
  end

  # A case file is read with the suite as self, so `group`, `check` and
  # `raises` are the whole vocabulary a case file needs.
  class Suite
    attr_reader :cases

    def initialize
      @cases = []
      @group = nil
      @needs = []
    end

    def load(path)
      instance_eval(File.read(path, encoding: Encoding::UTF_8), path)
      self
    end

    def group(title, needs: nil)
      outer_title = @group
      outer_needs = @needs
      @group = title
      @needs = outer_needs + Array(needs)
      yield
    ensure
      @group = outer_title
      @needs = outer_needs
    end

    def check(expr, want: UNSET, bytes: UNSET, needs: nil, note: nil)
      at = caller_locations(1, 1).first
      @cases << Case.new(@cases.size, expr.to_s, want, bytes,
                         @needs + Array(needs), note, @group, at.path, at.lineno)
    end

    def raises(klass, message = nil)
      Raises.new(klass, message)
    end
  end

  class Engine
    attr_reader :name, :cmd, :traits, :error

    def initialize(name, cmd)
      @name = name
      @cmd = cmd
      @traits = {}
      @error = nil
    end

    def model
      @traits[:model]
    end

    def bytes?
      model == 'bytes'
    end

    def ready?
      @error.nil?
    end

    def probe(timeout)
      script = Audit.script(TRAITS.each_with_index.map { |(_, expr), i| [i, expr] })
      out, err, = Audit.run(@cmd, script, timeout)
      answers = Audit.parse(out)
      TRAITS.each_with_index do |(key, _), i|
        answer = answers[i]
        unless answer && answer.kind == 'v'
          @error = "could not be asked what it is#{err.empty? ? '' : ": #{err.lines.first.to_s.strip}"}"
          return self
        end
        @traits[key] = Audit.literal(answer.payload)
      end
      self
    end

    def meets?(needs)
      needs.all? do |need|
        case need
        when :chars then !bytes?
        when :bytes then bytes?
        else @traits[need]
        end
      end
    end

    def trait_line
      labels = TRAIT_LABELS.filter_map { |key, label| label if @traits[key] }
      labels.empty? ? '-' : labels.join(' ')
    end
  end

  class << self
    # A binary under build/<name>/bin answers to its build name; anything
    # else has to answer to where it is.
    def engine_name(path)
      match = File.expand_path(path).match(%r{/build/([^/]+)/bin/mruby(\.exe)?\z})
      match ? match[1] : relative(path)
    end

    def relative(path)
      rel = path.to_s.sub(/\A#{Regexp.escape(ROOT)}\//, '')
      rel.empty? ? path.to_s : rel
    end

    # ----------------------------------------------------------- running

    # One script for the whole run, so a build is started once. Every case
    # gets its own block, which keeps a local one case assigns out of the
    # next and makes a case read the same alone as it does in the batch.
    def script(pairs)
      body = pairs.map { |index, expr| "__audit_emit(#{index}) do\n#{expr}\nend\n" }
      PROBE + "\n" + body.join
    end

    def run(cmd, source, timeout)
      Tempfile.create(['audit', '.rb']) do |file|
        file.binmode
        file.write(source)
        file.flush
        capture(cmd + [file.path], timeout)
      end
    end

    def capture(argv, timeout)
      Open3.popen3(*argv) do |stdin, stdout, stderr, wait|
        stdin.close
        out = ''.dup
        err = ''.dup
        readers = [Thread.new { out << stdout.read }, Thread.new { err << stderr.read }]
        killed = false
        unless wait.join(timeout)
          Process.kill('KILL', wait.pid)
          killed = true
        end
        readers.each(&:join)
        [out, err, wait.value, killed]
      end
    end

    def parse(out)
      answers = {}
      out.each_line do |line|
        next unless (m = line.match(/\A(\d+)\t([ve])\t(.*)\n?\z/))

        answers[m[1].to_i] = Answer.new(m[2], m[3], nil)
      end
      answers
    end

    # The batch is the fast path; a build that dies takes the rest of the
    # batch with it, so whatever is missing afterwards is asked again one
    # case per process. What is still missing then died on its own, and the
    # process says how.
    def ask(engine, cases, timeout, isolate)
      answers = {}
      unless isolate
        out, = run(engine.cmd, script(cases.map { |c| [c.index, c.expr] }), timeout)
        answers = parse(out)
      end
      cases.each do |kase|
        next if answers[kase.index]

        out, err, status, killed = run(engine.cmd, script([[kase.index, kase.expr]]), timeout)
        found = parse(out)[kase.index]
        answers[kase.index] = found || Answer.new('crash', '', death(status, err, killed))
      end
      answers
    end

    def death(status, err, killed)
      return "timed out after being killed" if killed
      return "exited #{status.exitstatus} without an answer#{first_line(err)}" if status.exited?

      "died on signal #{status.termsig}#{first_line(err)}"
    end

    def first_line(err)
      line = err.lines.find { |l| !l.strip.empty? }
      line ? ": #{line.strip}" : ''
    end

    # ----------------------------------------------------- what to expect

    def expect_from(spec)
      if spec.is_a?(Raises)
        message = spec.message
        Expect.new('e', "#{spec.klass} s:#{__audit_hex(message.to_s)}", !message.nil?)
      else
        Expect.new('v', __audit_dump(spec), false)
      end
    end

    def oracle_for(kase, engine, cruby)
      if engine.bytes? && ![UNSET, :same].include?(kase.bytes)
        return Oracle.new(nil, :skip, true) if kase.bytes == :skip

        return Oracle.new(expect_from(kase.bytes), :bytes, true)
      end

      declared = !engine.bytes? || kase.bytes == :same
      if kase.want == UNSET
        Oracle.new(cruby, :cruby, declared)
      else
        Oracle.new(expect_from(kase.want), :want, declared)
      end
    end

    def agrees?(expect, answer)
      return false unless expect && answer && expect.kind == answer.kind
      return expect.payload == answer.payload unless expect.kind == 'e'

      wanted = expect.payload.split(' ', 2)
      got = answer.payload.split(' ', 2)
      return false unless wanted[0] == got[0]

      expect.pin_message ? wanted[1] == got[1] : true
    end

    # --------------------------------------------------------- rendering

    def plain(dump)
      dump.start_with?('s:') ? [dump[2..]].pack('H*').force_encoding(Encoding::UTF_8) : dump
    end

    # A trait answer read back as the thing it is: "false" is an answer a
    # build gave, not a string that happens to be true.
    def literal(dump)
      case dump
      when 'true' then true
      when 'false' then false
      when 'nil' then nil
      else plain(dump)
      end
    end

    def render(dump)
      dump.gsub(/(?<![A-Za-z0-9_])([sy]):([0-9a-f]*)/) do
        raw = [Regexp.last_match(2)].pack('H*')
        Regexp.last_match(1) == 's' ? quote(raw) : ":#{raw}"
      end
    end

    def show(answer)
      return "(#{answer.detail})" if answer.kind == 'crash'

      rendered = render(answer.payload)
      answer.kind == 'e' ? "raises #{rendered.sub(' "', ': "')}" : rendered
    end

    def show_expect(expect)
      return '(nothing to compare against)' unless expect

      rendered = render(expect.payload)
      return rendered unless expect.kind == 'e'

      expect.pin_message ? "raises #{rendered.sub(' "', ': "')}" : "raises #{rendered.split(' ').first}"
    end

    ESCAPES = { '"' => '\\"', '\\' => '\\\\', "\n" => '\\n', "\t" => '\\t',
                "\r" => '\\r', "\e" => '\\e', "\0" => '\\0' }.freeze

    def quote(raw)
      str = raw.dup.force_encoding(Encoding::UTF_8)
      body = if str.valid_encoding?
               str.each_char.map { |c| escape_char(c) }.join
             else
               raw.each_byte.map { |b| escape_byte(b) }.join
             end
      "\"#{body}\""
    end

    def escape_char(char)
      return ESCAPES[char] if ESCAPES.key?(char)

      byte = char.bytes
      byte.size == 1 && (byte[0] < 0x20 || byte[0] == 0x7f) ? format('\\x%02X', byte[0]) : char
    end

    def escape_byte(byte)
      char = byte.chr
      return ESCAPES[char] if ESCAPES.key?(char)

      byte.between?(0x20, 0x7e) ? char : format('\\x%02X', byte)
    end
  end
end

# The probe answers for the driver too: a `want:` written as a Ruby object has
# to be spelled the way a build spells its answer before the two can be
# compared.
eval(Audit::PROBE, TOPLEVEL_BINDING, File.join(__dir__, 'probe.rb')) # rubocop:disable Security/Eval

options = {
  format: 'text',
  builds: [],
  mruby: [],
  ruby: RbConfig.ruby,
  timeout: 60,
  isolate: false,
  list: false,
}

parser = OptionParser.new do |o|
  o.banner = "usage: ruby #{Audit.relative(__FILE__)} [options] [case files or directories]"
  o.on('--build NAME', 'audit this build alone (repeatable)') { |v| options[:builds] << v }
  o.on('--mruby PATH', 'audit this mruby binary too (repeatable)') { |v| options[:mruby] << v }
  o.on('--ruby PATH', 'the CRuby that answers for the cases') { |v| options[:ruby] = v }
  o.on('--format FORMAT', %w[text md], 'text (default) or md') { |v| options[:format] = v }
  o.on('--timeout SECONDS', Integer, 'how long a run may take (default 60)') { |v| options[:timeout] = v }
  o.on('--isolate', 'one process per case, without the batch') { options[:isolate] = true }
  o.on('--list', 'say what builds were found, then stop') { options[:list] = true }
  o.on('-h', '--help') do
    puts o
    exit 0
  end
end
paths = parser.parse(ARGV)

# ------------------------------------------------------------------- cases

files = paths.flat_map { |path| File.directory?(path) ? Dir[File.join(path, '**', '*.rb')].sort : path }
files = Dir[File.join(Audit::CASE_DIR, '**', '*.rb')].sort if files.empty?
missing = files.reject { |f| File.file?(f) }
abort "no such case file: #{missing.join(', ')}" unless missing.empty?

suite = Audit::Suite.new
files.each { |file| suite.load(file) }
cases = suite.cases
abort 'no cases to audit' if cases.empty? && !options[:list]

# ----------------------------------------------------------------- engines

found = Dir[File.join(Audit::ROOT, 'build', '*', 'bin', 'mruby{,.exe}')].sort
found.select! { |path| options[:builds].include?(Audit.engine_name(path)) } unless options[:builds].empty?
engines = found.map { |path| Audit::Engine.new(Audit.engine_name(path), [path]) }
engines += options[:mruby].map { |path| Audit::Engine.new(Audit.engine_name(path), [path]) }
engines.uniq! { |engine| File.expand_path(engine.cmd.first) }

if engines.empty?
  abort "no mruby to audit under #{Audit.relative(File.join(Audit::ROOT, 'build'))}; " \
        'build one first (MRUBY_CONFIG=ci/gcc-clang rake) or pass --mruby PATH'
end

cruby = Audit::Engine.new('CRuby', [options[:ruby]]).probe(options[:timeout])
abort "#{options[:ruby]} #{cruby.error}" unless cruby.ready?

engines.each { |engine| engine.probe(options[:timeout]) }
engines.each { |engine| warn "skipping #{engine.name}: #{engine.error}" unless engine.ready? }
engines.select!(&:ready?)
abort 'no mruby answered the probe' if engines.empty?

version = IO.popen([options[:ruby], '-v'], &:read).strip

if options[:list]
  puts "oracle  #{version}"
  engines.each { |engine| puts format('%-14s %-6s %s', engine.name, engine.model, engine.trait_line) }
  exit 0
end

# ------------------------------------------------------------------ answers

wanted_by_cruby = cases.select { |kase| cruby.meets?(kase.needs) }
cruby_answers = Audit.ask(cruby, wanted_by_cruby, options[:timeout], options[:isolate])

verdicts = {}
engines.each do |engine|
  runnable = cases.select { |kase| engine.meets?(kase.needs) }
  answers = Audit.ask(engine, runnable, options[:timeout], options[:isolate])

  verdicts[engine.name] = cases.to_h do |kase|
    unless engine.meets?(kase.needs)
      next [kase.index, Audit::Verdict.new(:skip, nil, Audit::Oracle.new(nil, :needs, true))]
    end

    answer = answers[kase.index]
    cruby_answer = cruby_answers[kase.index]
    cruby_expect =
      if cruby_answer && cruby_answer.kind != 'crash'
        Audit::Expect.new(cruby_answer.kind, cruby_answer.payload, false)
      end
    oracle = Audit.oracle_for(kase, engine, cruby_expect)

    status =
      if oracle.source == :skip then :skip
      elsif answer.kind == 'crash' then :crash
      elsif oracle.expect.nil? then :error
      elsif Audit.agrees?(oracle.expect, answer) then :ok
      else :diff
      end
    [kase.index, Audit::Verdict.new(status, answer, oracle)]
  end
end

# ------------------------------------------------------------------- report

def tally(verdicts, status)
  verdicts.values.count { |v| v.status == status }
end

failed = cases.select do |kase|
  engines.any? { |e| %i[diff crash error].include?(verdicts[e.name][kase.index].status) }
end

# A case that pins an answer CRuby does not give is not a failure: it is a
# divergence someone decided to keep, and the report says so every run rather
# than leaving it to a comment nobody reads.
pins = cases.filter_map do |kase|
  next if kase.want == Audit::UNSET

  answer = cruby_answers[kase.index]
  next unless answer && answer.kind != 'crash'

  expect = Audit.expect_from(kase.want)
  next if Audit.agrees?(expect, answer)

  [kase, expect, answer]
end

if options[:format] == 'md'
  puts "Read off `#{version}`."
  puts
  puts "| case | CRuby | #{engines.map(&:name).join(' | ')} |"
  puts "| --- | --- |#{' --- |' * engines.size}"
  cases.each do |kase|
    cruby_answer = cruby_answers[kase.index]
    cells = engines.map do |engine|
      verdict = verdicts[engine.name][kase.index]
      case verdict.status
      when :skip then '—'
      when :ok
        # A byte-indexed build that legitimately answers something else is
        # worth spelling out; agreeing with CRuby is worth a tick.
        Audit.agrees?(Audit::Expect.new(cruby_answer&.kind, cruby_answer&.payload, false),
                      verdict.answer) ? '✓' : "`#{Audit.show(verdict.answer)}`"
      else "`#{Audit.show(verdict.answer)}` ⚠"
      end
    end
    cruby_cell = cruby_answer ? "`#{Audit.show(cruby_answer)}`" : '—'
    puts "| `#{kase.title}` | #{cruby_cell} | #{cells.join(' | ')} |"
  end
  puts
  puts "⚠ marks an answer the audit did not expect; — marks a case the build cannot run."
  exit failed.empty? ? 0 : 1
end

puts 'String / Regexp behavior audit'
puts "  oracle  #{version}"
puts "  cases   #{cases.size} from #{files.size} file#{'s' if files.size != 1}"
puts
puts format('  %-14s %-6s %-30s %5s %5s %5s %5s', 'build', 'model', 'traits', 'ok', 'diff', 'skip', 'crash')
engines.each do |engine|
  seen = verdicts[engine.name]
  puts format('  %-14s %-6s %-30s %5d %5d %5d %5d', engine.name, engine.model, engine.trait_line,
              tally(seen, :ok), tally(seen, :diff) + tally(seen, :error), tally(seen, :skip), tally(seen, :crash))
end

failed.each do |kase|
  puts
  puts "FAIL  #{kase.where}  #{kase.group}"
  puts "      #{kase.title}"
  puts "      note: #{kase.note}" if kase.note

  unhappy = engines.reject { |e| %i[ok skip].include?(verdicts[e.name][kase.index].status) }
  crashed, rest = unhappy.partition { |e| verdicts[e.name][kase.index].status == :crash }
  unanswerable, diverged = rest.partition { |e| verdicts[e.name][kase.index].status == :error }

  crashed.group_by { |e| verdicts[e.name][kase.index].answer.detail }.each do |detail, names|
    puts "      crash #{detail}  [#{names.map(&:name).join(', ')}]"
  end

  unless unanswerable.empty?
    puts "      error nothing to compare against: CRuby did not answer this case  " \
         "[#{unanswerable.map(&:name).join(', ')}]"
  end

  diverged.group_by do |engine|
    verdict = verdicts[engine.name][kase.index]
    [Audit.show_expect(verdict.oracle.expect), Audit.show(verdict.answer),
     verdict.oracle.source, verdict.oracle.declared]
  end.each do |(expected, actual, source, declared), names|
    puts "      want  #{expected}  (#{source == :cruby ? 'CRuby' : source})"
    puts "      got   #{actual}  [#{names.map(&:name).join(', ')}]"
    unless declared
      puts '      hint  a byte-indexed build answers something else and the case does not say so; ' \
           'declare it with bytes:'
    end
  end
end

pins.each do |kase, expect, answer|
  puts
  puts "PIN   #{kase.where}  #{kase.group}"
  puts "      #{kase.title}"
  puts "      note: #{kase.note}" if kase.note
  puts "      CRuby   #{Audit.show(answer)}"
  puts "      pinned  #{Audit.show_expect(expect)}"
end

totals = %i[ok diff skip crash error].to_h { |s| [s, engines.sum { |e| tally(verdicts[e.name], s) }] }
puts
puts "#{pins.size} case#{'s' if pins.size != 1} pinned to an answer CRuby does not give" unless pins.empty?
puts "#{cases.size} cases x #{engines.size} build#{'s' if engines.size != 1}: #{totals[:ok]} ok, " \
     "#{totals[:diff] + totals[:error]} diff, #{totals[:skip]} skip, #{totals[:crash]} crash"
exit failed.empty? ? 0 : 1
