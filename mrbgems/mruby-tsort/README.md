# mruby-tsort

`mruby-tsort` provides the `TSort` module for mruby, a port of the `tsort`
library bundled with CRuby. It implements topological sorting and the
detection of strongly connected components using Tarjan's algorithm, and can be
mixed into any object that can be interpreted as a directed graph.

## Installation

`mruby-tsort` is part of the `stdlib` gembox, so it is available in the default
build. To add it explicitly to your `build_config.rb`:

```ruby
conf.gem :core => "mruby-tsort"
```

## Usage

To use `TSort` as a mixin, include the module and define two methods:

- `tsort_each_node`: yields every node of the graph.
- `tsort_each_child(node)`: yields every child of `node`.

Node equality is defined by `eql?` and `hash`, because `TSort` uses a `Hash`
internally.

```ruby
class Hash
  include TSort
  alias tsort_each_node each_key
  def tsort_each_child(node, &block)
    fetch(node).each(&block)
  end
end

{1=>[2, 3], 2=>[3], 3=>[], 4=>[]}.tsort
#=> [3, 2, 1, 4]

{1=>[2], 2=>[3, 4], 3=>[2], 4=>[]}.strongly_connected_components
#=> [[4], [2, 3], [1]]
```

`tsort` raises `TSort::Cyclic` when the graph contains a cycle.
`strongly_connected_components` returns the cycles as components instead.

### Instance methods

- `tsort` returns the nodes sorted from children to parents.
- `tsort_each { |node| ... }` is the iterator version of `tsort`.
- `strongly_connected_components` returns an array of components, each an array of nodes.
- `each_strongly_connected_component { |nodes| ... }` is the iterator version.
- `each_strongly_connected_component_from(node) { |nodes| ... }` iterates only over the subgraph reachable from `node`.

The iterator methods return an `Enumerator` when called without a block.

### Module functions

The same operations are available as module functions that do not require a
class including `TSort`. They take callable objects (anything that responds to
`call`) which enumerate the nodes and the children of a node:

```ruby
g = {1=>[2, 3], 2=>[4], 3=>[2, 4], 4=>[]}
each_node = lambda {|&b| g.each_key(&b) }
each_child = lambda {|n, &b| g[n].each(&b) }

TSort.tsort(each_node, each_child)
#=> [4, 2, 3, 1]
TSort.strongly_connected_components(each_node, each_child)
#=> [[4], [2], [3], [1]]
TSort.each_strongly_connected_component_from(1, each_child) {|scc| p scc }
```

- `TSort.tsort(each_node, each_child)`
- `TSort.tsort_each(each_node, each_child) { |node| ... }`
- `TSort.strongly_connected_components(each_node, each_child)`
- `TSort.each_strongly_connected_component(each_node, each_child) { |nodes| ... }`
- `TSort.each_strongly_connected_component_from(node, each_child, id_map = {}, stack = []) { |nodes| ... }`

## Differences from CRuby

The API is the same as CRuby's `tsort`. The only difference is that the
callables passed to the mixin-less module functions are built with lambdas
instead of `Method` objects, so `mruby-method` is not required. `Method`
objects still work when `mruby-method` is available, because they respond to
`call`.

## License

MIT
