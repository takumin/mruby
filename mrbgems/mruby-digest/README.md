# mruby-digest

Cryptographic hash functions, under the `Digest` module that CRuby's
`digest` library defines.

```ruby
Digest::SHA256.hexdigest("abc")
#=> "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

d = Digest::SHA256.new
d << "ab"
d << "c"
d.hexdigest  #=> "ba7816bf..."
d.digest     #=> the same 32 bytes, as an ASCII-8BIT string
```

## What is here

- `Digest::Instance`, the methods every digest object answers.
- `Digest::Class`, what a digest class is, with `.digest` and `.hexdigest`.
- `Digest::Base`, whose instances hold the state of one algorithm.
- `Digest::SHA256`.
- `Digest.hexencode(string)`.

`#digest` hands back the bytes the algorithm produced, in a string that
says it is `ASCII-8BIT` where the build tells one encoding from another;
`#hexdigest` hands back those bytes spelled as lowercase hexadecimal.

`#base64digest` and `#file` are not here: the first wants Base64 and the
second wants a file, and neither is what this gem is about.

## Adding an algorithm

An algorithm is three calls over a block of state, handed to
`mrb_digest_define_algorithm()` in `<mruby/digest.h>`. Everything else,
from allocating the state to copying it for `#dup`, belongs to
`Digest::Base`. `src/sha256.c` is the whole of what an algorithm has to
write.

```c
static const mrb_digest_metadata sha256_metadata = {
  (uint32_t)sizeof(sha256_ctx),
  SHA256_BLOCK_LENGTH,
  SHA256_DIGEST_LENGTH,
  sha256_init,
  sha256_update,
  sha256_finish
};

mrb_digest_define_algorithm(mrb, "SHA256", &sha256_metadata);
```

A class defined this way can be subclassed in Ruby, and the subclass
computes the same algorithm.

## License

MIT
