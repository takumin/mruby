# Where `$~` lives. CRuby's match state belongs to a method scope: a C
# frame has no slot of its own and writes through to the Ruby frame below
# it, and a block shares its defining method's slot. Every expectation here
# is CRuby's measured answer.

def backref_scope_match
  "zz" =~ /zz/
end

def backref_scope_no_match
  "zz" =~ /q/
end

def backref_scope_reads
  $~
end

def backref_scope_own
  "own" =~ /(ow)n/
  [$~ && $~[0], $&, $1]
end

def backref_scope_sub
  "hello".sub(/l/, "L")
  $~ && $~[0]
end

def backref_scope_block_share
  "before" =~ /before/
  [1].each { "inner" =~ /inner/ }
  $~ && $~[0]
end

def backref_scope_mk
  -> { "leak" =~ /leak/ }
end

def backref_scope_run(pr)
  pr.call
  $~
end

def backref_scope_escape
  "esc" =~ /esc/
  -> { $~ }
end

def backref_scope_pair
  wr = -> { "pair" =~ /(pa)ir/ }
  rd = -> { $~ && $~[0] }
  [wr, rd]
end

def backref_scope_nested_escape
  "nest" =~ /nest/
  [1].map { -> { $~ } }.first
end

def backref_scope_deep_pair
  wr = rd = nil
  [1].each do
    wr = -> { "deep" =~ /(de)ep/ }
    rd = -> { $~ && $~[0] }
  end
  [wr, rd]
end

def backref_scope_three_deep
  "m3" =~ /m3/
  [[1]].map { |a| a.map { -> { $~ } } }.first.first
end

def backref_scope_block_writes
  [1].each { "wb" =~ /wb/ }
  -> { $~ && $~[0] }
end

def backref_scope_last_match
  "helper" =~ /helper/
  Regexp.last_match
end

def backref_scope_dead_fiber
  "df" =~ /df/
  f = Fiber.new { -> { $~ && $~[0] } }
  f.resume
end

def backref_scope_dead_fiber_pair
  f = Fiber.new { [-> { "dw" =~ /dw/ }, -> { $~ && $~[0] }] }
  f.resume
end

def backref_scope_fiber_escape
  "lf" =~ /lf/
  f = Fiber.new { Fiber.yield(-> { $~ && $~[0] }); :done }
  [f, f.resume]
end

def backref_scope_fiber_live
  "lm" =~ /lm/
  f = Fiber.new { Fiber.yield(-> { $~ && $~[0] }); :done }
  pr = f.resume
  r = yield pr
  f.resume
  r
end

def backref_scope_fiber_carried(pr)
  f = Fiber.new { Fiber.yield; pr.call }
  f.resume
  f
end

def backref_scope_fiber_suspended_write
  wr = -> { "xf" =~ /xf/ }
  Fiber.yield(wr)
  $~ && $~[0]
end

def backref_scope_fiber_suspended_read
  "rf" =~ /rf/
  Fiber.yield(-> { $~ && $~[0] })
end

assert("$~ - a match inside a method is invisible to its caller") do
  "outer" =~ /outer/
  backref_scope_match
  assert_equal "outer", $~ && $~[0]
end

assert("$~ - a failed match inside a method does not clear the caller") do
  "outer" =~ /outer/
  backref_scope_no_match
  assert_equal "outer", $~ && $~[0]
end

assert("$~ - a method reads its own scope, not its caller's") do
  "outer" =~ /outer/
  assert_nil backref_scope_reads
end

assert("$~ - a method sees its own match, and the derived names follow") do
  assert_equal ["own", "own", "ow"], backref_scope_own
end

assert("$~ - a failed match in the same scope publishes nil") do
  "keep" =~ /keep/
  "keep" =~ /q/
  assert_nil $~
end

assert("$~ - match? neither publishes nor clears") do
  "keep" =~ /keep/
  assert_true(/ke/.match?("keep"))
  assert_false "keep".match?(/q/)
  assert_equal "keep", $~ && $~[0]
end

assert("$~ - String#sub publishes into its caller") do
  # under this frame sit `String#sub` (mrblib) and the C search, both
  # transparent, so the publish lands here, as `rb_str_sub_bang()`'s does
  "hello".sub(/l/, "L")
  assert_equal "l", $~ && $~[0]
  assert_equal "l", backref_scope_sub
end

assert("$~ - a block shares its defining method's slot") do
  assert_equal "inner", backref_scope_block_share
end

assert("$~ - a method called from a block leaves the block's scope alone") do
  "outer" =~ /outer/
  [1].each do
    "blk" =~ /blk/
    backref_scope_match
    assert_equal "blk", $~ && $~[0]
  end
  assert_equal "blk", $~ && $~[0]
end

assert("$~ - a gsub block reads the match being replaced") do
  seen = []
  "ab".gsub(/[ab]/) { seen << ($~ && $~[0]); "x" }
  assert_equal ["a", "b"], seen
  assert_equal "b", $~ && $~[0]
end

assert("$~ - a proc called elsewhere writes its defining scope, not its caller") do
  # `backref_scope_run` calls a proc whose defining frame has returned: the
  # write lands nowhere `run` can see, so `run` answers its own untouched
  # slot, which is CRuby's nil too.
  "outer" =~ /outer/
  assert_nil backref_scope_run(backref_scope_mk)
  assert_equal "outer", $~ && $~[0]
end

assert("$~ - a proc outliving its scope still reads its match") do
  # The frame's slot moves into the env when the frame returns, the way
  # CRuby's svar lives on the lep and survives with it.
  pr = backref_scope_escape
  assert_equal "esc", pr.call && pr.call[0]
end

assert("$~ - two procs from one dead scope share its slot") do
  wr, rd = backref_scope_pair
  assert_nil rd.call
  wr.call
  assert_equal "pair", rd.call
end

assert("$~ - a proc born in a block of a dead scope reads the method's match") do
  # The lexical chain stands in for CRuby's lep walk: the proc's env is the
  # block frame's, and the walk crosses it into the method's, where the
  # slot moved when the frame returned.
  pr = backref_scope_nested_escape
  assert_equal "nest", pr.call && pr.call[0]
  pr3 = backref_scope_three_deep
  assert_equal "m3", pr3.call && pr3.call[0]
end

assert("$~ - block-born procs of one dead scope share the method's slot") do
  wr, rd = backref_scope_deep_pair
  assert_nil rd.call
  wr.call
  assert_equal "deep", rd.call
end

assert("$~ - a match made in a block survives the method's escape") do
  # the block published into the method's frame (its defining scope), and
  # the frame's slot moved into the env on return
  assert_equal "wb", backref_scope_block_writes.call
end

assert("$~ - accepts a MatchData and nil, refuses the rest") do
  m = /a/.match("a")
  $~ = m
  assert_equal "a", $~[0]
  $~ = nil
  assert_nil $~
  assert_raise(TypeError) { $~ = 1 }
  assert_raise(TypeError) { $~ = "str" }
  assert_raise(TypeError) { $~ = Object.new }
end

assert("$~ - stays a global name") do
  assert_equal "global-variable", defined?($~)
end

assert("Regexp.last_match reads the calling scope") do
  "top" =~ /(t)op/
  assert_equal "top", Regexp.last_match[0]
  assert_equal "top", Regexp.last_match(0)
  assert_equal "t", Regexp.last_match(1)
  helper_md = backref_scope_last_match
  assert_equal "helper", helper_md && helper_md[0]
  assert_equal "top", $~ && $~[0]
end

assert("$~ - a match on a Symbol publishes into the caller") do
  :symbol_subject =~ /sym(bol)/
  assert_equal "symbol", $~ && $~[0]
  assert_equal "bol", $1
end

assert("$~ - fibers do not share the slot") do
  skip unless Object.const_defined?(:Fiber)
  inner = nil
  f = Fiber.new { "fib" =~ /fib/; inner = ($~ && $~[0]) }
  "main" =~ /main/
  f.resume
  assert_equal "fib", inner
  assert_equal "main", $~ && $~[0]
end

assert("$~ - a proc born in a dead fiber reads its lexical method scope") do
  skip unless Object.const_defined?(:Fiber)
  # the chain crosses the fiber's root block into the defining method,
  # whose slot escaped with its env; the fiber's death does not reroute it
  pr = backref_scope_dead_fiber
  assert_equal "df", pr.call
end

assert("$~ - procs born in a dead fiber share the method's escaped slot") do
  skip unless Object.const_defined?(:Fiber)
  wr, rd = backref_scope_dead_fiber_pair
  assert_nil rd.call
  wr.call
  assert_equal "dw", rd.call
end

assert("$~ - a fiber's own matches are unreachable once it dies") do
  skip unless Object.const_defined?(:Fiber)
  # the fiber-local slot is its root frame's, and nothing chains to it,
  # so it dies with the fiber the way CRuby's per-context root svar does
  pr = nil
  f = Fiber.new { "fl" =~ /fl/; pr = -> { $~ && $~[0] } }
  "base" =~ /base/
  f.resume
  assert_equal "base", pr.call
end

assert("$~ - a proc escaping a live fiber reads its dead method scope") do
  skip unless Object.const_defined?(:Fiber)
  f, pr = backref_scope_fiber_escape
  assert_equal "lf", pr.call
  f.resume
end

assert("$~ - a fiber-born proc reads its live method scope from outside") do
  skip unless Object.const_defined?(:Fiber)
  assert_equal "lm", backref_scope_fiber_live { |pr| pr.call }
end

assert("$~ - a carried proc reads its defining scope from inside a fiber") do
  skip unless Object.const_defined?(:Fiber)
  "car" =~ /car/
  rd = -> { $~ && $~[0] }
  f = backref_scope_fiber_carried(rd)
  assert_equal "car", f.resume
end

assert("$~ - a carried proc writes its defining scope from inside a fiber") do
  skip unless Object.const_defined?(:Fiber)
  wr = -> { "cw" =~ /cw/ }
  "pre" =~ /pre/
  f = backref_scope_fiber_carried(wr)
  f.resume
  assert_equal "cw", $~ && $~[0]
end

assert("$~ - a proc reaches a scope suspended on another fiber") do
  skip unless Object.const_defined?(:Fiber)
  # the write lands on a frame owned by neither the running nor the root
  # context, the arm that marks through the owning fiber
  f1 = Fiber.new { backref_scope_fiber_suspended_write }
  wr = f1.resume
  f2 = Fiber.new { wr.call }
  f2.resume
  assert_equal "xf", f1.resume
  f3 = Fiber.new { backref_scope_fiber_suspended_read }
  rd = f3.resume
  f4 = Fiber.new { rd.call }
  assert_equal "rf", f4.resume
  f3.resume
end

def backref_scope_nested_load
  "before" =~ /before/
  inner = __backref_nested_load("'ab' =~ /a(b)/; [$~ && $~[0], $1]")
  [inner, $~ && $~[0], $1]
end

assert("$~ - a nested load is transparent to the scope below") do
  # A C function calling mrb_load_string() mid-execution (the helper in
  # test/backref_scope.c) runs the loaded top proc on a frame with no
  # scope of its own: reads and writes pass through to the Ruby scope
  # below, the way rb_eval_string()'s do.
  assert_equal [["ab", "b"], "ab", "b"], backref_scope_nested_load
end

assert("$~ - consecutive nested loads share the scope below") do
  "keep" =~ /keep/
  assert_equal "keep", __backref_nested_load("$~ && $~[0]")
  __backref_nested_load("'x1' =~ /x(1)/")
  assert_equal "x1", __backref_nested_load("$~ && $~[0]")
  assert_equal ["x1", "1"], [$~ && $~[0], $1]
end
