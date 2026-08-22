# Objects, classes and instance variables: defining a class allocates a
# method table, an instance variable allocates a shape, and a singleton
# class allocates a class of its own.
class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  attr_reader :x, :y
  def to_s; "(#{@x},#{@y})"; end
end

points = []
begin
  300.times { |i| points << Point.new(i, i * 2) }
rescue NoMemoryError
end
raise "a Point lost its state" unless points.empty? || (points[0].x == 0 && points[0].y == 0)

begin
  10.times do |i|
    k = Class.new do
      define_method(:answer) { i }
    end
    k.new.answer
  end
rescue NoMemoryError
end

begin
  20.times do |i|
    o = Object.new
    def o.greet; "hi"; end
    o.instance_variable_set("@n#{i}", i)
    o.greet
  end
rescue NoMemoryError
end

s = Struct.new(:a, :b, :c)
begin
  30.times { |i| s.new(i, i.to_s, [i]) }
rescue NoMemoryError
end
raise "the struct class was disturbed" unless s.members == [:a, :b, :c]
