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

    class << self
      #
      # call-seq:
      #   Process.spawn(command... [, options]) -> pid
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
      def spawn(*args)
        kind, argv = _spawn_normalize(args)
        __spawn(kind, argv)
      end

      private

      def _spawn_normalize(args)
        args = args.dup
        if args.empty?
          raise ArgumentError, "wrong number of arguments (given 0, expected 1+)"
        end
        # A trailing Hash is options.  None are supported here, and an option
        # nothing acts on has to be refused rather than dropped: a caller that
        # wrote one is expecting it to happen.
        if args.size >= 2 && args[-1].is_a?(Hash)
          args.pop.each_key do |key|
            raise ArgumentError, "wrong exec option: #{key.inspect}"
          end
        end

        if args[0].is_a?(Array)
          raise NotImplementedError, "[cmdname, argv0] form is not supported"
        end
        if args.size == 1
          _spawn_command_line(_spawn_str(args[0]))
        else
          [SPAWN_ARGV, args.map { |a| _spawn_str(a) }]
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
    end
  end
end
