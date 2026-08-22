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
    #   IO.popen(cmd, mode="r" [, opt])               -> io
    #   IO.popen(cmd, mode="r" [, opt]) {|io| block } -> obj
    #
    # Runs the specified command as a subprocess; the subprocess's standard input
    # and output will be connected to the returned IO object.
    #
    #   p IO.popen("date").read   #=> "Wed Apr  9 08:56:30 CDT 2003\n"
    #   IO.popen("dc", "r+") {|f|
    #     f.puts "5 2 *"
    #     f.close_write
    #     puts f.read
    #   }
    #
    def self.popen(command, mode = 'r', **opts, &block)
      io = _popen_open(command, mode, opts)
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

    # A pipe to a command is a composition of two things this class already
    # has no business doing twice: pipes, which are IO.pipe, and a child
    # process, which is Process.spawn.  So it is written here rather than in C,
    # and a build without mruby-process has no command to open a pipe to.
    def self._popen_open(command, mode, opts)
      process = begin
                  Process
                rescue NameError
                  raise NotImplementedError, "popen requires mruby-process"
                end
      unless process.respond_to?(:spawn)
        raise NotImplementedError, "popen is not supported on this platform"
      end
      if command == "-"
        raise NotImplementedError, "IO.popen(\"-\") is not supported: mruby has no fork"
      end

      readable, writable = _popen_mode(mode)
      child_in = child_out = child_err = nil
      opts.each_key do |key|
        unless key == :in || key == :out || key == :err
          raise ArgumentError, "unknown keyword: #{key.inspect}"
        end
      end
      child_in = _popen_fd(opts[:in]) if opts.key?(:in)
      child_out = _popen_fd(opts[:out]) if opts.key?(:out)
      child_err = _popen_fd(opts[:err]) if opts.key?(:err)

      read_r = read_w = write_r = write_w = nil
      begin
        read_r, read_w = self._pipe if readable
        write_r, write_w = self._pipe if writable

        child_in ||= write_r
        child_out ||= read_w
        # Nothing is done with the child's standard error unless `err:` says so,
        # which is what this gem did before and what Ruby does: the command's
        # diagnostics go where this process's do, not into the pipe its output
        # is read from.

        spawn_opts = {}
        spawn_opts[:in] = child_in if child_in
        spawn_opts[:out] = child_out if child_out
        spawn_opts[:err] = child_err if child_err
        pid = process.spawn(command, spawn_opts)
      rescue StandardError
        [read_r, read_w, write_r, write_w].each do |io|
          io.close if io && !io.closed?
        end
        raise
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
      io._pid = pid
      io
    end

    # Which way the pipe runs.  A mode is the same thing it is for IO.new: a
    # String such as "r", "w" or "r+", or the integer flags File::RDONLY and
    # its friends.
    def self._popen_mode(mode)
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

    # What a redirection option names, as a descriptor.  Deliberately narrower
    # than Process.spawn's own options: this has always taken an IO or a
    # descriptor number and nothing else, and a String here would silently
    # become a file to open.
    def self._popen_fd(value)
      return value if value.is_a?(Integer)
      return value.fileno if value.is_a?(IO)
      raise ArgumentError, "wrong exec redirect action"
    end
  end
end
