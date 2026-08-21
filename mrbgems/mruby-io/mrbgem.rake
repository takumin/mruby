MRuby::Gem::Specification.new('mruby-io') do |spec|
  spec.license = 'MIT'
  spec.authors = ['Internet Initiative Japan Inc.', 'mruby developers']
  spec.summary = 'IO and File class'

  spec.build.defines << "HAVE_MRUBY_IO_GEM"
  spec.add_test_dependency 'mruby-time', core: 'mruby-time'
  # IO.popen is a pipe plus a child process, and the child is mruby-process's
  # to create and to wait for.  Only the tests need it: the method resolves
  # `Process` when it is called, so a build without that gem still builds and
  # tells the caller at the call site.
  spec.add_test_dependency 'mruby-process', core: 'mruby-process'

  if spec.for_windows?
    spec.linker.libraries << "ws2_32"
  end
end
