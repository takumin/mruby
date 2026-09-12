##
# Digest Test

# FIPS 180-4 and the NIST example set. The empty message and the 56-byte one
# are the two cases the padding is written for: nothing to pad but the block
# itself, and a length that no longer fits in the block it ends.
SHA256_VECTORS = {
  "" => "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
  "abc" => "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" =>
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
  "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu" =>
    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
}

assert('Digest::SHA256 test vectors') do
  SHA256_VECTORS.each do |message, want|
    assert_equal want, Digest::SHA256.hexdigest(message)
    assert_equal want, Digest::SHA256.new.update(message).hexdigest
  end
end

assert('Digest::SHA256 a message of a million bytes') do
  # The one vector long enough to run the block loop rather than the buffer,
  # fed in pieces that do not line up with a block so that both are used.
  d = Digest::SHA256.new
  1000.times { d << "a" * 1000 }
  assert_equal "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", d.hexdigest
end

assert('Digest::SHA256 a message fed one byte at a time') do
  # Whatever the buffering does with a message, it has to arrive at what the
  # message digests to whole. The lengths are the ones the block boundaries
  # fall on, either side of them, and where the length no longer fits.
  [0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 128, 129].each do |len|
    message = "0123456789" * 13
    message = message[0, len]
    d = Digest::SHA256.new
    len.times { |i| d << message[i, 1] }
    assert_equal Digest::SHA256.hexdigest(message), d.hexdigest, "length #{len}"
  end
end

assert('Digest::SHA256#digest returns the bytes') do
  digest = Digest::SHA256.digest("abc")
  assert_equal 32, digest.bytesize
  assert_equal Digest::SHA256.hexdigest("abc"), Digest.hexencode(digest)
end

assert('Digest::SHA256#digest leaves the object where it found it') do
  d = Digest::SHA256.new
  d << "ab"
  # Given a string, the digest is that string's and the object is emptied;
  # given nothing, the object carries on from where it stood.
  assert_equal Digest::SHA256.hexdigest("xyz"), Digest.hexencode(d.digest("xyz"))
  assert_equal Digest::SHA256.hexdigest(""), d.hexdigest
  d << "ab"
  assert_equal Digest::SHA256.hexdigest("ab"), d.hexdigest
  d << "c"
  assert_equal Digest::SHA256.hexdigest("abc"), d.hexdigest
end

assert('Digest::SHA256#digest! empties the object') do
  d = Digest::SHA256.new
  d << "abc"
  assert_equal Digest::SHA256.digest("abc"), d.digest!
  assert_equal Digest::SHA256.hexdigest(""), d.hexdigest
end

assert('Digest::SHA256#hexdigest!') do
  d = Digest::SHA256.new
  d << "abc"
  assert_equal Digest::SHA256.hexdigest("abc"), d.hexdigest!
  assert_equal Digest::SHA256.hexdigest(""), d.hexdigest
end

assert('Digest::SHA256#reset') do
  d = Digest::SHA256.new
  d << "abc"
  assert_same d, d.reset
  assert_equal Digest::SHA256.hexdigest(""), d.hexdigest
end

assert('Digest::SHA256#dup carries the state and shares none of it') do
  d = Digest::SHA256.new
  d << "ab"
  copy = d.dup
  copy << "c"
  d << "z"
  assert_equal Digest::SHA256.hexdigest("abc"), copy.hexdigest
  assert_equal Digest::SHA256.hexdigest("abz"), d.hexdigest
end

assert('Digest::SHA256#new returns an empty digest of the same class') do
  d = Digest::SHA256.new
  d << "abc"
  fresh = d.new
  assert_equal Digest::SHA256, fresh.class
  assert_equal Digest::SHA256.hexdigest(""), fresh.hexdigest
  assert_equal Digest::SHA256.hexdigest("abc"), d.hexdigest
end

assert('Digest::SHA256 lengths') do
  d = Digest::SHA256.new
  assert_equal 32, d.digest_length
  assert_equal 32, d.length
  assert_equal 32, d.size
  assert_equal 64, d.block_length
end

assert('Digest::SHA256#== and #to_s') do
  d = Digest::SHA256.new.update("abc")
  other = Digest::SHA256.new.update("abc")
  assert_true d == other
  assert_true d == Digest::SHA256.hexdigest("abc")
  assert_equal Digest::SHA256.hexdigest("abc"), d.to_s
  other << "d"
  assert_false d == other
  assert_false d == "not a digest"
  assert_false d == 1
end

assert('Digest::SHA256#inspect') do
  hex = Digest::SHA256.hexdigest("abc")
  d = Digest::SHA256.new.update("abc")
  assert_equal "#<Digest::SHA256: #{hex}>", d.inspect
end

assert('Digest::SHA256#update takes a String and nothing else') do
  d = Digest::SHA256.new
  assert_raise(TypeError) { d << 1 }
  assert_raise(TypeError) { d.update(nil) }
end

assert('Digest::SHA256#finish is private') do
  d = Digest::SHA256.new
  assert_raise(NoMethodError) { d.finish }
end

assert('a frozen digest refuses what would move it along') do
  d = Digest::SHA256.new
  d << "abc"
  d.freeze
  assert_raise(FrozenError) { d << "d" }
  assert_raise(FrozenError) { d.update("d") }
  assert_raise(FrozenError) { d.reset }
  assert_raise(FrozenError) { d.digest! }
  # Reading it is still reading: what these two finish is a copy.
  assert_equal Digest::SHA256.hexdigest("abc"), d.hexdigest
  assert_equal Digest::SHA256.digest("abc"), d.digest
  copy = d.clone
  assert_true copy.frozen?
  assert_equal Digest::SHA256.hexdigest("abc"), copy.hexdigest
  copy = d.dup
  assert_false copy.frozen?
  copy << "d"
  assert_equal Digest::SHA256.hexdigest("abcd"), copy.hexdigest
end

assert('Digest::Base names no algorithm') do
  assert_raise(NotImplementedError) { Digest::Base.new }
end

assert('a digest class subclassed in Ruby computes what it inherits') do
  c = Class.new(Digest::SHA256)
  assert_equal Digest::SHA256.hexdigest("abc"), c.hexdigest("abc")
  assert_equal 32, c.new.digest_length
end

assert('Digest.hexencode') do
  assert_equal "", Digest.hexencode("")
  assert_equal "0001ff80", Digest.hexencode("\x00\x01\xff\x80")
  assert_equal "616263", Digest.hexencode("abc")
  assert_raise(TypeError) { Digest.hexencode(1) }
end

assert('a digest is bytes rather than text') do
  # A digest holds whatever the algorithm produced, which is why it says it
  # is bytes rather than letting them be read as the build's default
  # encoding. The digest of "1" is what shows the difference: two of its
  # bytes read as one UTF-8 character, so a build that indexes by character
  # would count 31 of them in the 32 bytes.
  d = Digest::SHA256.digest("1")
  assert_equal 32, d.bytesize
  assert_equal 32, d.length

  # Where the build carries encodings as well, the digest names the one it
  # is in. mruby-encoding is not among this gem's test dependencies, since
  # it is what turns a build's strings from bytes into UTF-8 and pulling it
  # in would take that choice away from the build; the assertions below run
  # wherever a state carries both gems.
  if d.respond_to?(:encoding)
    assert_equal Encoding::BINARY, d.encoding
    assert_true d.valid_encoding?
    assert_equal Encoding::UTF_8, Digest::SHA256.hexdigest("1").encoding
  end
end
