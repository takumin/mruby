/*
** The build writes the revision it read out of the source tree to
** `mruby/revision.h`, which this file is alone in including: the revision
** reaches `MRUBY_DESCRIPTION` and the constant below without any other object
** being compiled with it, so a commit recompiles this one and no more.
**
** Not every build writes the header, so ask for it only where the compiler can
** tell us whether it is there; `mruby/version.h` answers with an empty
** revision where it is not. The include has to come first, ahead of the
** `mruby.h` that pulls `mruby/version.h` in.
*/
#if defined(__has_include)
# if __has_include(<mruby/revision.h>)
#  include <mruby/revision.h>
# endif
#endif

#include <mruby.h>
#include <mruby/variable.h>

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
  mrb_define_global_const(mrb, "MRUBY_REVISION", mrb_str_new_lit(mrb, MRUBY_REVISION));
  mrb_define_global_const(mrb, "MRUBY_DESCRIPTION", mrb_str_new_lit(mrb, MRUBY_DESCRIPTION));
  mrb_define_global_const(mrb, "MRUBY_COPYRIGHT", mrb_str_new_lit(mrb, MRUBY_COPYRIGHT));
}
