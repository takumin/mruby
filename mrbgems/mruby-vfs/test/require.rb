##
# require, require_relative, load Test
#
# The files these tests load leave what they did in global variables, since
# the state a gem's tests run in is the core and this gem alone, with no way
# to take a constant back out.

# Ruby source loads only where the build carries mruby-compiler; a build
# without it (one that runs bytecode alone) skips the tests that need it.
VFS_TEST_HAS_COMPILER = !VFSTest.compile("nil").nil?

def vfs_skip_without_compiler
  skip "this build has no compiler for Ruby source" unless VFS_TEST_HAS_COMPILER
end

# Every test mounts what it needs and takes it down again, and leaves
# $LOAD_PATH and $LOADED_FEATURES as it found them.
def vfs_with_lib(files, prefix = "/vfstest")
  vfs_skip_without_compiler if files.values.any? { |content| content[0, 4] != "RITE" }
  load_path = $LOAD_PATH.dup
  features = $LOADED_FEATURES.dup
  VFS.mount(prefix, VFS::Memory.new(files))
  begin
    $LOAD_PATH.unshift("#{prefix}/lib")
    yield prefix
  ensure
    $LOAD_PATH.replace(load_path)
    $LOADED_FEATURES.replace(features)
    VFS.umount(prefix)
  end
end

assert('$LOAD_PATH and $LOADED_FEATURES') do
  assert_kind_of Array, $LOAD_PATH
  assert_same $LOAD_PATH, $:
  assert_kind_of Array, $LOADED_FEATURES
  assert_same $LOADED_FEATURES, $"
end

assert('LoadError') do
  assert_equal ScriptError, LoadError.superclass
  e = LoadError.new("x")
  assert_nil e.path
end

assert('require finds a feature under $LOAD_PATH') do
  vfs_with_lib("lib/vfs_req_a.rb" => "$vfs_req_a = __FILE__\n") do |prefix|
    $vfs_req_a = nil
    assert_true require("vfs_req_a")
    assert_equal "#{prefix}/lib/vfs_req_a.rb", $vfs_req_a
    assert_include $LOADED_FEATURES, "#{prefix}/lib/vfs_req_a.rb"
    $vfs_req_a = nil
    assert_false require("vfs_req_a")
    assert_false require("vfs_req_a.rb")
    assert_nil $vfs_req_a
  end
end

assert('require takes $LOAD_PATH in order') do
  files = {
    "lib/vfs_req_order.rb" => "$vfs_req_order = :first\n",
    "other/vfs_req_order.rb" => "$vfs_req_order = :second\n",
  }
  vfs_with_lib(files) do |prefix|
    $LOAD_PATH.push("#{prefix}/other")
    assert_true require("vfs_req_order")
    assert_equal :first, $vfs_req_order
    $vfs_req_order = nil
  end
end

assert('require tries .rb before .mrb and takes an extension as given') do
  files = {
    "lib/vfs_req_ext.rb" => "$vfs_req_ext = :rb\n",
    "lib/vfs_req_ext.mrb" => "not bytecode",
    "lib/vfs_req_named.txt.rb" => "$vfs_req_named = :txt\n",
  }
  vfs_with_lib(files) do
    assert_true require("vfs_req_ext")
    assert_equal :rb, $vfs_req_ext
    assert_true require("vfs_req_named.txt")
    assert_equal :txt, $vfs_req_named
    $vfs_req_ext = $vfs_req_named = nil
  end
end

assert('require by an explicit path') do
  files = {
    "lib/vfs_req_abs.rb" => "$vfs_req_abs = 1\n",
    "lib/sub/vfs_req_up.rb" => "$vfs_req_up = 1\n",
  }
  vfs_with_lib(files) do |prefix|
    $LOAD_PATH.clear
    assert_true require("#{prefix}/lib/vfs_req_abs")
    assert_equal 1, $vfs_req_abs
    # a path is cleaned before it is looked up and recorded
    assert_true require("#{prefix}/lib/./sub/../sub//vfs_req_up")
    assert_include $LOADED_FEATURES, "#{prefix}/lib/sub/vfs_req_up.rb"
    assert_false require("#{prefix}/lib/sub/vfs_req_up.rb")
    # a bare name is not looked for as a path
    assert_raise(LoadError) { require("vfs_req_abs") }
    $vfs_req_abs = $vfs_req_up = nil
  end
end

assert('require raises LoadError with the name') do
  vfs_with_lib({}) do
    e = assert_raise(LoadError) { require("vfs_no_such_feature") }
    assert_equal "vfs_no_such_feature", e.path
    assert_include e.message, "vfs_no_such_feature"
    assert_raise(LoadError) { require("") }
    assert_raise(TypeError) { require(:sym) }
  end
end

assert('require rejects a $LOAD_PATH that is not directories') do
  vfs_with_lib({}) do
    $LOAD_PATH.unshift(nil)
    assert_raise(TypeError) { require("anything") }
  end
end

assert('a required file runs at the top level') do
  src = "$vfs_req_self = self\nlocal = 1\ndef vfs_req_top_method; :top; end\n"
  vfs_with_lib("lib/vfs_req_top.rb" => src) do
    local = 2
    require "vfs_req_top"
    assert_equal self, $vfs_req_self
    assert_equal 2, local
    assert_equal :top, vfs_req_top_method
    $vfs_req_self = nil
  end
end

assert('a required file that raises is not recorded') do
  files = {
    "lib/vfs_req_boom.rb" => "$vfs_req_boom_ran = true\nraise ArgumentError, 'boom'\n",
  }
  vfs_with_lib(files) do |prefix|
    e = assert_raise(ArgumentError) { require("vfs_req_boom") }
    assert_equal "boom", e.message
    assert_true $vfs_req_boom_ran
    assert_not_include $LOADED_FEATURES, "#{prefix}/lib/vfs_req_boom.rb"
    # and can be tried again
    assert_raise(ArgumentError) { require("vfs_req_boom") }
    $vfs_req_boom_ran = nil
  end
end

assert('a syntax error names the file and line') do
  vfs_with_lib("lib/vfs_req_syntax.rb" => "x = 1\ndef\n") do |prefix|
    e = assert_raise(SyntaxError) { require("vfs_req_syntax") }
    assert_include e.message, "#{prefix}/lib/vfs_req_syntax.rb:"
    assert_not_include $LOADED_FEATURES, "#{prefix}/lib/vfs_req_syntax.rb"
  end
end

assert('require from a required file') do
  files = {
    "lib/vfs_req_outer.rb" => "require 'vfs_req_inner'\n$vfs_req_outer = $vfs_req_inner + 1\n",
    "lib/vfs_req_inner.rb" => "$vfs_req_inner = 1\n",
  }
  vfs_with_lib(files) do |prefix|
    assert_true require("vfs_req_outer")
    assert_equal 2, $vfs_req_outer
    assert_false require("vfs_req_inner")
    assert_equal ["#{prefix}/lib/vfs_req_inner.rb", "#{prefix}/lib/vfs_req_outer.rb"], $LOADED_FEATURES.last(2)
    $vfs_req_outer = $vfs_req_inner = nil
  end
end

assert('a circular require answers false') do
  files = {
    "lib/vfs_req_cyc_a.rb" => "$vfs_req_cyc_a = require('vfs_req_cyc_b')\n",
    "lib/vfs_req_cyc_b.rb" => "$vfs_req_cyc_b = require('vfs_req_cyc_a')\n",
  }
  vfs_with_lib(files) do
    assert_true require("vfs_req_cyc_a")
    assert_true $vfs_req_cyc_a
    assert_false $vfs_req_cyc_b
    $vfs_req_cyc_a = $vfs_req_cyc_b = nil
  end
end

assert('require from inside a method') do
  vfs_with_lib("lib/vfs_req_nested.rb" => "$vfs_req_nested = 1\n") do
    def vfs_req_helper(name)
      [require(name), :after]
    end
    assert_equal [true, :after], vfs_req_helper("vfs_req_nested")
    assert_equal 1, $vfs_req_nested
    $vfs_req_nested = nil
  end
end

assert('require loads bytecode') do
  bin = VFSTest.compile("$vfs_req_mrb = [:bytecode, __FILE__]\ndef vfs_req_mrb_method; :mrb; end\n")
  skip "no compiler to make bytecode with" if bin.nil?
  vfs_with_lib("lib/vfs_req_mrb.mrb" => bin) do |prefix|
    assert_true require("vfs_req_mrb")
    assert_equal :bytecode, $vfs_req_mrb[0]
    assert_equal :mrb, vfs_req_mrb_method
    assert_include $LOADED_FEATURES, "#{prefix}/lib/vfs_req_mrb.mrb"
    $vfs_req_mrb = nil
  end
end

assert('require of broken bytecode') do
  vfs_with_lib("lib/vfs_req_badmrb.mrb" => "RITE0400 not really") do
    assert_raise(ScriptError) { require("vfs_req_badmrb") }
  end
end

assert('require_relative') do
  files = {
    "lib/vfs_rel_a.rb" => "require_relative 'sub/vfs_rel_b'\n$vfs_rel_a = __dir__\n",
    "lib/sub/vfs_rel_b.rb" => "require_relative '../vfs_rel_c'\n$vfs_rel_b = __dir__\n",
    "lib/vfs_rel_c.rb" => "$vfs_rel_c = __FILE__\n",
  }
  vfs_with_lib(files) do |prefix|
    assert_true require("vfs_rel_a")
    assert_equal "#{prefix}/lib", $vfs_rel_a
    assert_equal "#{prefix}/lib/sub", $vfs_rel_b
    assert_equal "#{prefix}/lib/vfs_rel_c.rb", $vfs_rel_c
    assert_false require("#{prefix}/lib/vfs_rel_c")
    $vfs_rel_a = $vfs_rel_b = $vfs_rel_c = nil
  end
end

assert('require_relative raises LoadError with the path it looked at') do
  vfs_with_lib("lib/vfs_rel_missing.rb" => "require_relative 'nothing/here'\n") do |prefix|
    e = assert_raise(LoadError) { require("vfs_rel_missing") }
    assert_equal "#{prefix}/lib/nothing/here", e.path
  end
end

assert('load runs a file every time') do
  vfs_with_lib("lib/vfs_load_count.rb" => "$vfs_load_count = ($vfs_load_count || 0) + 1\n") do |prefix|
    $vfs_load_count = nil
    assert_true load("#{prefix}/lib/vfs_load_count.rb")
    assert_true load("#{prefix}/lib/vfs_load_count.rb")
    assert_equal 2, $vfs_load_count
    assert_not_include $LOADED_FEATURES, "#{prefix}/lib/vfs_load_count.rb"
    # a bare name is looked for under $LOAD_PATH, without an extension added
    assert_true load("vfs_load_count.rb")
    assert_equal 3, $vfs_load_count
    assert_raise(LoadError) { load("vfs_load_count") }
    assert_raise(LoadError) { load("#{prefix}/lib/nothing.rb") }
    $vfs_load_count = nil
  end
end

assert('load does not wrap') do
  vfs_with_lib("lib/vfs_load_wrap.rb" => "") do |prefix|
    assert_true load("#{prefix}/lib/vfs_load_wrap.rb", false)
    assert_raise(NotImplementedError) { load("#{prefix}/lib/vfs_load_wrap.rb", true) }
  end
end

assert('__dir__') do
  vfs_with_lib("lib/vfs_dir.rb" => "$vfs_dir = [__dir__, [1].map { __dir__ }[0]]\n") do |prefix|
    require "vfs_dir"
    assert_equal ["#{prefix}/lib", "#{prefix}/lib"], $vfs_dir
    $vfs_dir = nil
  end
end

assert('require from the host') do
  skip "this port has no host filesystem" unless VFS.const_defined?(:Host)
  vfs_skip_without_compiler
  sandbox = VFSTest.setup
  load_path = $LOAD_PATH.dup
  features = $LOADED_FEATURES.dup
  begin
    $LOAD_PATH.unshift("#{sandbox}/lib")
    assert_true require("vfs_host_hello")
    assert_equal "hello from the host", vfs_host_hello
    assert_false require("vfs_host_hello")
    assert_true require("vfs_host_relative")
    assert_equal "#{sandbox}/lib", $vfs_host_relative_dir
    assert_true load("#{sandbox}/lib/vfs_host_hello.rb")
    assert_raise(LoadError) { require("vfs_host_nothing") }
    $vfs_host_relative_dir = nil
  ensure
    $LOAD_PATH.replace(load_path)
    $LOADED_FEATURES.replace(features)
    VFSTest.teardown
  end
end

assert('require and load from C') do
  vfs_skip_without_compiler
  load_path = $LOAD_PATH.dup
  features = $LOADED_FEATURES.dup
  VFSTest.c_mount("/rom", VFSTest.table_backend)
  begin
    VFSTest.c_load_path_push("/rom/sub")
    assert_equal "/rom/sub", $LOAD_PATH.last
    assert_true VFSTest.c_require("vfs_table_deep")
    assert_true $vfs_table_deep
    assert_equal ["/rom/vfs_table_hello.rb", "/rom"], $vfs_table_hello
    assert_false VFSTest.c_require("vfs_table_deep")
    assert_false require("vfs_table_deep")
    assert_raise(LoadError) { VFSTest.c_require("vfs_table_nothing") }
    $vfs_table_hello = nil
    assert_equal ["/rom/vfs_table_hello.rb", "/rom"], VFSTest.c_load("/rom/vfs_table_hello.rb")
    assert_raise(LoadError) { VFSTest.c_load("/rom/nothing.rb") }
    $vfs_table_hello = $vfs_table_deep = nil
  ensure
    $LOAD_PATH.replace(load_path)
    $LOADED_FEATURES.replace(features)
    VFSTest.c_umount("/rom")
  end
end
