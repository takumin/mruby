require "digest"
require "find"

module MRuby
  # Where a build came from: the commit the tree was at, and a digest over the
  # sources that went into it.
  #
  # `tasks/build_info.rake` writes these into the binaries, so a benchmark
  # result, a core file, or a binary someone kept around can be traced back to
  # the sources it was built from without running it:
  #
  #   strings build/host/bin/mruby | grep mruby-build-info:
  #
  # Every value here is location independent: the paths that go into the digest
  # are relative to the tree they live in, and a commit hash is a commit hash
  # wherever the checkout sits. Recording them therefore keeps a build
  # reproducible across checkouts, which is the point of having them at all.
  #
  # What the digest does *not* cover is how the sources were built: the compiler
  # and its flags, the build configuration, the gems' link settings. Two builds
  # of the same sources with different flags share a source digest and differ as
  # binaries.
  class BuildInfo
    # Bumped when the layout of the recorded line changes, so a reader can tell
    # what it is looking at.
    FORMAT = "1".freeze

    UNKNOWN = "unknown".freeze

    # Directories under `MRUBY_ROOT` whose contents reach the built binary.
    # `lib` and `tasks` are the build system itself: what it does decides what
    # gets compiled, so it belongs in the digest as much as the C sources do.
    CORE_DIRS = %w[include src mrblib lib tasks].freeze

    # Files under `MRUBY_ROOT` counted the same way, outside of any directory
    # above.
    CORE_FILES = %w[Rakefile].freeze

    class << self
      # One rake invocation walks a directory once however many targets share
      # it. `mrbgems/mruby-compiler/lib/prism` alone is over 20 MB, and every
      # target in the build config holds the same compiler gem.
      def dir_cache
        @dir_cache ||= {}
      end

      # `git` is asked about the tree, not about a target, so its answers are
      # cached for the whole run too.
      def git(*args)
        @git_cache ||= {}
        return @git_cache[args] if @git_cache.key?(args)
        @git_cache[args] = run_git(*args)
      end

      # A C string literal for a value that is written into generated source.
      def c_string(str)
        %|"#{str.gsub(/["\\]/) { |c| "\\#{c}" }}"|
      end

      private

      def run_git(*args)
        return nil unless File.exist?("#{MRUBY_ROOT}/.git")
        out = IO.popen(["git", "-C", MRUBY_ROOT, *args], err: File::NULL, &:read)
        $?.success? ? out.strip : nil
      rescue SystemCallError, IOError
        # No `git` on this machine, or it could not be run. A tree unpacked
        # from an archive is the ordinary case; the digest still works.
        nil
      end
    end

    def initialize(build)
      @build = build
    end

    # The commit `HEAD` was at, or "unknown" outside a git checkout.
    def commit
      self.class.git("rev-parse", "HEAD") || UNKNOWN
    end

    # Whether the tree carried changes the commit does not describe: "1", "0",
    # or "unknown" when there is no git to ask. The source digest is what
    # actually pins the sources down; this is the human-readable warning that
    # the commit alone does not.
    def dirty
      status = self.class.git("status", "--porcelain")
      return UNKNOWN if status.nil?
      status.empty? ? "0" : "1"
    end

    # A digest over every source that reaches this target, as
    # "sha256:<hex>".
    def source_digest
      @source_digest ||= "sha256:#{Digest::SHA256.hexdigest(manifest)}"
    end

    # What `source_digest` is taken over, in `sha256sum` format so the entries
    # can be read and compared directly. Written beside the build, so two
    # builds whose digests differ can be diffed down to the file.
    def manifest
      @manifest ||= entries.map { |path, digest| "#{digest}  #{path}\n" }.join
    end

    # The one line that goes into the binary. Kept on a single line, and led by
    # a marker no other string in the binary starts with, so `strings | grep`
    # is all it takes to read it back out.
    def line
      "mruby-build-info:#{FORMAT}" \
        " target=#{@build.name}" \
        " commit=#{commit}" \
        " dirty=#{dirty}" \
        " source-digest=#{source_digest}"
    end

    private

    # `[logical path, digest]` for every source file, sorted.
    #
    # The paths are logical: relative to `MRUBY_ROOT` for the core, and
    # `gems/<name>/...` for a gem, whose directory may sit anywhere once it
    # was fetched from a remote. Neither says where the tree is checked out,
    # which is what keeps the digest the same across checkouts.
    def entries
      list = []
      CORE_DIRS.each { |dir| list.concat(dir_entries("#{MRUBY_ROOT}/#{dir}", dir)) }
      CORE_FILES.each do |name|
        path = "#{MRUBY_ROOT}/#{name}"
        list << [name, file_digest(path)] if File.file?(path)
      end
      @build.gems.each do |gem|
        list.concat(dir_entries(gem.dir, "gems/#{gem.name}", [gem.build_dir]))
      end
      list.sort!
      list
    end

    def dir_entries(dir, prefix, excludes = [])
      return [] unless File.directory?(dir)
      key = [File.expand_path(dir), *excludes.map { |e| File.expand_path(e) }.sort]
      scanned = self.class.dir_cache[key] ||= scan(key[0], key[1..-1])
      scanned.map { |rel, digest| ["#{prefix}/#{rel}", digest] }
    end

    def scan(dir, excludes)
      offset = dir.length + 1
      list = []
      Find.find(dir) do |path|
        if File.directory?(path)
          # `.git` is the checkout's own bookkeeping, and a gem that keeps its
          # build products inside its own directory would otherwise digest what
          # this build just wrote.
          Find.prune if File.basename(path) == ".git" || excludes.include?(path)
          next
        end
        next unless File.file?(path)  # a dangling symlink has nothing to read
        list << [path[offset..-1], file_digest(path)]
      end
      list
    end

    def file_digest(path)
      Digest::SHA256.file(path).hexdigest
    end
  end
end
