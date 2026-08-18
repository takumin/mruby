/**
** @file mruby/build_info.h - provenance of the build this binary came from
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_BUILD_INFO_H
#define MRUBY_BUILD_INFO_H

#include "common.h"

/**
 * Which sources this binary was built from.
 *
 * These are compiled in only when the build asks for them, with
 * `conf.enable_build_info` in the build configuration; that is also what
 * defines `MRB_USE_BUILD_INFO`, so guard any use with it.
 *
 * The same values are in the binary as one greppable line, which is what to
 * reach for when running the binary is not an option (a cross compiled target,
 * a benchmark that died half way, a binary someone kept):
 *
 *   strings ./bin/mruby | grep mruby-build-info:
 */
MRB_BEGIN_DECL

/**
 * The whole record as one line:
 *
 *   mruby-build-info:<format> target=<name> commit=<sha1> dirty=<0|1>
 *   source-digest=sha256:<hex>
 *
 * `<format>` is 1. Later formats keep the leading marker and the `key=value`
 * shape, so a reader that looks for the keys it knows keeps working.
 */
MRB_API const char *mrb_build_info(void);

/** The commit `HEAD` was at, or "unknown" outside a git checkout. */
MRB_API const char *mrb_build_commit(void);

/**
 * A digest over the sources that went into this build, as "sha256:<hex>".
 *
 * Equal digests mean equal sources. They do not mean equal binaries: the
 * compiler and its flags are not part of the digest.
 */
MRB_API const char *mrb_build_source_digest(void);

/**
 * Whether the tree carried changes the commit does not describe: 1 if it did,
 * 0 if it did not, and -1 when there was no git to ask.
 */
MRB_API int mrb_build_dirty(void);

MRB_END_DECL

#endif  /* MRUBY_BUILD_INFO_H */
