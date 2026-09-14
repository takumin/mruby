MRuby::Toolchain.new(:gcc) do |conf, params|
  default_command = params[:default_command] || 'gcc'
  compiler_flags = %w(-g -O3 -Wall -Wundef)
  c_mandatory_flags = %w(-std=gnu99)
  cxx_invalid_flags = %w(-Werror-implicit-function-declaration)
  compile_opt = '%{flags} -o "%{outfile}" "%{infile}"'

  [conf.cc, conf.objc, conf.asm, conf.cxx].each do |compiler|
    if compiler == conf.cxx
      compiler.command = ENV['CXX'] || conf.cc.command.sub(/g\Kcc|$/, '++')
      compiler.flags = [ENV['CXXFLAGS'] || ENV['CFLAGS'] || compiler_flags]
    else
      compiler.command = ENV['CC'] || default_command
      compiler.flags = [c_mandatory_flags, ENV['CFLAGS'] || [compiler_flags, cxx_invalid_flags, %w(-Wwrite-strings)]]
    end
    compiler.option_include_path = %q[-I"%s"]
    compiler.option_define = '-D%s'
    compiler.compile_options = "-MMD -c #{compile_opt}"
    compiler.preprocess_options = "-E -P #{compile_opt}"
    compiler.cxx_compile_flag = '-x c++ -std=gnu++03'
    compiler.cxx_exception_flag = '-fexceptions'
    compiler.cxx_invalid_flags = c_mandatory_flags + cxx_invalid_flags

    def compiler.setup_debug(conf)
      self.flags << %w(-g3 -O0)
    end
  end

  conf.linker do |linker|
    linker.command = ENV['LD'] || ENV['CXX'] || ENV['CC'] || default_command
    linker.flags = [ENV['LDFLAGS'] || %w()]
    if ENV['OS'] == 'Windows_NT'
      linker.libraries = []
    else
      linker.libraries = %w(m)
    end
    linker.library_paths = []
    linker.option_library = '-l%s'
    linker.option_library_path = '-L%s'
    linker.option_use_linker = '-fuse-ld=%s'
    # The drivers of this family all take `-fuse-ld=`, so a build for this
    # machine looks for a linker faster than the `ld` they would otherwise
    # reach for. `mold` links a binary of this size several times faster than
    # GNU ld, and `lld`, which more machines have already, sits between the
    # two. Which of them is installed is settled by a link and not by this
    # list, so a machine with neither builds as it always did.
    #
    # A cross build looks for nothing and names its linkers itself. Linking
    # for another target is often more than resolving symbols: a linker
    # script places the image of a bare-metal target, and a specs file of the
    # toolchain says how. A probe that links a program answers whether a
    # linker runs, and not whether the image it wrote is the one that target
    # boots.
    linker.preferred_linkers = conf.kind_of?(MRuby::CrossBuild) ? [] : %w(mold lld)
    linker.link_options = '%{flags} -o "%{outfile}" %{objs} %{flags_before_libraries} %{libs} %{flags_after_libraries}'
  end

  [[conf.cc, 'c'], [conf.cxx, 'c++']].each do |cc, lang|
    cc.instance_variable_set :@header_search_language, lang
    def cc.header_search_paths
      if @header_search_command != command
        result = `echo | #{build.filename command} -x#{@header_search_language} -Wp,-v - -fsyntax-only 2>&1`
        return include_paths if $?.exitstatus != 0

        @frameworks = []
        @header_search_paths = result.lines.map { |v|
          framework = v.match(/^ (.*)(?: \(framework directory\))$/)
          if framework
            @frameworks << framework[1]
            next nil
          end

          v.match(/^ (.*)$/)
        }.compact.map { |v| v[1] }.select { |v| File.directory? v }
        @header_search_paths += include_paths
        @header_search_command = command
      end
      @header_search_paths
    end
  end

  def conf.enable_sanitizer(*opts)
    fail 'sanitizer already set' if @sanitizer_list

    @sanitizer_list = opts
    flg = "-fsanitize=#{opts.join ','}"
    [self.cc, self.cxx, self.linker].each{|cmd| cmd.flags << flg }
  end
end
