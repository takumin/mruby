# Decomposition: what does a send actually cost, next to the inline opcode?
# `__aref` is the gem's alias of the C builtin `String#[]`, so `s.__aref(i)`
# is an ordinary send straight to the same C function the opcode inlines.
# The gap between it and `s[i]` on a build where the opcode still inlines is
# the send machinery — the part candidate B pays and candidate E would try to
# remove.

N = 1_000_000
REPS = 5

s = "hello world"
idx = 3
r = nil

def show(label, delta, n)
  ns = delta / n * 1_000_000_000
  puts label + " " + ((ns * 10).round / 10.0).to_s + " ns/iter"
end

ctl = nil; op = nil; snd = nil; triv = nil; slc = nil

REPS.times do
  t = Time.now; i = 0; while i < N; i += 1; end; d = Time.now - t
  ctl = d if ctl.nil? || d < ctl

  t = Time.now; i = 0; while i < N; r = s[idx]; i += 1; end; d = Time.now - t
  op = d if op.nil? || d < op

  t = Time.now; i = 0; while i < N; r = s.__aref(idx); i += 1; end; d = Time.now - t
  snd = d if snd.nil? || d < snd

  t = Time.now; i = 0; while i < N; r = s.slice(idx); i += 1; end; d = Time.now - t
  slc = d if slc.nil? || d < slc

  t = Time.now; i = 0; while i < N; r = s.bytesize; i += 1; end; d = Time.now - t
  triv = d if triv.nil? || d < triv
end

show("s[idx]          (opcode)        ", op - ctl, N)
show("s.__aref(idx)   (send -> builtin)", snd - ctl, N)
show("s.slice(idx)    (send)          ", slc - ctl, N)
show("s.bytesize      (send, no args) ", triv - ctl, N)
