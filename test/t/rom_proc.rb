##
# A method whose proc is built into the binary.
#
# `RomProcBase` and `RomProcChild` are defined from C in
# mrbgems/mruby-test/rom_proc.c: their methods are `const` procs installed in a
# read-only method table, the way a build-time transformation over `mrblib`
# would install them. A proc like that carries no target class of its own, so
# everything below asks for the class it was defined in by another route.

assert('a method from a ROM proc') do
  assert_equal "base", RomProcBase.new.greet
end

assert('super from a method in a ROM proc') do
  assert_equal "child->base", RomProcChild.new.greet
end

assert('super into a method in a ROM proc') do
  c = Class.new(RomProcChild) do
    def greet
      "grand->" + super
    end
  end
  assert_equal "grand->child->base", c.new.greet
end

assert('a lexical constant read from a ROM proc') do
  assert_equal "lexical", RomProcChild.new.lexical
  assert_equal "constant", RomProcChild.new.defined_const
end

assert('a class variable read and written from a ROM proc') do
  before = RomProcChild.new.cvar_bump
  assert_equal before + 1, RomProcChild.new.cvar_bump
  assert_equal "class variable", RomProcChild.new.defined_cvar
end

assert('a method from a ROM proc answers for the class that holds it') do
  assert_true RomProcChild.new.respond_to?(:lexical)
  assert_false RomProcBase.new.respond_to?(:lexical)
  assert_true RomProcBase.new.respond_to?(:greet)
end

assert('a method defined over a ROM proc replaces it') do
  c = Class.new(RomProcChild) do
    def lexical
      "replaced"
    end
  end
  assert_equal "replaced", c.new.lexical
  assert_equal "lexical", RomProcChild.new.lexical
end

assert('the visibility of a method from a ROM proc can be changed') do
  c = Class.new(RomProcChild) do
    private :lexical
  end
  assert_raise(NoMethodError) { c.new.lexical }
  assert_equal "lexical", RomProcChild.new.lexical
end
