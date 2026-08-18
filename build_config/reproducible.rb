# A build whose binaries can be compared and traced back.
#
#   rake MRUBY_CONFIG=reproducible
#
# Two builds of the same sources produce byte identical binaries, wherever the
# tree is checked out, and each binary says which sources it came from:
#
#   strings build/reproducible/bin/mruby | grep mruby-build-info:
#   build/reproducible/bin/mruby --version
#
# Which is what it takes to hold a benchmark result against the code that
# produced it: a run that died half way leaves a binary, and the binary is
# enough to say whether the next run is measuring the same thing.
MRuby::Build.new('reproducible') do |conf|
  conf.toolchain

  # Without this, an otherwise identical build made under a different path is a
  # different binary. `-g` is on by default and writes the compile directory
  # into the debug info, and `__FILE__` in assertions writes the path into
  # `.rodata`; mapping the tree root to a fixed name takes both back out.
  #
  # Needs GCC 8+ or Clang 10+. Older compilers have `-fdebug-prefix-map`, which
  # covers the debug info but leaves `__FILE__` alone.
  conf.compilers.each { |c| c.flags << "-ffile-prefix-map=#{conf.root}=/mruby" }

  # Record the commit and a digest of the sources into the binaries.
  conf.enable_build_info

  conf.gembox 'default'
end
