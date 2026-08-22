# Procs and the environments they capture: a block that outlives its frame
# takes a heap-allocated environment, and a Fiber takes a stack of its own.
adders = []
begin
  80.times do |i|
    n = i
    adders << lambda { |v| v + n }
  end
rescue NoMemoryError
end
raise "a captured local was lost" unless adders.empty? || adders[0].call(1) == 1

begin
  20.times do |i|
    f = Fiber.new do
      Fiber.yield i
      i * 2
    end
    f.resume
    f.resume
  end
rescue NoMemoryError
rescue FiberError
end

def deep(n)
  return 0 if n == 0
  1 + deep(n - 1)
end

begin
  deep(60)
rescue NoMemoryError
end

begin
  10.times { |i| [1, 2, 3].each_with_object([]) { |v, acc| acc << v * i } }
rescue NoMemoryError
end
