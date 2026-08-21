module Process
  # Whether this build can create processes at all.  A platform without
  # process creation leaves Process.spawn undefined rather than defining one
  # that always fails, so a program can ask before it commits to a plan that
  # needs a child.
  if respond_to?(:__spawn)
    # Internal encodings, shared with src/spawn.c.  They are not API: what a
    # caller passes is the CRuby argument shape below, and this is only how
    # it reaches C without the C side having to take a Hash apart.
    SPAWN_ARGV  = 0
    SPAWN_SHELL = 1

    REDIR_PARENT = 0
    REDIR_CHILD  = 1
    REDIR_CLOSE  = 2

    SPAWN_UNSETENV_OTHERS = 1
    SPAWN_CLOSE_OTHERS    = 2

    SPAWN_OPTION_KEYS = [:unsetenv_others, :close_others, :chdir]

    class << self
      #
      # call-seq:
      #   Process.spawn([env, ] command... [, options]) -> pid
      #
      # Runs +command+ in a child process and returns its process ID without
      # waiting for it.  The caller owes that child a wait: Process.waitpid,
      # or Process.child(pid).wait, or Process.detach to say it will not be
      # waited for at all.
      #
      # A single String is run through the system shell; two or more
      # arguments are the command and its arguments, run directly.
      #
      #   Process.spawn("echo hello > out.txt")     # through the shell
      #   Process.spawn("echo", "hello")            # no shell involved
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
        flags |= SPAWN_UNSETENV_OTHERS if opts[:unsetenv_others]
        flags |= SPAWN_CLOSE_OTHERS if opts[:close_others]
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
          [env, SPAWN_SHELL, [_spawn_str(args[0])], opts]
        else
          [env, SPAWN_ARGV, args.map { |a| _spawn_str(a) }, opts]
        end
      end

      def _spawn_str(v)
        return v if v.is_a?(String)
        unless v.respond_to?(:to_str)
          raise TypeError, "no implicit conversion of #{v.class} into String"
        end
        v.to_str
      end

      # [name, value, name, value, ...]; a nil value means "unset".  Deltas,
      # not a whole environment, so nothing here has to read the parent's --
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
            keys = key.is_a?(Array) ? key : [key]
            keys.each { |k| _spawn_redirect(_spawn_fd(k), value, table, opened) }
          end
        rescue StandardError
          opened.each { |io| io.close }
          raise
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

      def _spawn_redirect(child_fd, value, table, opened)
        case value
        when :close
          table << child_fd << REDIR_CLOSE << -1
        when String
          io = _spawn_open(value, child_fd == 0 ? "r" : "w", nil)
          opened << io
          table << child_fd << REDIR_PARENT << io.fileno
        when Array
          if value[0] == :child
            table << child_fd << REDIR_CHILD << _spawn_fd(value[1])
          else
            io = _spawn_open(value[0], value[1] || "r", value[2])
            opened << io
            table << child_fd << REDIR_PARENT << io.fileno
          end
        else
          table << child_fd << REDIR_PARENT << _spawn_fd(value)
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
