# Every method written in Ruby costs an RProc in the object heap at startup,
# even though the irep behind it is already read-only. Moving those definitions
# into read-only method tables would take them out of RAM, but only the ones a
# build can pin down. `rake rom_coverage` reports how many that is, over the
# same units the build compiles: the files of `mrblib`, then each gem's
# `mrblib`. What the count is made of is written in tools/rom_coverage.
#
# The tool links against the host build's libmruby because it uses the
# compiler to read the sources, so it is built for the host target alone.
MRuby.each_target do |build|
  next unless build.host?
  next unless build.gems['mruby-compiler']

  src = "#{MRUBY_ROOT}/tools/rom_coverage/rom_coverage.c"
  obj = build.objfile("#{build.build_dir}/tools/rom_coverage/rom_coverage")
  exe = build.exefile("#{build.build_dir}/bin/rom_coverage")
  manifest = "#{build.build_dir}/tools/rom_coverage/units.tsv"

  file obj => src do |t|
    build.cc.run t.name, t.prerequisites.first
  end

  file exe => [obj, *build.libraries] do |t|
    build.linker.run t.name, t.prerequisites, *build.gems.linker_attrs
  end

  # One unit per line: its name, then the files that make it up, in the order
  # the build feeds them to the compiler.
  file manifest => [__FILE__, *build.libraries] do |t|
    units = [['mrblib', Dir["#{MRUBY_ROOT}/mrblib/*.rb"].sort]]
    build.gems.each do |gem|
      units << [gem.name, gem.rbfiles] unless gem.rbfiles.empty?
    end
    mkdir_p File.dirname(t.name)
    File.write(t.name, units.map { |name, files| [name, *files].join("\t") }.join("\n") + "\n")
  end

  desc 'report how much of the Ruby code a build loads could be defined from ROM'
  task :rom_coverage => [exe, manifest] do
    sh exe, manifest, *(ENV['VERBOSE'] ? ['-v'] : [])
  end
end
