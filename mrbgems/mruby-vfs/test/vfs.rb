##
# VFS Test

# A backend written in Ruby, with only the two methods the protocol asks for
class VFSTestBackend
  def initialize(files)
    @files = files
  end

  def stat(path)
    return :file if @files.key?(path)
    :directory if path == "/"
  end

  def read(path)
    @files[path]
  end
end

def vfs_with_mount(prefix, backend)
  VFS.mount(prefix, backend)
  yield
ensure
  VFS.umount(prefix)
end

assert('VFS::Memory#[]= normalizes the path') do
  mem = VFS::Memory.new
  mem["lib/a.rb"] = "1"
  mem["/lib//b.rb"] = "2"
  mem["./lib/../lib/./c.rb"] = "3"
  assert_equal %w[/lib/a.rb /lib/b.rb /lib/c.rb], mem.paths
  assert_equal "1", mem["lib/a.rb"]
  assert_equal "1", mem["/lib/a.rb"]
  assert_equal "1", mem.read("/lib/a.rb")
  assert_nil mem["/lib/d.rb"]
  assert_raise(TypeError) { mem["/x"] = 1 }
  assert_raise(TypeError) { mem[:x] }
end

assert('VFS::Memory#stat') do
  mem = VFS::Memory.new("lib/a.rb" => "1", "top.rb" => "2")
  assert_equal :file, mem.stat("/lib/a.rb")
  assert_equal :file, mem.stat("top.rb")
  assert_equal :directory, mem.stat("/lib")
  assert_equal :directory, mem.stat("/lib/")
  assert_equal :directory, mem.stat("/")
  assert_nil mem.stat("/li")
  assert_nil mem.stat("/lib/a.rb/x")
  assert_nil mem.stat("/nothing")
end

assert('VFS::Memory#delete') do
  mem = VFS::Memory.new("a.rb" => "1")
  assert_equal "1", mem.delete("/a.rb")
  assert_nil mem.delete("/a.rb")
  assert_equal [], mem.paths
end

assert('VFS.mount keeps the longest prefix first') do
  a = VFS::Memory.new
  b = VFS::Memory.new
  before = VFS.mounts
  begin
    VFS.mount("/a", a)
    VFS.mount("/a/b/", b)
    prefixes = VFS.mounts.map { |m| m[0] }
    assert_equal ["/a/b", "/a"], prefixes[0, 2]
    assert_equal before.size + 2, VFS.mounts.size
    # mounting again replaces
    c = VFS::Memory.new
    assert_same c, VFS.mount("/a", c)
    assert_equal before.size + 2, VFS.mounts.size
    assert_same c, VFS.mounts.find { |m| m[0] == "/a" }[1]
  ensure
    VFS.umount("/a")
    VFS.umount("/a/b")
  end
  assert_equal before, VFS.mounts
end

assert('VFS.umount') do
  mem = VFS::Memory.new
  VFS.mount("/gone", mem)
  assert_same mem, VFS.umount("/gone")
  assert_nil VFS.umount("/gone")
  assert_nil VFS.umount("/never")
end

assert('VFS.mount rejects what is not a mount') do
  assert_raise(TypeError) { VFS.mount(:sym, VFS::Memory.new) }
  assert_raise(ArgumentError) { VFS.mount("rel", VFS::Memory.new) }
  assert_raise(TypeError) { VFS.mount("/x", Object.new) }
end

assert('VFS.mounts is a copy') do
  before = VFS.mounts
  VFS.mounts.clear
  VFS.mounts.each { |m| m.clear }
  assert_equal before, VFS.mounts
end

assert('VFS.stat and VFS.read through the closest mount') do
  outer = VFS::Memory.new("x.rb" => "outer", "sub/y.rb" => "outer sub")
  inner = VFS::Memory.new("y.rb" => "inner")
  vfs_with_mount("/m", outer) do
    vfs_with_mount("/m/sub", inner) do
      assert_equal :file, VFS.stat("/m/x.rb")
      assert_equal "outer", VFS.read("/m/x.rb")
      assert_equal "inner", VFS.read("/m/sub/y.rb")
      assert_equal :directory, VFS.stat("/m/sub")
      assert_equal :directory, VFS.stat("/m")
      assert_nil VFS.stat("/m/sub/nothing")
      assert_nil VFS.stat("/mx")
      assert_true VFS.exist?("/m/x.rb")
      assert_true VFS.file?("/m/x.rb")
      assert_false VFS.file?("/m/sub")
      assert_true VFS.directory?("/m/sub")
      assert_false VFS.directory?("/m/x.rb")
      assert_false VFS.exist?("/m/nothing")
    end
    # the outer mount answers again once the inner one is gone
    assert_equal "outer sub", VFS.read("/m/sub/y.rb")
  end
end

assert('VFS.read raises for a file that is not there') do
  vfs_with_mount("/m", VFS::Memory.new) do
    e = assert_raise(Errno::ENOENT) { VFS.read("/m/nothing.rb") }
    assert_include e.message, "/m/nothing.rb"
  end
end

assert('a backend written in Ruby') do
  backend = VFSTestBackend.new("/hello.rb" => "def vfs_test_hello; :hello; end")
  vfs_with_mount("/rb", backend) do
    assert_equal :file, VFS.stat("/rb/hello.rb")
    assert_equal :directory, VFS.stat("/rb")
    assert_nil VFS.stat("/rb/other")
    assert_equal "def vfs_test_hello; :hello; end", VFS.read("/rb/hello.rb")
  end
end

assert('a backend that answers what the protocol does not allow') do
  bad_stat = VFSTestBackend.new({})
  def bad_stat.stat(path); "file"; end
  vfs_with_mount("/bad", bad_stat) do
    assert_raise(TypeError) { VFS.stat("/bad/x") }
  end
  bad_read = VFSTestBackend.new({})
  def bad_read.stat(path); :file; end
  def bad_read.read(path); 42; end
  vfs_with_mount("/bad", bad_read) do
    assert_raise(TypeError) { VFS.read("/bad/x") }
  end
end

assert('VFS::Backend cannot be made from Ruby') do
  assert_raise(NotImplementedError) { VFS::Backend.new }
end

assert('VFS::Host reads the host') do
  skip "this port has no host filesystem" unless VFS.const_defined?(:Host)
  sandbox = VFSTest.setup
  begin
    host = VFS::Host.new
    assert_kind_of VFS::Backend, host
    assert_equal :directory, host.stat(sandbox)
    assert_equal :file, host.stat("#{sandbox}/plain.txt")
    assert_nil host.stat("#{sandbox}/nothing")
    assert_equal "plain text\n", host.read("#{sandbox}/plain.txt")
    assert_nil host.read("#{sandbox}/nothing")

    # the root mount is one of these
    assert_equal :directory, VFS.stat(sandbox)
    assert_equal :file, VFS.stat("#{sandbox}/plain.txt")
    assert_equal "plain text\n", VFS.read("#{sandbox}/plain.txt")
    assert_nil VFS.stat("#{sandbox}/plain.txt/below")
    assert_raise(Errno::EISDIR) { VFS.read(sandbox) }
    assert_raise(Errno::ENOENT) { VFS.read("#{sandbox}/nothing") }
  ensure
    VFSTest.teardown
  end
end

assert('a mount shadows the host below its prefix') do
  skip "this port has no host filesystem" unless VFS.const_defined?(:Host)
  sandbox = VFSTest.setup
  begin
    # a mount prefix starts at the VFS root; a Windows sandbox is under a
    # drive letter, which no prefix reaches
    skip "the sandbox is not under the VFS root" unless sandbox[0] == "/"
    vfs_with_mount(sandbox, VFS::Memory.new("plain.txt" => "in memory")) do
      assert_equal "in memory", VFS.read("#{sandbox}/plain.txt")
      assert_nil VFS.stat("#{sandbox}/lib")
    end
    assert_equal "plain text\n", VFS.read("#{sandbox}/plain.txt")
  ensure
    VFSTest.teardown
  end
end

assert('a backend written in C') do
  table = VFSTest.table_backend
  assert_kind_of VFS::Backend, table
  assert_equal :file, table.stat("/plain.txt")
  assert_equal :directory, table.stat("/sub")
  assert_nil table.stat("/nothing")
  assert_equal "table text\n", table.read("/plain.txt")
  assert_nil table.read("/nothing")

  assert_same table, VFSTest.c_mount("/rom", table)
  begin
    assert_equal 1, VFSTest.c_stat("/rom/plain.txt")
    assert_equal 2, VFSTest.c_stat("/rom/sub")
    assert_equal 0, VFSTest.c_stat("/rom/nothing")
    assert_equal "table text\n", VFSTest.c_read("/rom/plain.txt")
    assert_nil VFSTest.c_read("/rom/nothing")
    assert_equal "table text\n", VFS.read("/rom/plain.txt")
  ensure
    assert_same table, VFSTest.c_umount("/rom")
  end
  assert_nil VFSTest.c_umount("/rom")
end
