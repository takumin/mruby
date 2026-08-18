#include <mruby.h>
#include <mruby/variable.h>
#ifdef MRB_USE_BUILD_INFO
#include <mruby/build_info.h>
#include <mruby/string.h>
#endif

void
mrb_init_version(mrb_state* mrb)
{
  mrb_value mruby_version = mrb_str_new_lit(mrb, MRUBY_VERSION);

  mrb_define_global_const(mrb, "RUBY_VERSION", mrb_str_new_lit(mrb, MRUBY_RUBY_VERSION));
  mrb_define_global_const(mrb, "RUBY_ENGINE", mrb_str_new_lit(mrb, MRUBY_RUBY_ENGINE));
  mrb_define_global_const(mrb, "RUBY_ENGINE_VERSION", mruby_version);
  mrb_define_global_const(mrb, "MRUBY_VERSION", mruby_version);
  mrb_define_global_const(mrb, "MRUBY_RELEASE_NO", mrb_fixnum_value(MRUBY_RELEASE_NO));
  mrb_define_global_const(mrb, "MRUBY_RELEASE_DATE", mrb_str_new_lit(mrb, MRUBY_RELEASE_DATE));
  mrb_define_global_const(mrb, "MRUBY_DESCRIPTION", mrb_str_new_lit(mrb, MRUBY_DESCRIPTION));
  mrb_define_global_const(mrb, "MRUBY_COPYRIGHT", mrb_str_new_lit(mrb, MRUBY_COPYRIGHT));
#ifdef MRB_USE_BUILD_INFO
  /* Which sources this binary was built from. The strings come from
  ** `build_info.c`, which the build generates; naming them here is also what
  ** keeps that object in the link, and its strings in the binary for
  ** `strings` to find. */
  mrb_define_global_const(mrb, "MRUBY_BUILD_INFO", mrb_str_new_cstr(mrb, mrb_build_info()));
  mrb_define_global_const(mrb, "MRUBY_BUILD_COMMIT", mrb_str_new_cstr(mrb, mrb_build_commit()));
  mrb_define_global_const(mrb, "MRUBY_BUILD_SOURCE_DIGEST", mrb_str_new_cstr(mrb, mrb_build_source_digest()));
#endif
}
