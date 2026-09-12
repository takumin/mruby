/*
** sha256.c - Digest::SHA256
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/digest.h>

#include <string.h>

/* SHA-256, as FIPS 180-4 defines it. The names below are the ones the
   standard uses, so that the rounds can be read against section 6.2. */

#define SHA256_BLOCK_LENGTH  64
#define SHA256_DIGEST_LENGTH 32

typedef struct sha256_ctx {
  uint32_t state[8];
  uint64_t count;                    /* bytes fed in so far */
  uint8_t buf[SHA256_BLOCK_LENGTH];  /* what is left of the last block */
} sha256_ctx;

/* FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the
   cube roots of the first 64 primes. */
static const uint32_t sha256_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/* The bytes of a block are read and the digest is written a byte at a time,
   since the standard spells both in big-endian and reading a word off the
   buffer would answer differently on the two ends. */
static void
sha256_compress(uint32_t state[8], const uint8_t block[SHA256_BLOCK_LENGTH])
{
  uint32_t w[64];

  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
           ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
  }
  for (int i = 16; i < 64; i++) {
    w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + sha256_k[i] + w[i];
    uint32_t t2 = BSIG0(a) + MAJ(a, b, c);

    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* FIPS 180-4 section 5.3.3: the first 32 bits of the fractional parts of the
   square roots of the first eight primes. */
static void
sha256_init(void *ctx_)
{
  sha256_ctx *ctx = (sha256_ctx*)ctx_;

  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
  ctx->count = 0;
}

static void
sha256_update(void *ctx_, const void *data, size_t len)
{
  sha256_ctx *ctx = (sha256_ctx*)ctx_;
  const uint8_t *p = (const uint8_t*)data;
  size_t used = (size_t)(ctx->count % SHA256_BLOCK_LENGTH);

  ctx->count += len;

  /* Fill what the last call left behind first: only a full block is
     compressed, so the buffer holds fewer than SHA256_BLOCK_LENGTH bytes
     whenever a call returns. */
  if (used > 0) {
    size_t n = SHA256_BLOCK_LENGTH - used;

    if (len < n) {
      memcpy(ctx->buf + used, p, len);
      return;
    }
    memcpy(ctx->buf + used, p, n);
    sha256_compress(ctx->state, ctx->buf);
    p += n;
    len -= n;
  }

  /* What is whole is compressed where it lies, so a long string is not
     copied through the buffer a block at a time. */
  while (len >= SHA256_BLOCK_LENGTH) {
    sha256_compress(ctx->state, p);
    p += SHA256_BLOCK_LENGTH;
    len -= SHA256_BLOCK_LENGTH;
  }

  if (len > 0) memcpy(ctx->buf, p, len);
}

static void
sha256_finish(void *ctx_, unsigned char *digest)
{
  sha256_ctx *ctx = (sha256_ctx*)ctx_;
  uint64_t bits = ctx->count * 8;
  size_t used = (size_t)(ctx->count % SHA256_BLOCK_LENGTH);

  /* FIPS 180-4 section 5.1.1: a 1 bit, then zeros, then the message length
     in bits as a 64-bit big-endian number, filling the block out. Where the
     length no longer fits, the zeros run to the end of this block and the
     length goes in the next one. */
  ctx->buf[used++] = 0x80;
  if (used > SHA256_BLOCK_LENGTH - 8) {
    memset(ctx->buf + used, 0, SHA256_BLOCK_LENGTH - used);
    sha256_compress(ctx->state, ctx->buf);
    used = 0;
  }
  memset(ctx->buf + used, 0, SHA256_BLOCK_LENGTH - 8 - used);
  for (int i = 0; i < 8; i++) {
    ctx->buf[SHA256_BLOCK_LENGTH-8+i] = (uint8_t)(bits >> (56 - i*8));
  }
  sha256_compress(ctx->state, ctx->buf);

  for (int i = 0; i < 8; i++) {
    digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
    digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
    digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
    digest[i*4+3] = (uint8_t)ctx->state[i];
  }
}

static const mrb_digest_metadata sha256_metadata = {
  (uint32_t)sizeof(sha256_ctx),
  SHA256_BLOCK_LENGTH,
  SHA256_DIGEST_LENGTH,
  sha256_init,
  sha256_update,
  sha256_finish
};

void
mrb_digest_sha256_init(mrb_state *mrb)
{
  mrb_digest_define_algorithm(mrb, "SHA256", &sha256_metadata);
}
