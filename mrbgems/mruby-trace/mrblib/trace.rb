module Trace
  # Records the calls made by the block and returns the folded stacks.
  # Whatever was recorded before is thrown away first, and recording is
  # stopped even if the block raises.
  #
  #   File.write("out.folded", Trace.record { work })
  #
  def self.record(unit = :ns)
    clear
    start
    begin
      yield
    ensure
      stop
    end
    folded(unit)
  end

  # Writes the folded stacks to +path+, ready for flamegraph.pl.
  # Needs a build with File (mruby-io).
  def self.write(path, unit = :ns)
    File.open(path, 'w') { |f| f.write(folded(unit)) }
    path
  end
end
