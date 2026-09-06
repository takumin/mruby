##
# VFS: the virtual filesystem that `require`, `require_relative` and `load`
# read from.  Backends are mounted at path prefixes; a path is answered by
# the mount with the longest prefix that covers it, and the mount at "/"
# covers everything else.  A backend is any object that answers `stat(path)`
# with :file, :directory, :other or nil, and `read(path)` with a String or
# nil, where path is the part below the mount, starting with the slash.
module VFS
  class << self
    ##
    # call-seq:
    #   VFS.mount(prefix, backend) -> backend
    #
    # Mounts backend at prefix, an absolute path, in place of whatever was
    # mounted there; "/" is the root.
    #
    #   VFS.mount("/lib", VFS::Memory.new("hello.rb" => "def hello; end"))
    #   $LOAD_PATH << "/lib"
    #   require "hello"
    #
    def mount(prefix, backend)
      prefix = _mount_prefix(prefix)
      unless backend.respond_to?(:stat) && backend.respond_to?(:read)
        raise TypeError, "VFS backend must respond to stat and read"
      end
      umount(prefix)
      # longest prefix first, so that the first match is the closest mount
      i = 0
      i += 1 while i < @mounts.size && @mounts[i][0].size > prefix.size
      @mounts[i, 0] = [[prefix, backend]]
      backend
    end

    ##
    # call-seq:
    #   VFS.umount(prefix) -> backend or nil
    #
    # Unmounts whatever is at prefix and returns it; nil when nothing was.
    #
    def umount(prefix)
      prefix = _mount_prefix(prefix)
      i = 0
      while i < @mounts.size
        return @mounts.delete_at(i)[1] if @mounts[i][0] == prefix
        i += 1
      end
      nil
    end

    ##
    # call-seq:
    #   VFS.mounts -> array
    #
    # The mount table as [prefix, backend] pairs, in the order paths are
    # matched against them: longest prefix first.
    #
    def mounts
      @mounts.map { |m| m.dup }
    end

    ##
    # call-seq:
    #   VFS.exist?(path) -> true or false
    #
    # Whether path names anything.
    #
    def exist?(path)
      !stat(path).nil?
    end

    ##
    # call-seq:
    #   VFS.file?(path) -> true or false
    #
    # Whether path names a regular file, one that `VFS.read` can answer.
    #
    def file?(path)
      stat(path) == :file
    end

    ##
    # call-seq:
    #   VFS.directory?(path) -> true or false
    #
    # Whether path names a directory.
    #
    def directory?(path)
      stat(path) == :directory
    end

    private

    # The form a prefix is kept in: "/" for the root, and for any other an
    # absolute path with no slash at its end
    def _mount_prefix(prefix)
      raise TypeError, "mount prefix must be a String" unless prefix.is_a?(String)
      raise ArgumentError, "mount prefix must be absolute" unless prefix[0] == "/"
      prefix = prefix[0, prefix.size - 1] while prefix.size > 1 && prefix[prefix.size - 1] == "/"
      prefix
    end
  end

  ##
  # A backend over a Hash: paths to their contents, all in memory.  Paths are
  # kept normalized, absolute with no "." or ".." in them, so a file put in
  # under "lib/hello.rb" is the one "/lib/hello.rb" reads.  Any directory a
  # file lies under exists.
  #
  #   mem = VFS::Memory.new("lib/hello.rb" => "def hello; :hi; end")
  #   VFS.mount("/mem", mem)
  #   VFS.stat("/mem/lib")             #=> :directory
  #   VFS.read("/mem/lib/hello.rb")    #=> "def hello; :hi; end"
  #
  class Memory
    def initialize(files = nil)
      @files = {}
      files.each { |path, content| self[path] = content } if files
    end

    ##
    # call-seq:
    #   memory[path] = content -> content
    #
    # Puts a file in.
    #
    def []=(path, content)
      raise TypeError, "file content must be a String" unless content.is_a?(String)
      @files[_normalize(path)] = content
    end

    ##
    # call-seq:
    #   memory[path] -> string or nil
    #   memory.read(path) -> string or nil
    #
    # The content of the file at path, or nil when there is none.
    #
    def [](path)
      @files[_normalize(path)]
    end
    alias read []

    ##
    # call-seq:
    #   memory.delete(path) -> string or nil
    #
    # Takes the file at path out and returns its content.
    #
    def delete(path)
      @files.delete(_normalize(path))
    end

    ##
    # call-seq:
    #   memory.paths -> array
    #
    # The paths of every file, in the order they were put in.
    #
    def paths
      @files.keys
    end

    ##
    # call-seq:
    #   memory.stat(path) -> :file, :directory or nil
    #
    def stat(path)
      path = _normalize(path)
      return :file if @files.key?(path)
      return :directory if path == "/"
      dir = path + "/"
      @files.each_key { |k| return :directory if k[0, dir.size] == dir }
      nil
    end

    private

    def _normalize(path)
      raise TypeError, "path must be a String" unless path.is_a?(String)
      parts = []
      path.split("/").each do |seg|
        if seg == ".."
          parts.pop
        elsif !seg.empty? && seg != "."
          parts << seg
        end
      end
      "/" + parts.join("/")
    end
  end

  # the port's files, wherever no other mount answers
  mount("/", Host.new) if const_defined?(:Host)
end

class LoadError
  ##
  # call-seq:
  #   load_error.path -> string or nil
  #
  # The name that could not be loaded.
  #
  attr_reader :path
end
