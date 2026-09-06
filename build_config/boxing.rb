# word boxing first: the tree's compile_commands.json is written from the
# first build a config declares unless one claims it, and word boxing is
# what mruby picks when nothing asks for another.
boxings = %w[word no nan]
bits = [64, 32]
ints = [64, 32]

boxings.product(bits, ints) do |boxing, bit, int|
  # mrbconf.h refuses MRB_INT64 on 32-bit unless boxing is off: a
  # heap-allocated RInteger would need an alignment the GC slots do not
  # promise.
  next if int == 64 && bit == 32 && boxing != "no"

  MRuby::Build.new("boxing-#{boxing}-m#{bit}-i#{int}") do |conf|
    conf.toolchain :gcc
    conf.gembox 'full-core'
    conf.compilers.each do |c|
      c.defines << "MRB_#{boxing.upcase}_BOXING"
      c.defines << "MRB_INT#{int}"
      c.flags << "-m#{bit}"
    end
    conf.linker.flags << "-m#{bit}"

    conf.enable_debug
    conf.enable_test
    conf.enable_bintest
  end
end
