module Process
  # Whether this build can create processes at all.  A platform without
  # process creation leaves Process.spawn undefined rather than defining one
  # that always fails, so a program can ask before it commits to a plan that
  # needs a child.
  if respond_to?(:__spawn)
    # Internal encodings, shared with src/spawn.c.  They are not API: what a
    # caller passes is the CRuby argument shape below, and this is only how
    # it reaches C.
    SPAWN_ARGV  = 0
    SPAWN_SHELL = 1

    # The characters only a shell acts on.  A command line holding one of
    # them is the shell's to take apart; a command line holding none of them
    # is taken apart here instead.
    SPAWN_META = "*?{}[]<>()~&|\\$;'\"`\n#"

    # The POSIX shell's reserved words and special built-ins.  Nothing on the
    # PATH is called any of these, so a command line naming one is the
    # shell's however plain the rest of it looks.
    SPAWN_SH_CMDS = [
      "!", ".", ":", "break", "case", "continue", "do", "done", "elif",
      "else", "esac", "eval", "exec", "exit", "export", "fi", "for", "if",
      "in", "readonly", "return", "set", "shift", "then", "times", "trap",
      "unset", "until", "while"
    ]

    REDIR_PARENT = 0
    REDIR_CHILD  = 1
    REDIR_CLOSE  = 2

    SPAWN_CLOSE_OTHERS    = 1
    SPAWN_UNSETENV_OTHERS = 2

    SPAWN_OPTION_KEYS = [:close_others, :unsetenv_others, :chdir]

    class << self
      #
      # call-seq:
      #   Process.spawn([env, ] command... [, options]) -> pid
      #
      # Runs +command+ in a child process and returns its process ID without
      # waiting for it.  The caller owes that child a wait, through
      # Process.waitpid.
      #
      # A single String is a command line, and it reaches the system shell
      # only when there is something in it for a shell to do; otherwise it
      # is split on blanks and the command is run directly.  Two or more
      # arguments are always the command and its arguments.
      #
      #   Process.spawn("echo hello > out.txt")     # through the shell
      #   Process.spawn("echo hello")               # no shell involved
      #   Process.spawn("echo", "hello")            # no shell involved
      #
      #   pid = Process.spawn("sleep", "1")
      #   Process.waitpid(pid)
      #   $?.success?                               # -> true
      #
      # A command that cannot be run raises the Errno the attempt failed
      # with, rather than leaving a child that exited 127 to be guessed at.
      #
      # A leading Hash is added to the child's environment, and a nil value
      # removes a variable rather than setting it:
      #
      #   Process.spawn({"LANG" => "C", "TZ" => nil}, "date")
      #
      # A trailing Hash is options.  <code>:in</code>, <code>:out</code>,
      # <code>:err</code> and plain descriptor numbers redirect the child's
      # descriptors; the table is applied in order, so a later entry sees
      # what an earlier one did:
      #
      #   Process.spawn("cmd", out: io)                  # child's 1 -> io
      #   Process.spawn("cmd", out: io, err: [:child, :out])   # ... and 2>&1
      #   Process.spawn("cmd", out: "log.txt")           # a file, opened here
      #   Process.spawn("cmd", 3 => :close)
      #
      # As in CRuby, <code>err: :out</code> is *not* <code>2>&1</code>: a
      # bare <code>:out</code> names the parent's descriptor 1, and merging
      # inside the child is written <code>err: [:child, :out]</code>.
      #
      # The remaining options are <code>:chdir</code>, which runs the child
      # in another directory, <code>:unsetenv_others</code>, which starts the
      # child's environment empty, and <code>:close_others</code>, which
      # closes descriptors above 2 that the table does not name.  Descriptors
      # this build opens are close-on-exec already, so the last one is rarely
      # needed.
      #
      def spawn(*args)
        env, kind, argv, opts = _spawn_normalize(args)
        table, opened = _spawn_redirects(opts)
        flags = 0
        flags |= SPAWN_CLOSE_OTHERS if opts[:close_others]
        flags |= SPAWN_UNSETENV_OTHERS if opts[:unsetenv_others]
        begin
          __spawn(kind, argv, env, table, flags, opts[:chdir])
        ensure
          opened.each { |io| io.close }
        end
      end

      private

      def _spawn_normalize(args)
        args = args.dup
        env = args[0].is_a?(Hash) ? _spawn_env(args.shift) : []
        if args.empty?
          raise ArgumentError, "wrong number of arguments (given 0, expected 1+)"
        end
        opts = (args.size >= 2 && args[-1].is_a?(Hash)) ? args.pop : {}

        if args[0].is_a?(Array)
          raise NotImplementedError, "[cmdname, argv0] form is not supported"
        end
        if args.size == 1
          kind, argv = _spawn_command_line(_spawn_str(args[0]))
          [env, kind, argv, opts]
        else
          [env, SPAWN_ARGV, args.map { |a| _spawn_str(a) }, opts]
        end
      end

      # A single String is a command line, and CRuby does not hand every one
      # of them to a shell: it looks for what only a shell can do and, where
      # there is none, splits the line on blanks and runs the command
      # itself.  Which is what makes Process.spawn("no-such-command") an
      # exec that fails and an Errno::ENOENT to report, where a shell would
      # have run, complained and exited 127 with nothing to tell that from a
      # command exiting 127 of its own accord.
      def _spawn_command_line(cmd)
        words = _spawn_split(cmd)
        # Blanks name no command.  CRuby execs the empty string for them and
        # that answers ENOENT, which is what the port answers for it here.
        return [SPAWN_ARGV, [""]] if words.empty?
        return [SPAWN_SHELL, [cmd]] if _spawn_shell?(cmd, words[0])
        [SPAWN_ARGV, words]
      end

      # The command line split as a shell would have split one holding
      # nothing of the shell's.  Spaces and tabs only: every other blank is a
      # character of the word it sits in, and a newline never reaches here
      # because a newline is one of the characters that call for a shell.
      def _spawn_split(cmd)
        words = []
        i = 0
        len = cmd.length
        while i < len
          i += 1 while i < len && _spawn_blank?(cmd[i])
          break if i >= len
          start = i
          i += 1 until i >= len || _spawn_blank?(cmd[i])
          words << cmd[start, i - start]
        end
        words
      end

      def _spawn_blank?(c)
        c == " " || c == "\t"
      end

      # Whether the command line is the shell's: it holds a character only a
      # shell acts on, its first word assigns a variable, or that word is a
      # reserved word or a special built-in.
      def _spawn_shell?(cmd, first)
        i = 0
        len = SPAWN_META.length
        while i < len
          return true if cmd.index(SPAWN_META[i])
          i += 1
        end
        return true if first.index("=")
        SPAWN_SH_CMDS.include?(first)
      end

      # mruby asks for the built-in type rather than converting to it, so a
      # command that is not a String is refused here rather than sent
      # through to_str.
      def _spawn_str(v)
        unless v.is_a?(String)
          raise TypeError, "no implicit conversion of #{v.class} into String"
        end
        v
      end

      # [name, value, name, value, ...]; a nil value means "unset".  Deltas,
      # not a whole environment, so nothing here has to read the parent's,
      # which is what keeps this gem independent of mruby-env.
      def _spawn_env(hash)
        env = []
        hash.each do |k, v|
          env << _spawn_str(k) << (v.nil? ? nil : _spawn_str(v))
        end
        env
      end

      def _spawn_redirects(opts)
        table = []
        opened = []
        begin
          opts.each do |key, value|
            next if SPAWN_OPTION_KEYS.include?(key)
            fds = (key.is_a?(Array) ? key : [key]).map { |k| _spawn_fd(k) }
            next if fds.empty?
            # What the value names is worked out once and then written for
            # every descriptor that named it, so `[1, 2] => "log"` opens the
            # file once and the two share that one open file, as `>log 2>&1`
            # does.  Opening it once per descriptor would give each its own
            # offset into the same file, and each would write over what the
            # other had written.  Which way it is opened follows the first
            # descriptor named, as it does in Ruby.
            kind, source = _spawn_source(fds[0], value, opened)
            fds.each { |fd| table << fd << kind << source }
          end
        rescue StandardError => e
          # Named rather than re-raised bare: mruby's `raise` with no
          # argument does not re-raise what is being rescued.
          opened.each { |io| io.close }
          raise e
        end
        [table, opened]
      end

      def _spawn_fd(v)
        case v
        when :in then 0
        when :out then 1
        when :err then 2
        when Integer then v
        else
          return v.fileno if v.respond_to?(:fileno)
          raise ArgumentError, "wrong exec redirect: #{v.inspect}"
        end
      end

      # What one redirection value stands for, as the [kind, source] pair the
      # table is written from.  +first_fd+ is the descriptor that decides how
      # a file is opened when the value names one.
      def _spawn_source(first_fd, value, opened)
        case value
        when :close
          [REDIR_CLOSE, -1]
        when String
          io = _spawn_open(value, first_fd == 0 ? "r" : "w", nil)
          opened << io
          [REDIR_PARENT, io.fileno]
        when Array
          if value[0] == :child
            [REDIR_CHILD, _spawn_fd(value[1])]
          else
            io = _spawn_open(value[0], value[1] || "r", value[2])
            opened << io
            [REDIR_PARENT, io.fileno]
          end
        else
          [REDIR_PARENT, _spawn_fd(value)]
        end
      end

      # Opening a file for the child happens here, in the parent, so that the
      # HAL never grows a notion of a filename and every other redirection
      # form works in a build without mruby-io.
      def _spawn_open(path, mode, perm)
        file = begin
                 File
               rescue NameError
                 raise NotImplementedError, "file redirection requires mruby-io"
               end
        perm ? file.open(path, mode, perm) : file.open(path, mode)
      end
    end
  end
end
