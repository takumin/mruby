MRuby::Gem::Specification.new('mruby-alloc-fault') do |spec|
  spec.license = 'MIT'
  spec.author  = 'mruby developers'
  spec.summary = 'allocation failure injection driver'
  spec.bins = %w(mruby-alloc-fault)
  spec.add_dependency('mruby-compiler', core: 'mruby-compiler')

  # `rake test:alloc-fault` sweeps every scenario, refusing one allocation
  # per run.  It is not part of `test:run`: a sweep starts one process per
  # allocation and takes minutes, where the bintest beside it keeps the fast
  # gate on the driver itself.
  #
  # SWEEP_ARGS reaches sweep.rb as it stands, which is how a CI job asks for
  # its shard (`SWEEP_ARGS="--shard 2/4"`).
  sweep = File.expand_path('sweep.rb', File.dirname(__FILE__))
  driver = build.exefile("#{build.build_dir}/bin/mruby-alloc-fault")

  desc "sweep the allocations of #{build.name}, refusing one per run"
  sweep_task = task "test:alloc-fault:#{build.name}" => driver do
    sh "ruby #{sweep} --bin #{driver} #{ENV['SWEEP_ARGS']}"
  end
  task "test:alloc-fault" => sweep_task
end
