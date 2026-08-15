# Issue #137 benchmark: the cost of `str[i]` under each candidate.
# Reports the loop-overhead-subtracted ns/iter, best of REPS runs.
# ARGV[0] == "alias" re-aliases String#[] back to the C builtin first, which
# re-arms #103's idx_class slot; that is the control the issue itself ran.

if ARGV[0] == "alias"
  class String
    alias [] __aref
    alias slice __aref
  end
end

N = 1_000_000
REPS = 5

s = "hello world"
a = [1, 2, 3, 4]
h = {0 => 9}
idx = 3
r = nil

def show(label, delta, n)
  ns = delta / n * 1_000_000_000
  puts label + " " + ((ns * 10).round / 10.0).to_s + " ns/iter"
end

best_ctl = nil; best_g1 = nil; best_g0 = nil; best_ary = nil; best_hsh = nil

REPS.times do
  t = Time.now; i = 0; while i < N; i += 1; end; d = Time.now - t
  best_ctl = d if best_ctl.nil? || d < best_ctl

  t = Time.now; i = 0; while i < N; r = s[idx]; i += 1; end; d = Time.now - t
  best_g1 = d if best_g1.nil? || d < best_g1

  t = Time.now; i = 0; while i < N; r = s[0]; i += 1; end; d = Time.now - t
  best_g0 = d if best_g0.nil? || d < best_g0

  t = Time.now; i = 0; while i < N; r = a[idx]; i += 1; end; d = Time.now - t
  best_ary = d if best_ary.nil? || d < best_ary

  t = Time.now; i = 0; while i < N; r = h[0]; i += 1; end; d = Time.now - t
  best_hsh = d if best_hsh.nil? || d < best_hsh
end

show("String s[i]  OP_GETIDX ", best_g1 - best_ctl, N)
show("String s[0]  OP_GETIDX0", best_g0 - best_ctl, N)
show("Array  a[i]  OP_GETIDX ", best_ary - best_ctl, N)
show("Hash   h[0]  OP_GETIDX0", best_hsh - best_ctl, N)

# Correctness spot-checks, so a fast binary that answers wrongly is not
# mistaken for a win.
raise "s[3]"      unless s[3] == "l"
raise "s[0]"      unless s[0] == "h"
raise "s[-1]"     unless s[-1] == "d"
raise "s[1,3]"    unless s[1, 3] == "ell"
raise "s[1..3]"   unless s[1..3] == "ell"
raise "s['lo']"   unless s["lo"] == "lo"
raise "s['bye']"  unless s["bye"].nil?
raise "s[99]"     unless s[99].nil?
raise "slice"     unless s.slice(1, 3) == "ell"
if Object.const_defined?(:Regexp) && ARGV[0] != "alias"
  raise "s[re]"      unless s[/w(o)r/] == "wor"
  raise "s[re,1]"    unless s[/w(o)r/, 1] == "o"
  raise "s[re] miss" unless s[/zzz/].nil?
  raise "slice re"   unless s.slice(/w(o)r/, 1) == "o"
  raise "sym[re]"    unless :"hello world"[/w(o)r/] == "wor"
end
begin
  s[1, 2, 3]
  raise "arity"
rescue ArgumentError
end
puts "checks ok"
