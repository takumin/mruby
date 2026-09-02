MRuby::Gem::Specification.new('mruby-io') do |spec|
  spec.license = 'MIT'
  spec.authors = ['Internet Initiative Japan Inc.', 'mruby developers']
  spec.summary = 'IO and File class'

  spec.build.defines << "HAVE_MRUBY_IO_GEM"
  spec.add_test_dependency 'mruby-time', core: 'mruby-time'
  spec.add_test_dependency 'mruby-errno', core: 'mruby-errno'
  spec.add_test_dependency 'mruby-process', core: 'mruby-process'

  # IO.popen is a pipe plus a child process, and the child is mruby-process's
  # to create and to wait for.  A build without that gem therefore has no
  # IO.popen at all: the file that defines it is left out, so that asking
  # `IO.respond_to?(:popen)` is answered rather than met with a method that
  # raises as soon as it is called.  No dependency is declared in either
  # direction; what is asked is only whether the build already has the gem.
  unless build.gems.any? {|g| g.name == 'mruby-process'}
    spec.rbfiles.reject! {|f| File.basename(f) == 'popen.rb'}
  end

  if spec.for_windows?
    spec.linker.libraries << "ws2_32"
  end
end
