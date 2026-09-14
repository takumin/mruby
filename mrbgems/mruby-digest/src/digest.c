/*
** digest.c - Digest module
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/digest.h>
#include <mruby/internal.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#include <string.h>

/* Where a class keeps the algorithm its instances compute. It is read by
   walking up from an instance's class, so a subclass written in Ruby
   computes what its superclass computes without being given anything. The
   name is one no Ruby program can write, since the walk has to tell a class
   that was given an algorithm from one that merely carries an instance
   variable of its own. */
#define DIGEST_METADATA_IV MRB_SYM(mruby_Digest_metadata)

typedef struct digest_object {
  const mrb_digest_metadata *md;
  /* The algorithm's state, in an allocation of its own. Carrying it here
     rather than behind this pointer would put a flexible array member in a
     struct that also has to compile as C++, and would leave the alignment of
     the state to be arranged by hand. */
  void *ctx;
} digest_object;

static void
digest_free(mrb_state *mrb, void *ptr)
{
  digest_object *d = (digest_object*)ptr;

  if (d) {
    mrb_free(mrb, d->ctx);
    mrb_free(mrb, d);
  }
}

static const mrb_data_type digest_type = { "Digest", digest_free };

/* The algorithm the given class computes, or NULL where no class in the
   chain was given one: that is Digest::Base itself, and any subclass of it
   that names no algorithm. */
static const mrb_digest_metadata*
digest_metadata(mrb_state *mrb, struct RClass *c)
{
  for (; c; c = c->super) {
    /* An included module reaches the chain as an iclass, which shares its
       instance variables with the module it stands for; skipping it keeps
       the walk to the classes that were actually given an algorithm. */
    if (c->tt == MRB_TT_ICLASS) continue;

    mrb_value v = mrb_iv_get(mrb, mrb_obj_value(c), DIGEST_METADATA_IV);
    if (mrb_cptr_p(v)) return (const mrb_digest_metadata*)mrb_cptr(v);
  }
  return NULL;
}

static digest_object*
digest_ptr(mrb_state *mrb, mrb_value self)
{
  digest_object *d = (digest_object*)mrb_data_get_ptr(mrb, self, &digest_type);

  /* A digest object with no state is one whose #initialize was never
     reached, which allocate alone can arrive at. */
  if (d == NULL || d->ctx == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "uninitialized digest object");
  }
  return d;
}

/* The state of a digest object is what its methods write, so a frozen one
   refuses every method that would move it along. What #digest does to a
   frozen object it does to an unfrozen copy of it, so that still works. */
static digest_object*
digest_ptr_for_write(mrb_state *mrb, mrb_value self)
{
  mrb_check_frozen_value(mrb, self);
  return digest_ptr(mrb, self);
}

/* Gives self a fresh context for the algorithm md, dropping whatever it
   held. The half-built object is attached before the second allocation, so
   a failure there leaves it to be freed rather than lost. */
static digest_object*
digest_alloc(mrb_state *mrb, mrb_value self, const mrb_digest_metadata *md)
{
  mrb_check_frozen_value(mrb, self);

  /* #initialize and #initialize_copy are ordinary methods and can be
     reached again on an object that already holds a context. */
  if (DATA_TYPE(self) == &digest_type) {
    digest_free(mrb, DATA_PTR(self));
    mrb_data_init(self, NULL, &digest_type);
  }

  digest_object *d = (digest_object*)mrb_malloc(mrb, sizeof(digest_object));

  d->md = md;
  d->ctx = NULL;
  mrb_data_init(self, d, &digest_type);
  d->ctx = mrb_malloc(mrb, md->context_size);
  return d;
}

/*
 * call-seq:
 *   Digest::SHA256.new -> digest_obj
 *
 * Returns a digest object with nothing fed into it yet.
 */
static mrb_value
digest_base_init(mrb_state *mrb, mrb_value self)
{
  const mrb_digest_metadata *md = digest_metadata(mrb, mrb_obj_class(mrb, self));

  if (md == NULL) {
    mrb_raisef(mrb, E_NOTIMP_ERROR, "%C names no digest algorithm", mrb_obj_class(mrb, self));
  }

  digest_object *d = digest_alloc(mrb, self, md);
  md->init(d->ctx);
  return self;
}

/*
 * Copies what the source has been fed so far, so that #dup and #clone hand
 * back an object that continues from the same point rather than one sharing
 * the original's state.
 */
static mrb_value
digest_base_init_copy(mrb_state *mrb, mrb_value self)
{
  mrb_value src = mrb_get_arg1(mrb);

  if (mrb_obj_equal(mrb, self, src)) return self;

  digest_object *s = digest_ptr(mrb, src);
  digest_object *d = digest_alloc(mrb, self, s->md);
  memcpy(d->ctx, s->ctx, s->md->context_size);
  return self;
}

/*
 * call-seq:
 *   digest_obj.update(string) -> digest_obj
 *   digest_obj << string      -> digest_obj
 *
 * Feeds the string's bytes to the digest and returns the receiver, so that
 * the calls chain.
 *
 *   Digest::SHA256.new << "ab" << "c"
 */
static mrb_value
digest_base_update(mrb_state *mrb, mrb_value self)
{
  mrb_value str;
  mrb_get_args(mrb, "S", &str);

  digest_object *d = digest_ptr_for_write(mrb, self);
  d->md->update(d->ctx, RSTRING_PTR(str), (size_t)RSTRING_LEN(str));
  return self;
}

/*
 * call-seq:
 *   digest_obj.reset -> digest_obj
 *
 * Puts the digest back to where a fresh one stands.
 */
static mrb_value
digest_base_reset(mrb_state *mrb, mrb_value self)
{
  digest_object *d = digest_ptr_for_write(mrb, self);

  d->md->init(d->ctx);
  return self;
}

/*
 * Finishes the digest and returns its bytes. The context is left finished,
 * which is why the callers in Digest::Instance either work on a copy or
 * reset afterwards.
 */
static mrb_value
digest_base_finish(mrb_state *mrb, mrb_value self)
{
  digest_object *d = digest_ptr_for_write(mrb, self);
  mrb_value str = mrb_str_new(mrb, NULL, d->md->digest_length);

  /* A digest is bytes rather than text, so the string it comes back in says
     so. Without this the bytes would be read as the build's default
     encoding, where all but one digest in eight is broken UTF-8. */
  RSTR_ENCODING_SET(mrb_str_ptr(str), MRB_STR_ENCODING_BINARY);
  d->md->finish(d->ctx, (unsigned char*)RSTRING_PTR(str));
  return str;
}

/*
 * call-seq:
 *   digest_obj.digest_length -> integer
 *
 * The length in bytes of the digests this object produces.
 */
static mrb_value
digest_base_digest_length(mrb_state *mrb, mrb_value self)
{
  digest_object *d = digest_ptr(mrb, self);

  return mrb_int_value(mrb, (mrb_int)d->md->digest_length);
}

/*
 * call-seq:
 *   digest_obj.block_length -> integer
 *
 * The length in bytes of the input block the algorithm works on.
 */
static mrb_value
digest_base_block_length(mrb_state *mrb, mrb_value self)
{
  digest_object *d = digest_ptr(mrb, self);

  return mrb_int_value(mrb, (mrb_int)d->md->block_length);
}

static const char digest_hexmap[] = "0123456789abcdef";

static mrb_value
digest_hexencode(mrb_state *mrb, mrb_value src)
{
  mrb_int len = RSTRING_LEN(src);

  if (len > MRB_INT_MAX / 2) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "string too long to hex-encode");
  }

  mrb_value dst = mrb_str_new(mrb, NULL, len * 2);
  const unsigned char *p = (const unsigned char*)RSTRING_PTR(src);
  char *q = RSTRING_PTR(dst);

  for (mrb_int i = 0; i < len; i++) {
    *q++ = digest_hexmap[p[i] >> 4];
    *q++ = digest_hexmap[p[i] & 0x0f];
  }
  /* Every byte written above is ASCII, so the walk that would find that out
     is answered here instead. */
  RSTR_CODERANGE_SET(mrb_str_ptr(dst), MRB_STR_CODERANGE_7BIT);
  return dst;
}

/*
 * call-seq:
 *   Digest.hexencode(string) -> string
 *
 * Returns the string's bytes spelled as lowercase hexadecimal, which is
 * what #hexdigest hands back a digest in.
 *
 *   Digest.hexencode("\x01\xff")  #=> "01ff"
 */
static mrb_value
digest_s_hexencode(mrb_state *mrb, mrb_value self)
{
  mrb_value str;
  mrb_get_args(mrb, "S", &str);

  return digest_hexencode(mrb, str);
}

static const mrb_mt_entry digest_base_rom_entries[] = {
  MRB_MT_ENTRY(digest_base_init,          MRB_SYM(initialize),      MRB_ARGS_NONE()),
  MRB_MT_ENTRY(digest_base_init_copy,     MRB_SYM(initialize_copy), MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(digest_base_update,        MRB_SYM(update),          MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(digest_base_update,        MRB_OPSYM(lshift),        MRB_ARGS_REQ(1)),
  MRB_MT_ENTRY(digest_base_reset,         MRB_SYM(reset),           MRB_ARGS_NONE()),
  /* Private for the reason CRuby's is: what a digest object is asked for is
     a digest, and #finish leaves the object somewhere only its own methods
     know how to carry on from. */
  MRB_MT_ENTRY(digest_base_finish,        MRB_SYM(finish),          MRB_ARGS_NONE() | MRB_MT_PRIVATE),
  MRB_MT_ENTRY(digest_base_digest_length, MRB_SYM(digest_length),   MRB_ARGS_NONE()),
  MRB_MT_ENTRY(digest_base_block_length,  MRB_SYM(block_length),    MRB_ARGS_NONE()),
};

MRB_API struct RClass*
mrb_digest_define_algorithm(mrb_state *mrb, const char *name, const mrb_digest_metadata *md)
{
  struct RClass *digest = mrb_module_get_id(mrb, MRB_SYM(Digest));
  struct RClass *base = mrb_class_get_under_id(mrb, digest, MRB_SYM(Base));
  struct RClass *c = mrb_define_class_under(mrb, digest, name, base);

  mrb_iv_set(mrb, mrb_obj_value(c), DIGEST_METADATA_IV, mrb_cptr_value(mrb, (void*)md));
  return c;
}

/* Defined in the file of the algorithm it registers. */
void mrb_digest_sha256_init(mrb_state *mrb);

void
mrb_mruby_digest_gem_init(mrb_state *mrb)
{
  struct RClass *digest = mrb_define_module_id(mrb, MRB_SYM(Digest));
  mrb_define_module_function_id(mrb, digest, MRB_SYM(hexencode), digest_s_hexencode, MRB_ARGS_REQ(1));

  /* The three of them stand where CRuby's do: Instance carries what every
     digest object answers, Class is what a digest class is, and Base is the
     one whose instances hold a context. mrblib/digest.rb fills the first two
     in; it runs after this, which is why the classes themselves are made
     here rather than there. */
  struct RClass *instance = mrb_define_module_under_id(mrb, digest, MRB_SYM(Instance));
  struct RClass *klass = mrb_define_class_under_id(mrb, digest, MRB_SYM(Class), mrb->object_class);
  mrb_include_module(mrb, klass, instance);

  struct RClass *base = mrb_define_class_under_id(mrb, digest, MRB_SYM(Base), klass);
  MRB_SET_INSTANCE_TT(base, MRB_TT_CDATA);
  MRB_MT_INIT_ROM(mrb, base, digest_base_rom_entries);

  mrb_digest_sha256_init(mrb);
}

void
mrb_mruby_digest_gem_final(mrb_state *mrb)
{
}
