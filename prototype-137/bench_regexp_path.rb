# The regexp path, which every candidate routes differently.  Today it is
# send -> Ruby wrapper.  Under B it is send -> cfunc -> funcall -> Ruby, and
# under C it is send -> builtin -> hook -> funcall -> Ruby, so both prototypes
# add a hop the shipped code would not have.  Measured so the fast-path win is
# not being paid for here unnoticed.

N = 200_000
REPS = 5

s = "hello world"
re = /w(o)r/
r = nil

def show(label, delta, n)
  ns = delta / n * 1_000_000_000
  puts label + " " + ((ns * 10).round / 10.0).to_s + " ns/iter"
end

ctl = nil; are = nil; arec = nil; slc = nil

REPS.times do
  t = Time.now; i = 0; while i < N; i += 1; end; d = Time.now - t
  ctl = d if ctl.nil? || d < ctl

  t = Time.now; i = 0; while i < N; r = s[re]; i += 1; end; d = Time.now - t
  are = d if are.nil? || d < are

  t = Time.now; i = 0; while i < N; r = s[re, 1]; i += 1; end; d = Time.now - t
  arec = d if arec.nil? || d < arec

  t = Time.now; i = 0; while i < N; r = s.slice(re, 1); i += 1; end; d = Time.now - t
  slc = d if slc.nil? || d < slc
end

show("s[re]        ", are - ctl, N)
show("s[re, 1]     ", arec - ctl, N)
show("s.slice(re,1)", slc - ctl, N)
raise "s[re]" unless s[re] == "wor"
raise "s[re,1]" unless s[re, 1] == "o"
raise "slice" unless s.slice(re, 1) == "o"
puts "checks ok"
