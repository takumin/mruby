MRuby::Gem::Specification.new('mruby-process') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mruby developers'
  spec.summary = 'Process module and Process::Status'

  # `Process.kill(:TERM, pid)` takes a signal by name and `Process::Status#to_s`
  # spells one out, so both need the platform's signal table.  mruby-signal owns
  # it; this gem reaches it through signal_hal.h rather than keeping a copy.
  spec.add_dependency 'mruby-signal', core: 'mruby-signal'

  # A gem's tests run in a state holding its dependency closure and nothing
  # else, so a test that means to name an Errno class has to ask for the gem
  # that defines them.
  spec.add_test_dependency 'mruby-errno', core: 'mruby-errno'

  # The tests ask Object.const_defined? whether this build has a Float or a
  # File, and #const_defined? is mruby-metaprog's to define.
  spec.add_test_dependency 'mruby-metaprog', core: 'mruby-metaprog'
end
