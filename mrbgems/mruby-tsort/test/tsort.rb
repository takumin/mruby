##
# TSort Test

class TSortHash < Hash
  include TSort
  alias tsort_each_node each_key
  def tsort_each_child(node, &block)
    self[node].each(&block)
  end

  def self.from(hash)
    h = new
    hash.each {|k, v| h[k] = v }
    h
  end
end

class TSortArray < Array
  include TSort
  alias tsort_each_node each_index
  def tsort_each_child(node, &block)
    self[node].each(&block)
  end

  def self.from(array)
    a = new
    array.each {|v| a << v }
    a
  end
end

assert("TSort#tsort with a DAG") do
  h = TSortHash.from({1=>[2, 3], 2=>[3], 3=>[]})
  assert_equal [3, 2, 1], h.tsort
  assert_equal [[3], [2], [1]], h.strongly_connected_components
end

assert("TSort#tsort with a cycle") do
  h = TSortHash.from({1=>[2], 2=>[3, 4], 3=>[2], 4=>[]})
  assert_equal [[4], [2, 3], [1]], h.strongly_connected_components.map {|nodes| nodes.sort }
  assert_raise(TSort::Cyclic) { h.tsort }
end

assert("TSort#tsort_each") do
  h = TSortHash.from({1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[]})
  r = []
  assert_nil h.tsort_each {|n| r << n }
  assert_equal [4, 2, 3, 1], r
  assert_kind_of Enumerator, h.tsort_each
  assert_equal ["4", "2", "3", "1"], h.tsort_each.map {|n| n.to_s }
end

assert("TSort#each_strongly_connected_component") do
  h = TSortHash.from({1=>[2], 2=>[3, 4], 3=>[2], 4=>[]})
  r = []
  assert_nil h.each_strongly_connected_component {|scc| r << scc }
  assert_equal [[4], [2, 3], [1]], r.map {|nodes| nodes.sort }
  assert_kind_of Enumerator, h.each_strongly_connected_component
  assert_equal [[4], [2, 3], [1]], h.each_strongly_connected_component.map {|nodes| nodes.sort }
end

assert("TSort#each_strongly_connected_component_from") do
  h = TSortHash.from({1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[], 5=>[1]})
  r = []
  h.each_strongly_connected_component_from(1) {|scc| r << scc }
  assert_equal [[4], [2], [3], [1]], r
  assert_equal [[4], [2], [3], [1]], h.each_strongly_connected_component_from(1).to_a
end

assert("TSort with an Array subclass") do
  a = TSortArray.from([[1], [0], [0], [2]])
  assert_equal [[0, 1], [2], [3]], a.strongly_connected_components.map {|nodes| nodes.sort }
  a = TSortArray.from([[], [0]])
  assert_equal [[0], [1]], a.strongly_connected_components.map {|nodes| nodes.sort }
end

assert("TSort node equality uses eql? and hash") do
  h = TSortHash.from({"a"=>["b"], "b"=>["c"], "c"=>[]})
  assert_equal ["c", "b", "a"], h.tsort
  h = TSortHash.from({:a=>[:b, :c], :b=>[:c], :c=>[]})
  assert_equal [:c, :b, :a], h.tsort
end

assert("TSort.tsort") do
  g = {1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[]}
  each_node = lambda {|&b| g.each_key(&b) }
  each_child = lambda {|n, &b| g[n].each(&b) }
  assert_equal [4, 2, 3, 1], TSort.tsort(each_node, each_child)

  g = {1=>[2], 2=>[3, 4], 3=>[2], 4=>[]}
  assert_raise(TSort::Cyclic) { TSort.tsort(each_node, each_child) }
end

assert("TSort.tsort_each") do
  g = {1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[]}
  each_node = lambda {|&b| g.each_key(&b) }
  each_child = lambda {|n, &b| g[n].each(&b) }
  r = []
  TSort.tsort_each(each_node, each_child) {|n| r << n }
  assert_equal [4, 2, 3, 1], r
  assert_equal ["4", "2", "3", "1"], TSort.tsort_each(each_node, each_child).map {|n| n.to_s }
end

assert("TSort.strongly_connected_components") do
  g = {1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[]}
  each_node = lambda {|&b| g.each_key(&b) }
  each_child = lambda {|n, &b| g[n].each(&b) }
  assert_equal [[4], [2], [3], [1]], TSort.strongly_connected_components(each_node, each_child)

  g = {1=>[2], 2=>[3, 4], 3=>[2], 4=>[]}
  assert_equal [[4], [2, 3], [1]], TSort.strongly_connected_components(each_node, each_child)
end

assert("TSort.each_strongly_connected_component") do
  g = {1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[]}
  each_node = lambda {|&b| g.each_key(&b) }
  each_child = lambda {|n, &b| g[n].each(&b) }
  r = []
  assert_nil TSort.each_strongly_connected_component(each_node, each_child) {|scc| r << scc }
  assert_equal [[4], [2], [3], [1]], r

  g = {1=>[2], 2=>[3, 4], 3=>[2], 4=>[]}
  r = []
  TSort.each_strongly_connected_component(each_node, each_child) {|scc| r << scc }
  assert_equal [[4], [2, 3], [1]], r
  r = TSort.each_strongly_connected_component(each_node, each_child).map {|scc| scc.map {|n| n.to_s } }
  assert_equal [["4"], ["2", "3"], ["1"]], r
end

assert("TSort.each_strongly_connected_component_from") do
  g = {1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[]}
  each_child = lambda {|n, &b| g[n].each(&b) }
  r = []
  TSort.each_strongly_connected_component_from(1, each_child) {|scc| r << scc }
  assert_equal [[4], [2], [3], [1]], r

  g = {1=>[2], 2=>[3, 4], 3=>[2], 4=>[]}
  r = []
  TSort.each_strongly_connected_component_from(1, each_child) {|scc| r << scc }
  assert_equal [[4], [2, 3], [1]], r
  r = TSort.each_strongly_connected_component_from(1, each_child).map {|scc| scc.map {|n| n.to_s } }
  assert_equal [["4"], ["2", "3"], ["1"]], r
end

assert("TSort.each_strongly_connected_component_from shares id_map and stack") do
  g = {1=>[2], 2=>[], 3=>[1]}
  each_child = lambda {|n, &b| g[n].each(&b) }
  id_map = {}
  stack = []
  r = []
  TSort.each_strongly_connected_component_from(1, each_child, id_map, stack) {|scc| r << scc }
  TSort.each_strongly_connected_component_from(3, each_child, id_map, stack) {|scc| r << scc }
  assert_equal [[2], [1], [3]], r
  assert_true stack.empty?
end

assert("TSort#tsort_each_node and #tsort_each_child are abstract") do
  o = Object.new
  o.extend(TSort)
  assert_raise(NotImplementedError) { o.tsort }
  assert_raise(NotImplementedError) { o.tsort_each_child(1) }
end

assert("TSort::Cyclic") do
  assert_true TSort::Cyclic.ancestors.include?(StandardError)
  h = TSortHash.from({1=>[2], 2=>[1]})
  e = assert_raise(TSort::Cyclic) { h.tsort }
  assert_include e.message, "topological sort failed"
end

assert("TSort handles deep graphs") do
  n = 2000
  h = {}
  n.times {|i| h[i] = [i + 1] }
  h[n] = []
  r = TSortHash.from(h).tsort
  assert_equal n + 1, r.size
  assert_equal n, r.first
  assert_equal 0, r.last

  c = {}
  n.times {|i| c[i] = [(i + 1) % n] }
  scc = TSortHash.from(c).strongly_connected_components
  assert_equal 1, scc.size
  assert_equal n, scc[0].size
  assert_raise(TSort::Cyclic) { TSortHash.from(c).tsort }
end

assert("TSort with nil and false nodes") do
  h = TSortHash.from({nil=>[false], false=>[true], true=>[]})
  assert_equal [true, false, nil], h.tsort
  assert_equal [[true], [false], [nil]], h.strongly_connected_components
end

class TSortCountingHash < TSortHash
  attr_reader :calls
  def tsort_each_child(node, &block)
    (@calls ||= []) << node
    super
  end
end

assert("TSort#tsort_each_child is called once per node") do
  h = TSortCountingHash.from({1=>[2, 3], 2=>[3], 3=>[1, 2]})
  h.strongly_connected_components
  assert_equal [1, 2, 3], h.calls
end

assert("TSort adds only its own API to including classes") do
  h = TSortHash.new
  [:tsort, :tsort_each, :strongly_connected_components, :each_strongly_connected_component,
   :each_strongly_connected_component_from, :tsort_each_node, :tsort_each_child].each do |m|
    assert_true h.respond_to?(m), m.to_s
  end
  assert_false h.respond_to?(:__tsort_each_node_proc, true)
  assert_false h.respond_to?(:__tsort_each_child_proc, true)
end
