##
# Version constants

assert('MRUBY_VERSION') do
  assert_kind_of(String, MRUBY_VERSION)
  assert_equal(RUBY_ENGINE_VERSION, MRUBY_VERSION)
end

assert('MRUBY_REVISION') do
  assert_kind_of(String, MRUBY_REVISION)
  # The revision a build could not read is the empty one; what it could read is
  # an abbreviated commit hash, and nothing else belongs in it.
  i = 0
  while i < MRUBY_REVISION.size
    assert_include('0123456789abcdef', MRUBY_REVISION[i])
    i += 1
  end
end

assert('MRUBY_DESCRIPTION names the revision it was built from') do
  skip 'built without a revision' if MRUBY_REVISION.empty?
  assert_include(MRUBY_DESCRIPTION, " revision #{MRUBY_REVISION}")
end

assert('MRUBY_DESCRIPTION') do
  assert_include(MRUBY_DESCRIPTION, MRUBY_VERSION)
  assert_include(MRUBY_DESCRIPTION, MRUBY_RELEASE_DATE)
end
