MRuby.each_target do
  next unless libmruby_enabled? && build_info_enabled?

  info = MRuby::BuildInfo.new(self)
  src = "#{build_dir}/build_info.c"
  manifest = "#{build_dir}/source_manifest.sha256"

  self.libmruby_objs << objfile(src.ext)

  # What the digest is taken over, kept beside the build in `sha256sum` format.
  # A digest that changed says only that something did; diffing two of these
  # says what.
  generated_file manifest, [__FILE__], inputs: [info.source_digest] do |f|
    _pp "GEN", manifest.relative_path
    f.write info.manifest
  end

  # `generated_file` writes only when the text differs, so a rebuild that
  # changed nothing leaves the object and the binary alone. The values the text
  # is made of are not files, so they are passed as `inputs`: a new commit with
  # no source change touches nothing on disk and still has to be recorded.
  generated_file src, [__FILE__, manifest], inputs: [info.line] do |f|
    _pp "GEN", src.relative_path
    f.puts %Q[/*]
    f.puts %Q[ * Provenance of this build: the commit the tree was at, and a]
    f.puts %Q[ * digest over the sources that went into it.]
    f.puts %Q[ *]
    f.puts %Q[ * IMPORTANT:]
    f.puts %Q[ *   This file was generated!]
    f.puts %Q[ *   All manual changes will get lost.]
    f.puts %Q[ */]
    f.puts
    f.puts %Q[#include <mruby/build_info.h>]
    f.puts
    f.puts %Q[/* Led by a marker no other string in the binary starts with, and kept on]
    f.puts %Q[ * one line, so the record reads back with]
    f.puts %Q[ *   strings <binary> | grep mruby-build-info:]
    f.puts %Q[ * on a binary there is no running: one built for another target, or one]
    f.puts %Q[ * left behind by a benchmark that died half way. */]
    f.puts %Q[static const char build_info[] = #{MRuby::BuildInfo.c_string(info.line)};]
    f.puts %Q[static const char build_commit[] = #{MRuby::BuildInfo.c_string(info.commit)};]
    f.puts %Q[static const char build_source_digest[] = #{MRuby::BuildInfo.c_string(info.source_digest)};]
    f.puts
    f.puts %Q[MRB_API const char*]
    f.puts %Q[mrb_build_info(void)]
    f.puts %Q[{]
    f.puts %Q[  return build_info;]
    f.puts %Q[}]
    f.puts
    f.puts %Q[MRB_API const char*]
    f.puts %Q[mrb_build_commit(void)]
    f.puts %Q[{]
    f.puts %Q[  return build_commit;]
    f.puts %Q[}]
    f.puts
    f.puts %Q[MRB_API const char*]
    f.puts %Q[mrb_build_source_digest(void)]
    f.puts %Q[{]
    f.puts %Q[  return build_source_digest;]
    f.puts %Q[}]
    f.puts
    f.puts %Q[MRB_API int]
    f.puts %Q[mrb_build_dirty(void)]
    f.puts %Q[{]
    f.puts %Q[  return #{{"1" => 1, "0" => 0}.fetch(info.dirty, -1)};]
    f.puts %Q[}]
  end
end
