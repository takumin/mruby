/*
** mruby/digest.h - Digest module
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_DIGEST_H
#define MRUBY_DIGEST_H

#include <mruby.h>

MRB_BEGIN_DECL

/* What one hash algorithm hands to Digest::Base.

   Digest::Base carries every part of the object protocol that says nothing
   about which hash is being computed: allocating the state, copying it for
   #dup, feeding bytes in, handing the finished bytes back as a string. What
   is left is the algorithm itself, and that is the three calls below over a
   block of state whose size only the algorithm knows.

   Digest::Base allocates and frees that block, so none of the three
   allocates and none of them raises: each is handed a block of exactly
   context_size bytes, and they are the only things that read or write it. */
typedef struct mrb_digest_metadata {
  uint32_t context_size;   /* the state the three calls below work on */
  uint32_t block_length;   /* the input block the algorithm compresses */
  uint32_t digest_length;  /* the digest it produces */
  void (*init)(void *ctx);
  void (*update)(void *ctx, const void *data, size_t len);
  /* Writes digest_length bytes and leaves the context finished: what asks
     the same object for a further digest resets it first. */
  void (*finish)(void *ctx, unsigned char *digest);
} mrb_digest_metadata;

/* Defines Digest::<name> as a subclass of Digest::Base computing md.

   The metadata is read for as long as the class lives, and a class lives as
   long as the mrb_state, so it belongs in static storage. Every subclass of
   the class returned computes the same algorithm: what an instance is asked
   for is found by walking up from its class to the nearest class that was
   given metadata. */
MRB_API struct RClass *mrb_digest_define_algorithm(mrb_state *mrb, const char *name, const mrb_digest_metadata *md);

MRB_END_DECL

#endif  /* MRUBY_DIGEST_H */
