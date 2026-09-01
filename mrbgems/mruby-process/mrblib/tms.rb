module Process
  # What Process.times answers with: how much CPU time this process, and the
  # children it has already reaped, have used.
  #
  # A Struct of the four members, as CRuby's own Process::Tms is, so a Tms
  # answers to everything a Struct does (#[], #each, #to_h, #dig, a writer
  # per member, and the rest) and prints, compares and unpacks the way any
  # other Struct value does. The four values are stored exactly as
  # Process.times computed them, with nothing left to decode.
  #
  # #utime and #stime are this process's own user and system CPU time, in
  # seconds; #cutime and #cstime total the same over every terminated child
  # this process has waited for so far. A child still running, or one never
  # waited for, is not counted.
  Tms = Struct.new(:utime, :stime, :cutime, :cstime)
end
