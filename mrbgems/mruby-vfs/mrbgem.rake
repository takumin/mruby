MRuby::Gem::Specification.new('mruby-vfs') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'Virtual filesystem with require, require_relative and load'

  spec.add_test_dependency 'mruby-errno', core: 'mruby-errno'

  # Ruby source is compiled by mruby-compiler when the build carries it.  A
  # build without it (one that ships bytecode alone, as with mruby-bin-mrb)
  # still loads `.mrb` files, so the gem does not depend on the compiler;
  # src/require.c asks this define which of the two builds it is in.  Which
  # gems the build carries can only be asked once every gem has had its say,
  # which is what `build_settings` waits for.
  spec.build_settings do
    if spec.build.gems.any? { |g| g.name == 'mruby-compiler' }
      spec.compilers.each { |c| c.defines << 'MRB_VFS_HAVE_COMPILER' }
    end
  end
end
