##
# TSort implements topological sorting using Tarjan's algorithm for
# strongly connected components.
#
# TSort is designed to be able to be used with any object which can be
# interpreted as a directed graph.
#
# TSort requires two methods to interpret an object as a graph,
# tsort_each_node and tsort_each_child.
#
# * tsort_each_node is used to iterate for all nodes over a graph.
# * tsort_each_child is used to iterate for child nodes of a given node.
#
# The equality of nodes are defined by eql? and hash since
# TSort uses Hash internally.
#
# == A Simple Example
#
# The following example demonstrates how to mix the TSort module into an
# existing class (in this case, Hash). Here, we're treating each key in
# the hash as a node in the graph, and so we simply alias the required
# #tsort_each_node method to Hash's #each_key method. For each key in the
# hash, the associated value is a list of the node's child nodes. This
# choice in turn leads to our implementation of the required
# #tsort_each_child method, which fetches the array of child nodes and then
# iterates over that array using the user-supplied block.
#
#   class Hash
#     include TSort
#     alias tsort_each_node each_key
#     def tsort_each_child(node, &block)
#       fetch(node).each(&block)
#     end
#   end
#
#   {1=>[2, 3], 2=>[3], 3=>[], 4=>[]}.tsort
#   #=> [3, 2, 1, 4]
#
#   {1=>[2], 2=>[3, 4], 3=>[2], 4=>[]}.strongly_connected_components
#   #=> [[4], [2, 3], [1]]
#
# The class methods TSort.tsort, TSort.tsort_each,
# TSort.strongly_connected_components,
# TSort.each_strongly_connected_component and
# TSort.each_strongly_connected_component_from can be used without
# including TSort. They take two callable objects (anything responding
# to +call+) which enumerate all nodes and the children of a node.
#
#   g = {1=>[2, 3], 2=>[3], 3=>[], 4=>[]}
#   each_node = lambda {|&b| g.each_key(&b) }
#   each_child = lambda {|n, &b| g[n].each(&b) }
#   TSort.tsort(each_node, each_child)
#   #=> [3, 2, 1, 4]
module TSort
  # Raised by TSort#tsort and TSort#tsort_each when the graph has a cycle.
  class Cyclic < StandardError
  end

  # Returns a topologically sorted array of nodes.
  # The array is sorted from children to parents, i.e.
  # the first element has no child and the last node has no parent.
  #
  # If there is a cycle, TSort::Cyclic is raised.
  def tsort
    TSort.tsort(lambda {|&b| tsort_each_node(&b) }, lambda {|n, &b| tsort_each_child(n, &b) })
  end

  # Returns a topologically sorted array of nodes.
  # The array is sorted from children to parents, i.e.
  # the first element has no child and the last node has no parent.
  #
  # The graph is represented by _each_node_ and _each_child_.
  # _each_node_ should have +call+ method which yields for each node in the graph.
  # _each_child_ should have +call+ method which takes a node argument and yields for each child node.
  #
  # If there is a cycle, TSort::Cyclic is raised.
  def TSort.tsort(each_node, each_child)
    TSort.tsort_each(each_node, each_child).to_a
  end

  # The iterator version of the #tsort method.
  # <tt><em>obj</em>.tsort_each</tt> is similar to <tt><em>obj</em>.tsort.each</tt>, but
  # modification of _obj_ during the iteration may lead to unexpected result.
  #
  # #tsort_each returns +nil+.
  # If there is a cycle, TSort::Cyclic is raised.
  def tsort_each(&block)
    TSort.tsort_each(lambda {|&b| tsort_each_node(&b) }, lambda {|n, &b| tsort_each_child(n, &b) }, &block)
  end

  # The iterator version of the TSort.tsort method.
  #
  # The graph is represented by _each_node_ and _each_child_.
  # _each_node_ should have +call+ method which yields for each node in the graph.
  # _each_child_ should have +call+ method which takes a node argument and yields for each child node.
  def TSort.tsort_each(each_node, each_child, &block)
    return to_enum(:tsort_each, each_node, each_child) unless block

    TSort.each_strongly_connected_component(each_node, each_child) do |component|
      if component.size == 1
        block.call(component.first)
      else
        raise Cyclic.new("topological sort failed: #{component.inspect}")
      end
    end
  end

  # Returns strongly connected components as an array of arrays of nodes.
  # The array is sorted from children to parents.
  # Each elements of the array represents a strongly connected component.
  def strongly_connected_components
    TSort.strongly_connected_components(lambda {|&b| tsort_each_node(&b) }, lambda {|n, &b| tsort_each_child(n, &b) })
  end

  # Returns strongly connected components as an array of arrays of nodes.
  # The array is sorted from children to parents.
  # Each elements of the array represents a strongly connected component.
  #
  # The graph is represented by _each_node_ and _each_child_.
  # _each_node_ should have +call+ method which yields for each node in the graph.
  # _each_child_ should have +call+ method which takes a node argument and yields for each child node.
  def TSort.strongly_connected_components(each_node, each_child)
    TSort.each_strongly_connected_component(each_node, each_child).to_a
  end

  # The iterator version of the #strongly_connected_components method.
  # <tt><em>obj</em>.each_strongly_connected_component</tt> is similar to
  # <tt><em>obj</em>.strongly_connected_components.each</tt>, but
  # modification of _obj_ during the iteration may lead to unexpected result.
  #
  # #each_strongly_connected_component returns +nil+.
  def each_strongly_connected_component(&block)
    TSort.each_strongly_connected_component(lambda {|&b| tsort_each_node(&b) }, lambda {|n, &b| tsort_each_child(n, &b) }, &block)
  end

  # The iterator version of the TSort.strongly_connected_components method.
  #
  # The graph is represented by _each_node_ and _each_child_.
  # _each_node_ should have +call+ method which yields for each node in the graph.
  # _each_child_ should have +call+ method which takes a node argument and yields for each child node.
  def TSort.each_strongly_connected_component(each_node, each_child, &block)
    return to_enum(:each_strongly_connected_component, each_node, each_child) unless block

    id_map = {}
    stack = []
    each_node.call do |node|
      unless id_map.include?(node)
        TSort.each_strongly_connected_component_from(node, each_child, id_map, stack, &block)
      end
    end
    nil
  end

  # Iterates over strongly connected component in the subgraph reachable from
  # _node_.
  #
  # Return value is unspecified.
  #
  # #each_strongly_connected_component_from doesn't call #tsort_each_node.
  def each_strongly_connected_component_from(node, id_map = {}, stack = [], &block)
    TSort.each_strongly_connected_component_from(node, lambda {|n, &b| tsort_each_child(n, &b) }, id_map, stack, &block)
  end

  # Iterates over strongly connected components in a graph.
  # The graph is represented by _node_ and _each_child_.
  #
  # _node_ is the first node.
  # _each_child_ should have +call+ method which takes a node argument
  # and yields for each child node.
  #
  # Return value is unspecified.
  #
  # TSort.each_strongly_connected_component_from is a class method and
  # it doesn't need a class to represent a graph which includes TSort.
  def TSort.each_strongly_connected_component_from(node, each_child, id_map = {}, stack = [], &block)
    return to_enum(:each_strongly_connected_component_from, node, each_child, id_map, stack) unless block

    minimum_id = node_id = id_map[node] = id_map.size
    stack_length = stack.length
    stack << node

    each_child.call(node) do |child|
      if id_map.include?(child)
        child_id = id_map[child]
        minimum_id = child_id if child_id && child_id < minimum_id
      else
        sub_minimum_id = TSort.each_strongly_connected_component_from(child, each_child, id_map, stack, &block)
        minimum_id = sub_minimum_id if sub_minimum_id < minimum_id
      end
    end

    if node_id == minimum_id
      component = []
      component << stack.pop while stack.length > stack_length
      component.reverse!
      component.each {|n| id_map[n] = nil }
      block.call(component)
    end

    minimum_id
  end

  # Should be implemented by a extended class.
  #
  # #tsort_each_node is used to iterate for all nodes over a graph.
  def tsort_each_node
    raise NotImplementedError.new
  end

  # Should be implemented by a extended class.
  #
  # #tsort_each_child is used to iterate for child nodes of _node_.
  def tsort_each_child(node)
    raise NotImplementedError.new
  end
end
