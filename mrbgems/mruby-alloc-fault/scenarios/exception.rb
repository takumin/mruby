# Raising: an exception object, its message and its backtrace are all
# allocated on the way out, which is the path a refusal itself takes.
def thrower(depth)
  raise ArgumentError, "from depth #{depth}" if depth == 0
  thrower(depth - 1)
end

caught = 0
begin
  100.times do |i|
    begin
      thrower(8)
    rescue ArgumentError => e
      caught += 1 if e.message.start_with?("from depth")
    end
  end
rescue NoMemoryError
end
raise "a rescue ran without its exception" unless caught >= 0

ensured = 0
begin
  200.times do
    begin
      raise "boom"
    rescue RuntimeError
    ensure
      ensured += 1
    end
  end
rescue NoMemoryError
end

begin
  50.times do
    begin
      raise RuntimeError, "with a backtrace"
    rescue => e
      e.backtrace
    end
  end
rescue NoMemoryError
end
