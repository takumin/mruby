##
# IO.popen
#
# A pipe to a command is two things this class already has no business doing
# twice: a pipe, which is IO.pipe, and a child process, which is
# Process.spawn.  So IO.popen is their composition, written here rather than
# as a third implementation in C.
#
# Both halves have to be there for it to mean anything, and where one is
# missing the method is missing with it, so that `IO.respond_to?(:popen)` is
# an answer rather than a promise that fails when it is called.  A build
# without mruby-process leaves this file out, which mrbgem.rake decides; a
# platform without the pipe primitive, such as iOS, is what the condition
# below asks about.

class IO
  if respond_to?(:_pipe)
    #
    # call-seq:
    #   IO.popen([env,] cmd, mode="r" [, opt])               -> io
    #   IO.popen([env,] cmd, mode="r" [, opt]) {|io| block } -> obj
    #
    # Runs the specified command as a subprocess; the subprocess's standard input
    # and output will be connected to the returned IO object.
    #
    # +cmd+ is what Process.spawn takes: a String, a `[cmdname, argv0]`
    # pair, or an Array of the command and its arguments, which may start
    # with an environment Hash and end with an options Hash as
    # Process.spawn's arguments may.  +opt+ holds Process.spawn's options
    # as well, and +mode+ may be given there as <code>mode:</code>.
    #
    #   p IO.popen("date").read   #=> "Wed Apr  9 08:56:30 CDT 2003\n"
    #   IO.popen("dc", "r+") {|f|
    #     f.puts "5 2 *"
    #     f.close_write
    #     puts f.read
    #   }
    #   IO.popen(["ls", "-l"], chdir: "/tmp") {|f| puts f.read }
    #
    def self.popen(*args, &block)
      io = _popen_open(args)
      return io unless block

      begin
        yield io
      ensure
        begin
          io.close unless io.closed?
        rescue IOError
          # nothing
        end
      end
    end

    class << self
      # Neither method below is API; both are named to `private` at the end
      # of this block, since a bare `private` inside `class << self` opens no
      # default-visibility scope in mruby.

      # The arguments as CRuby reads them: a trailing Hash is options, a
      # leading one the environment, and what is left is the command and
      # perhaps a mode.
      def _popen_open(args)
        process = begin
                    Process
                  rescue NameError
                    raise NotImplementedError, "popen requires mruby-process"
                  end
        unless process.respond_to?(:spawn)
          raise NotImplementedError, "popen is not supported on this platform"
        end

        args = args.dup
        opts = (args.size > 1 && args[-1].is_a?(Hash)) ? args.pop : nil
        env = (args.size > 1 && args[0].is_a?(Hash)) ? args.shift : nil
        unless args.size == 1 || args.size == 2
          extra = opts ? 1 : 0
          raise ArgumentError, "wrong number of arguments (given #{args.size + extra}, expected #{1 + extra}..#{2 + extra})"
        end
        command, mode = args
        if command == "-"
          raise NotImplementedError, "IO.popen(\"-\") is not supported: mruby has no fork"
        end

        # The options are Process.spawn's, but for the two that are this
        # stream's own: `mode:` is the mode, and `binmode:` asks for what a
        # stream here already is, bytes either way.
        spawn_opts = {}
        (opts || {}).each do |key, value|
          case key
          when :mode
            raise ArgumentError, "mode specified twice" if mode
            mode = value
          when :binmode, :textmode
            # nothing: there is no text mode to leave
          else
            spawn_opts[key] = value
          end
        end
        readable, writable = _popen_mode(mode || "r")

        # An Array is Process.spawn's argument list, options Hash and all;
        # what it holds at the end is merged with what was given outside,
        # neither being allowed to say what the other already said.
        argv = command.is_a?(Array) ? command.dup : [command]
        if argv.size > 1 && argv[-1].is_a?(Hash)
          argv.pop.each do |key, value|
            raise ArgumentError, "wrong exec option" if spawn_opts.key?(key)
            spawn_opts[key] = value
          end
        end
        argv.unshift(env) if env

        read_r = read_w = write_r = write_w = nil
        begin
          read_r, read_w = self._pipe if readable
          write_r, write_w = self._pipe if writable

          # The pipe's ends are the child's 0 and 1, and they are written
          # after the caller's options, so a caller that named either
          # descriptor is told the descriptor was named twice, as CRuby
          # tells it.  The child's standard error is left where this
          # process's is unless `err:` says otherwise: the command's
          # diagnostics go where this process's do, not into the pipe its
          # output is read from.
          spawn_opts[0] = write_r if writable
          spawn_opts[1] = read_w if readable
          pid = process.spawn(*argv, spawn_opts)
        rescue StandardError => e
          # Named rather than re-raised bare: mruby's `raise` with no argument
          # does not re-raise what is being rescued, it raises an empty
          # RuntimeError, which would lose the Errno the failed spawn owes the
          # caller.
          [read_r, read_w, write_r, write_w].each do |io|
            io.close if io && !io.closed?
          end
          raise e
        end

        # The child has the ends it was given; holding them here as well would
        # keep the pipe from ever reaching EOF.
        read_w.close if read_w
        write_r.close if write_r

        io = if readable && writable
               self._duplex(read_r, write_w)
             elsif readable
               read_r
             else
               write_w
             end
        # The command this stream is a pipe to, which #close waits for and
        # #pid names.  Written from here rather than through a setter, so
        # that the stream shows no method CRuby's IO has not got.
        io.instance_eval { @__pid = pid }
        io
      end

      # Which way the pipe runs.  A mode is the same thing it is for IO.new: a
      # String such as "r", "w" or "r+", or the integer flags File::RDONLY and
      # its friends.
      def _popen_mode(mode)
        if mode.is_a?(Integer)
          access = mode & (File::RDONLY | File::WRONLY | File::RDWR)
          return [access == File::RDONLY || access == File::RDWR,
                  access == File::WRONLY || access == File::RDWR]
        end
        mode = mode.to_str
        plus = mode.include?("+")
        case mode[0]
        when "r" then [true, plus]
        when "w", "a" then [plus, true]
        else
          raise ArgumentError, "illegal access mode #{mode}"
        end
      end

      private :_popen_open, :_popen_mode
    end
  end
end
