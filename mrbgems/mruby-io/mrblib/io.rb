##
# IO
#
# ISO 15.2.20

class IOError < StandardError; end
class EOFError < IOError; end

class IO
  #
  # call-seq:
  #   IO.open(fd, mode="r" [, opt])                -> io
  #   IO.open(fd, mode="r" [, opt]) {|io| block }  -> obj
  #
  # With no associated block, IO.open is a synonym for IO.new. If the optional
  # code block is given, it will be passed io as an argument, and the IO object
  # will automatically be closed when the block terminates. In this instance,
  # IO.open returns the value of the block.
  #
  #   fd = IO.sysopen("/dev/tty", "w")
  #   a = IO.open(fd,"w")
  #   $stderr.puts "Hello"
  #   a.close
  #
  def self.open(*args, &block)
    io = self.new(*args)

    return io unless block

    begin
      yield io
    ensure
      begin
        io.close unless io.closed?
      rescue StandardError
      end
    end
  end

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
      # As it has always been here, and unlike CRuby: with nothing said about
      # it, the child's standard error follows its standard output.
      child_err ||= child_out

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
    io._child = process.child(pid)
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

  #
  # call-seq:
  #   IO.pipe                    -> [read_io, write_io]
  #   IO.pipe {|read_io, write_io| ... } -> obj
  #
  # Creates a pair of pipe endpoints (connected to each other) and returns
  # them as a two-element array of IO objects: [read_io, write_io].
  #
  #   rd, wr = IO.pipe
  #   if fork
  #     wr.close
  #     puts rd.read
  #     rd.close
  #     Process.wait
  #   else
  #     rd.close
  #     wr.write "Hello, parent!"
  #     wr.close
  #     exit
  #   end
  #
  def self.pipe(&block)
    if !self.respond_to?(:_pipe)
      raise NotImplementedError, "pipe is not supported on this platform"
    end
    if block
      begin
        r, w = IO._pipe
        yield r, w
      ensure
        r.close unless r.closed?
        w.close unless w.closed?
      end
    else
      IO._pipe
    end
  end

  #
  # call-seq:
  #   IO.read(name, [length [, offset]] )   -> string
  #   IO.read(name, [length [, offset]], mode: mode)   -> string
  #
  # Opens the file, optionally seeks to the given offset, then returns length
  # bytes (defaulting to the rest of the file). read ensures the file is
  # closed before returning.
  #
  #   IO.read("testfile")           #=> "This is line one\nThis is line two\n"
  #   IO.read("testfile", 20)       #=> "This is line one\nTh"
  #   IO.read("testfile", 20, 10)   #=> "ne one\nThis is line "
  #
  def self.read(path, length=nil, offset=0, mode: "r")
    str = ""
    fd = -1
    io = nil
    begin
      fd = IO.sysopen(path, mode)
      io = IO.open(fd, mode)
      io.seek(offset) if offset > 0
      str = io.read(length)
    ensure
      if io
        io.close
      elsif fd != -1
        IO._sysclose(fd)
      end
    end
    str
  end

  #
  # call-seq:
  #   ios.close -> nil
  #
  # Closes the stream.  For a stream `IO.popen` returned, the command at the
  # other end is waited for as well, and `$?` is set to how it finished --
  # which is why this is here rather than in C: the wait belongs to the child
  # this stream was given, and `Process` is what knows about children.
  #
  def close
    child = @__child
    begin
      _close
    ensure
      # Waiting twice is what the child object is for: closing a stream that
      # was already waited for elsewhere reaches no further than the status
      # it kept, and #pid goes on naming the process afterwards.
      child.wait if child
    end
    nil
  end

  #
  # call-seq:
  #   ios.pid -> integer or nil
  #
  # Returns the process ID of the command on the other end of the pipe, or
  # `nil` if the stream is not a pipe to one.
  #
  #   io = IO.popen("date")
  #   p io.pid   #=> 2056
  #   io.read
  #   io.close
  #
  def pid
    child = @__child
    child && child.pid
  end

  # Internal: remember the child this stream is a pipe to, so that #close can
  # wait for it and #pid can name it.  IO.popen is the only caller.
  def _child=(child)
    @__child = child
  end

  #
  # call-seq:
  #   ios.hash   -> integer
  #
  # Compute a hash based on the IO object. Two IO objects with the same
  # content will have the same hash code (and will compare using eql?).
  # We must define IO#hash here because IO includes Enumerable and
  # Enumerable#hash will call IO#read() otherwise.
  #
  def hash
    self.__id__
  end


  # Alias for eof?
  alias_method :eof, :eof?
  # Alias for pos
  alias_method :tell, :pos

  #
  # call-seq:
  #   ios.pos = integer    -> integer
  #
  # Seeks to the given position (in bytes) in ios. It is not guaranteed that
  # seeking to the right position when ios is textmode.
  #
  #   f = File.new("testfile")
  #   f.pos = 17
  #   f.gets   #=> "This is line two\n"
  #
  def pos=(i)
    seek(i, SEEK_SET)
  end

  #
  # call-seq:
  #   ios.rewind    -> 0
  #
  # Positions ios to the beginning of input, resetting lineno to zero.
  #
  #   f = File.new("testfile")
  #   f.readline   #=> "This is line one\n"
  #   f.rewind     #=> 0
  #   f.lineno     #=> 0
  #   f.readline   #=> "This is line one\n"
  #
  def rewind
    seek(0, SEEK_SET)
  end


  #
  # call-seq:
  #   ios.each(sep=$/) {|line| block }         -> ios
  #   ios.each(limit) {|line| block }          -> ios
  #   ios.each(sep,limit) {|line| block }      -> ios
  #   ios.each(...)                            -> an_enumerator
  #
  # Executes the block for every line in ios, where lines are separated by sep.
  # ios must be opened for reading. If no block is given, an enumerator is returned instead.
  #
  #   f = File.new("testfile")
  #   f.each {|line| puts "#{f.lineno}: #{line}" }
  #
  # 15.2.20.5.3
  def each(&block)
    return to_enum(:each) unless block

    while line = self.gets
      block.call(line)
    end
    self
  end

  #
  # call-seq:
  #   ios.each_byte {|byte| block }  -> ios
  #   ios.each_byte                  -> an_enumerator
  #
  # Calls the given block once for each byte (0..255) in ios, passing the byte
  # as an argument. The stream must be opened for reading or an IOError will be raised.
  #
  #   f = File.new("testfile")
  #   checksum = 0
  #   f.each_byte {|x| checksum ^= x }   #=> #<File:testfile>
  #   checksum                           #=> 12
  #
  # 15.2.20.5.4
  def each_byte(&block)
    return to_enum(:each_byte) unless block

    while byte = self.getbyte
      block.call(byte)
    end
    self
  end

  # Alias for each - 15.2.20.5.5
  alias each_line each

  #
  # call-seq:
  #   ios.each_char {|c| block }  -> ios
  #   ios.each_char               -> an_enumerator
  #
  # Calls the given block once for each character in ios, passing the character
  # as an argument. The stream must be opened for reading or an IOError will be raised.
  #
  #   f = File.new("testfile")
  #   ios.each_char {|c| print c, ' ' }   #=> #<File:testfile>
  #
  def each_char(&block)
    return to_enum(:each_char) unless block

    while char = self.getc
      block.call(char)
    end
    self
  end



  #
  # call-seq:
  #   ios.printf(format_string [, obj, ...])    -> nil
  #
  # Formats and writes to ios, converting parameters under control of the format string.
  # See sprintf for details of the format string.
  #
  #   $stdout.printf "Number: %5.2f,\nString: %s\n", 1.23, "hello"
  #   Number:  1.23,
  #   String: hello
  #
  def printf(*args)
    write sprintf(*args)
    nil
  end

  # Alias for fileno - returns the integer file descriptor for ios
  alias_method :to_i, :fileno
  # Alias for isatty - returns true if ios is associated with a terminal device
  alias_method :tty?, :isatty
end

# Standard input stream - connected to file descriptor 0
STDIN  = IO.open(0, "r")
# Standard output stream - connected to file descriptor 1
STDOUT = IO.open(1, "w")
# Standard error stream - connected to file descriptor 2
STDERR = IO.open(2, "w")

# Global variable for standard input
$stdin  = STDIN
# Global variable for standard output
$stdout = STDOUT
# Global variable for standard error
$stderr = STDERR
