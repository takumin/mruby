require 'open3'
require 'tempfile'
require 'tmpdir'

assert('Compiling multiple files without new line in last line. #2361') do
  a, b, out = Tempfile.new('a.rb'), Tempfile.new('b.rb'), Tempfile.new('out.mrb')
  a.write('module A; end')
  a.flush
  b.write('module B; end')
  b.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-c', '-o', out.path, a.path, b.path]))
  assert_equal "#{cmd_bin('mrbc')}:#{a.path}:Syntax OK", result.chomp
  assert_equal 0, status.exitstatus
end

assert('the first of several input files may hold no statements') do
  # codegen() walks from one input file to the next when the node it is handed
  # starts past the end of the file it is on, and closes the debug range of the
  # file it leaves against the scope's irep. The scope generate_code() starts in
  # has none: it only carries the program node down to the top-level scope. A
  # first file that parses to no statement puts that program node in the second
  # file, so the walk ran in the scope without an irep and read through it.
  Dir.mktmpdir do |dir|
    second = File.join(dir, 'second.rb')
    File.write(second, "p 1\nraise 'boom'\n")

    # Every file that holds no statement at all arrives the same way.
    ['', "\n", "# comment\n", "=begin\n=end\n"].each do |source|
      first = File.join(dir, 'first.rb')
      out = File.join(dir, 'out.mrb')
      File.write(first, source)

      result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-g', '-o', out, first, second]))
      assert_equal 0, status.exitstatus, source.inspect
      assert_equal '', result.chomp

      # The walk still has to land on the second file: that is what names the
      # line the backtrace blames.
      ran, = Open3.capture2e(*(cmd_list('mruby') + ['-b', out]))
      assert_include ran, "1\n", source.inspect
      assert_include ran, "#{second}:2: boom (RuntimeError)", source.inspect
    end
  end
end

assert('parsing function with void argument') do
  a, out = Tempfile.new('a.rb'), Tempfile.new('out.mrb')
  a.write('f ()')
  a.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-c', '-o', out.path, a.path]))
  assert_equal "#{cmd_bin('mrbc')}:#{a.path}:Syntax OK", result.chomp
  assert_equal 0, status.exitstatus
end

assert('too many local variables are rejected') do
  compile = lambda do |count|
    source = Tempfile.new(['many-locals', '.rb'])
    count.times { |i| source.puts("local_#{i} = nil") }
    source.flush
    Open3.capture2e(*(cmd_list('mrbc') + ['-c', source.path]))
  end

  result, status = compile.call(254)
  assert_true status.success?, result

  result, status = compile.call(255)
  assert_equal 1, status.exitstatus
  assert_include result, 'too many local variables'

  # 65,536 is where the count wrapped through the 16-bit stack pointer and
  # the table was written past its end (#7576), and it is the one number
  # that shows the check runs before the narrowing rather than after. It is
  # not compiled here: one such source costs 56 seconds in this build, 94
  # under a sanitizer and 101 at -O0, against the half minute the whole of
  # this file takes, and every runner builds this configuration. Run it by
  # hand where the narrowing is touched:
  #
  #   ruby -e '65_536.times {|i| puts "local_#{i} = nil"}' > many-locals.rb
  #   mrbc -c many-locals.rb
end

assert('a scope refused for its local variables is named with its position') do
  source = Tempfile.new(['many-locals', '.rb'])
  source.puts("x = 1")
  source.puts("def big")
  255.times { |i| source.puts("  local_#{i} = nil") }
  source.puts("end")
  source.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-c', source.path]))
  assert_equal 1, status.exitstatus
  assert_include result, "#{source.path}:2: too many local variables"
end

assert('a generator error carries its position into the diagnostic list') do
  # The list is what `mrbc` prints and what `eval` builds its SyntaxError from.
  # Every entry in it used to read 0:0, since the generator recorded no
  # position; the parser's entries have always carried one.
  source = Tempfile.new(['end-block', '.rb'])
  source.puts("x = 1")
  source.puts("END { }")
  source.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-c', source.path]))
  assert_equal 1, status.exitstatus
  assert_include result, "#{source.path}:2:1: generator error, END not supported"
end

assert('embedded document with invalid terminator') do
  a, out = Tempfile.new('a.rb'), Tempfile.new('out.mrb')
  a.write("=begin\n=endx\n")
  a.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-c', '-o', out.path, a.path]))
  assert_equal "#{a.path}:2:1: syntax error, embedded document meets end of file", result.chomp
  assert_equal 1, status.exitstatus
end

assert('a float literal under MRB_NO_FLOAT is read as 0 with a warning') do
  # Only a build without Float takes this path.  Whether this is one is asked
  # of its mruby, when there is one; mrbc itself cannot be asked.  The run
  # below is the one here that is not assert_run's: what it fails with is the
  # answer, not a broken fixture.
  skip 'no mruby to probe the build with' unless File.exist?(cmd_bin('mruby'))
  system(*(cmd_list('mruby') + ['-e', 'Float']), out: File::NULL, err: File::NULL)
  skip 'this build has Float' if $?.success?

  a, out = Tempfile.new('a.rb'), Tempfile.new('out.mrb')
  a.write("x = 1\np 1.5\n")
  a.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path]))
  assert_equal 0, status.exitstatus
  assert_include result, "#{a.path}:2:3: generator warning, floating-point numbers are not supported"
  compiled, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
  assert_true status.success?, 'mruby did not run the compiled program'
  assert_equal "0\n", compiled
end

assert('mrbc -v disassembles like mruby -v') do
  # mruby-compiler carries its own copy of the disassembler, because it has to
  # build for mruby/c as well and cannot share src/codedump.c. The copy has
  # drifted before (see #6970), so pin the two outputs to each other.
  src = <<~'EOS'
    CONST = 1
    $gv = "s"
    class C
      @@cv = 2
      def initialize(a, b = 1, *r, k: 2, **kw, &blk)
        @iv = a
        @x = a + b - 1 * 2 / 3
        @y = a[0]
        @y[1] = 2
        @s = "lit#{a}" + ''
        @h = {x: 1, **kw}
        @ary = [*r, 1, 2]
        @cmp = (a < b) && (a > b) || (a <= b) && (a >= b) && (a == b)
      end
      def self.m = C::CONST
      protected def prot = @iv
    end
    module M; end
    [1, 2].each { |v| p v }
    begin
      raise "x"
    rescue => e
      p e
      retry if false
    ensure
      $gv = "done"
    end
    ->(z) { z }.call(1)
    case 1 when 1 then 2 else 3 end
    while false; break; end
    def kw(a:, b: 1); [a, b]; end
  EOS

  a, out = Tempfile.new('a.rb'), Tempfile.new('out.mrb')
  a.write(src)
  a.flush

  # Keep only the disassembly. The irep address has to go by position, not by
  # shape: MSVC prints "%p" as 000001C3D1A77D50 and glibc as 0x5b5cc80b2660.
  clean = lambda do |s|
    s.sub(/\A.*?^(?=irep )/m, '')
     .gsub(/^irep \S+ /, 'irep ADDR ')
     .lines.reject { |l| l.start_with?('Syntax OK') }.join
  end

  # capture2 takes stdout only, which is where both write the disassembly.
  mrbc_out, mrbc_status = Open3.capture2(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path]))
  mruby_out, mruby_status = Open3.capture2(*(cmd_list('mruby') + ['-v', '-c', a.path]))
  assert_true mrbc_status.success?, 'mrbc -v did not run'
  assert_true mruby_status.success?, 'mruby -v -c did not run'
  from_mrbc = clean.call(mrbc_out)
  from_mruby = clean.call(mruby_out)

  assert_false from_mrbc.empty?, 'mrbc -v produced no disassembly'
  assert_equal from_mruby, from_mrbc
end

assert('non-seekable input file is rejected by size, not blamed on the read') do
  # The file arm of the source reader sizes its buffer from ftell(). On a pipe
  # ftell() fails, and before it was checked the -1 propagated into the
  # allocation and the fread() count, surfacing as the misleading "cannot read
  # program file"; the file opens and reads fine, only its size is unknown.
  # Needs a genuinely unseekable path: `< file` would still be seekable.
  skip 'no /dev/stdin' if target_win?
  skip 'no /dev/stdin' unless File.exist?('/dev/stdin')

  a = Tempfile.new('a.rb')
  a.write("puts 1\n")
  a.flush

  # The one command here that a shell has to read: what is under test is what
  # arrives through a pipe, and the pipeline is the shell's to build.  Both
  # halves are quoted for it, the command included: `cmd_list()` can hold a
  # build directory or an emulator argument with a space in it, and joining
  # that with spaces is what hands the shell the wrong words.
  mrbc = Shellwords.join(cmd_list('mrbc'))
  result = `cat #{shellquote(a.path)} | #{mrbc} -c /dev/stdin 2>&1`
  assert_equal 1, $?.exitstatus
  assert_include result, 'compile.c: cannot get size of program file. (/dev/stdin)'
  assert_not_include result, 'cannot read program file'
end

assert('a directory as an input file is refused') do
  # Only POSIX systems open a directory for reading; Windows refuses it at
  # fopen() and never reaches the reader this guards.
  skip 'fopen() refuses a directory' if target_win?
  # ftell() answers LONG_MAX for a directory stream on ext4 and 0 on tmpfs,
  # and the size check accepts both: the first overflows the length
  # arithmetic that sizes the buffer, the second compiles as an empty
  # program.  The fread() failure below reports the LONG_MAX case in wording
  # of its own, so pin which message arrives, not merely that one did.
  Dir.mktmpdir do |dir|
    result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-c', dir]))
    assert_true(
      result.include?('compile.c: cannot read from program file.') ||
      result.include?('compile.c: cannot get size of program file.') # AArch32/armhf
    )
    assert_not_include result, 'compile.c: cannot read program file.'
    assert_equal 1, status.exitstatus
  end
end

assert('# frozen_string_literal is read the same from -e as from a file') do
  # The two reach the parser by different routes and neither is the route the
  # asserts below take, so what this build does with the comment is checked
  # here rather than assumed by the skips that read it.
  skip 'no mruby to run the program' unless File.exist?(cmd_bin('mruby'))

  src = "# frozen_string_literal: true\np 'a'.frozen?"
  file = Tempfile.new(['one', '.rb'])
  file.write(src + "\n")
  file.flush
  from_e, from_e_status = Open3.capture2(*(cmd_list('mruby') + ['-e', src]))
  from_file, from_file_status = Open3.capture2(*(cmd_list('mruby') + [file.path]))
  assert_true from_e_status.success?, 'mruby did not run -e'
  assert_true from_file_status.success?, 'mruby did not run the file'
  assert_equal from_file, from_e
end

assert('# frozen_string_literal is read for each input file') do
  # mrbc parses the files it is given as one source, and a magic comment is
  # only read before the first token of what is parsed, so the comment of the
  # first file would otherwise stand for all of them.
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  frozen = "# frozen_string_literal: true\np 'a'.frozen?\n"
  plain = "p 'b'.frozen?\n"
  # asking for `false` is an answer of its own: it wants the literals a file
  # that carries no comment wants, but its comment was taken all the same.
  mutable = "# frozen_string_literal: false\np 'c'.frozen?\n"
  [[frozen, frozen, "true\ntrue\n"],
   [frozen, plain,  "true\nfalse\n"],
   [plain,  frozen, "false\ntrue\n"],
   [plain,  plain,  "false\nfalse\n"],
   [frozen, mutable, "true\nfalse\n"],
   [mutable, frozen, "false\ntrue\n"],
   [plain,  mutable, "false\nfalse\n"]].each do |first, second, expected|
    a, b = Tempfile.new(['a', '.rb']), Tempfile.new(['b', '.rb'])
    out = Tempfile.new(['out', '.mrb'])
    a.write(first)
    a.flush
    b.write(second)
    b.flush
    # -v so that a warning would be printed: the comment of the second file
    # is taken, which is what makes "is ignored" the wrong thing to say.
    result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path, b.path]))
    assert_equal 0, status.exitstatus
    assert_not_include result, 'is ignored after any tokens'
    ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
    assert_true status.success?, 'mruby did not run the compiled program'
    assert_equal expected, ran
  end
end

assert('# frozen_string_literal is read in every spelling prism takes') do
  # Prism matches the key with each `-` read as `_` and steps over a byte
  # order mark at the head of the source, while the magic comments it records
  # hold the key as written and the source from its first byte.
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  plain = "p 'a'.frozen?\n"
  dashed = "# frozen-string-literal: true\np 'b'.frozen?\n"
  emacs = "# -*- Frozen-String-Literal: true -*-\np 'c'.frozen?\n"
  bom = "\xEF\xBB\xBF# frozen_string_literal: true\np 'd'.frozen?\n"
  [[[dashed], "true\n"],
   [[emacs], "true\n"],
   [[bom], "true\n"],
   [[plain, dashed], "false\ntrue\n"],
   [[plain, emacs], "false\ntrue\n"],
   [[bom, plain], "true\nfalse\n"]].each do |sources, expected|
    files = sources.map do |src|
      file = Tempfile.new(['s', '.rb'])
      file.binmode
      file.write(src)
      file.flush
      file
    end
    out = Tempfile.new(['out', '.mrb'])
    result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path] + files.map(&:path)))
    assert_equal 0, status.exitstatus, result
    assert_not_include result, 'is ignored after any tokens'
    ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
    assert_true status.success?, 'mruby did not run the compiled program'
    assert_equal expected, ran
  end
end

assert('# frozen_string_literal after code is still reported') do
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  a, b = Tempfile.new(['a', '.rb']), Tempfile.new(['b', '.rb'])
  out = Tempfile.new(['out', '.mrb'])
  a.write("# frozen_string_literal: true\np 'a'.frozen?\n")
  a.flush
  b.write("code = 1\n# frozen_string_literal: true\np 'b'.frozen?\n")
  b.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path, b.path]))
  assert_equal 0, status.exitstatus
  assert_include result, "#{b.path}:2:1: syntax warning, 'frozen_string_literal' is ignored after any tokens"
  ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
  assert_true status.success?, 'mruby did not run the compiled program'
  assert_equal "true\nfalse\n", ran
end

assert('# frozen_string_literal carried twice by one file is reported once') do
  # The comment a file was answered from is the one the warning is dropped
  # for, so a file that carries a second one too late to be taken is told
  # about that one and no other.
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  a, b = Tempfile.new(['a', '.rb']), Tempfile.new(['b', '.rb'])
  out = Tempfile.new(['out', '.mrb'])
  a.write("p 'a'.frozen?\n")
  a.flush
  b.write("# frozen_string_literal: true\np 'b'.frozen?\n# frozen_string_literal: true\n")
  b.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path, b.path]))
  assert_equal 0, status.exitstatus
  ignored = result.lines.grep(/is ignored after any tokens/)
  assert_equal 1, ignored.size
  assert_include ignored[0], "#{b.path}:3:1:"
  ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
  assert_true status.success?, 'mruby did not run the compiled program'
  assert_equal "false\ntrue\n", ran
end

assert('a line a file boundary carries into a token is not a magic comment') do
  # A file boundary is not a token boundary. What decides whether a line is a
  # comment is the parse that compiles the source, not a parse of the file on
  # its own: the body of a heredoc the file before opened is not a comment,
  # and neither is anything after `__END__`.
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  [["payload = <<'TXT'",
    "# frozen_string_literal: true\nTXT\np payload\np 'x'.frozen?\n",
    "\"# frozen_string_literal: true\\n\"\nfalse\n"],
   ["p 'z'.frozen?\n__END__",
    "# frozen_string_literal: true\np 'x'.frozen?\n",
    "false\n"]].each do |first, second, expected|
    a, b = Tempfile.new(['a', '.rb']), Tempfile.new(['b', '.rb'])
    out = Tempfile.new(['out', '.mrb'])
    a.write(first)
    a.flush
    b.write(second)
    b.flush
    result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path, b.path]))
    assert_equal 0, status.exitstatus
    assert_not_include result, 'is ignored after any tokens'
    ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
    assert_true status.success?, 'mruby did not run the compiled program'
    assert_equal expected, ran
  end
end

assert('# frozen_string_literal in a file that is only part of what parses') do
  # A file mrbc is given does not have to parse on its own, since the
  # concatenation is what has to, and what it asked for is settled before its
  # first token: further back than anywhere a parse can break.
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  a, b = Tempfile.new(['a', '.rb']), Tempfile.new(['b', '.rb'])
  out = Tempfile.new(['out', '.mrb'])
  a.write("module M\n")
  a.flush
  b.write("# frozen_string_literal: true\np 'x'.frozen?\nend\n")
  b.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path, b.path]))
  assert_equal 0, status.exitstatus
  assert_not_include result, 'is ignored after any tokens'
  ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
  assert_true status.success?, 'mruby did not run the compiled program'
  assert_equal "true\n", ran
end

assert('literals joined into a frozen one are refused past what a pool entry holds') do
  # The same bytes written as one literal are taken up to what a pool entry
  # records its length in and refused past it, so a join is taken and refused
  # at the same place rather than left to the run-time concatenation, which
  # would answer a file that froze its literals with a string that is not one.
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  out = Tempfile.new(['out', '.mrb'])
  compile = lambda do |body|
    file = Tempfile.new(['join', '.rb'])
    file.write("# frozen_string_literal: true\n" + body)
    file.flush
    Open3.capture2e(*(cmd_list('mrbc') + ['-o', out.path, file.path]))
  end

  result, status = compile.call("s = \"#{'a' * 30000}\" \"#{'b' * 35535}\"\np s.frozen?\np s.size\n")
  assert_equal 0, status.exitstatus, result
  ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
  assert_true status.success?, 'mruby did not run the compiled program'
  assert_equal "true\n65535\n", ran

  result, status = compile.call("s = \"#{'a' * 30000}\" \"#{'b' * 35536}\"\n")
  assert_not_equal 0, status.exitstatus
  assert_include result, 'frozen string literal too long'

  result, status = compile.call("s = \"#{'a' * 30000}\" \"#{'b' * 30000}\" \"#{'c' * 5536}\"\n")
  assert_not_equal 0, status.exitstatus
  assert_include result, 'frozen string literal too long'

  result, status = compile.call("s = \"#{'a' * 65536}\"\n")
  assert_not_equal 0, status.exitstatus
  assert_include result, 'string literal too long'
  assert_not_include result, 'frozen string literal too long'
end

assert('literals too long to join are left where nothing joins them into one') do
  # A file that asked for nothing keeps the run-time concatenation it always
  # had, and so does an interpolation of its own however long its parts are.
  skip 'no mruby to run the compiled program' unless File.exist?(cmd_bin('mruby'))

  out = Tempfile.new(['out', '.mrb'])
  [["s = \"#{'a' * 30000}\" \"#{'b' * 35536}\"\np s.frozen?\np s.size\n", "false\n65536\n"],
   ["# frozen_string_literal: true\nx = 1\ns = \"#{'a' * 40000}\#{x}#{'b' * 30000}\"\np s.frozen?\np s.size\n",
    "false\n70001\n"]].each do |body, expected|
    file = Tempfile.new(['join', '.rb'])
    file.write(body)
    file.flush
    result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-o', out.path, file.path]))
    assert_equal 0, status.exitstatus, result
    ran, status = Open3.capture2(*(cmd_list('mruby') + ['-b', out.path]))
    assert_true status.success?, 'mruby did not run the compiled program'
    assert_equal expected, ran
  end
end

assert('a super outside a method forwards no block') do
  # The walk for the block a bare `super` forwards stops at the enclosing
  # method scope. Outside one there is none, and the walk used to hand
  # OP_GETUPVAR the count it had reached, a level past the outermost scope
  # that names nothing (#7290). The block to forward is nil there.
  src = <<~'EOS'
    super
    [1].each { super }
    -> { super }
    class C
      super
    end
  EOS
  a, out = Tempfile.new('a.rb'), Tempfile.new('out.mrb')
  a.write(src)
  a.flush
  result, status = Open3.capture2e(*(cmd_list('mrbc') + ['-v', '-o', out.path, a.path]))
  assert_equal 0, status.exitstatus
  assert_include result, 'SUPER'
  assert_not_include result, 'GETUPVAR'
end
