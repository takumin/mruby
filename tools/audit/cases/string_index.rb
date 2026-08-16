# Where a string method counts from, and in what unit.
#
# This is the file that separates the two string models: an index is a
# character index where MRB_UTF8_STRING is on and a byte index where it is
# off, and every case whose answer moves says so with `bytes:` rather than
# being left to the reader. "あいう" is E3 81 82 E3 81 84 E3 81 86, so the
# byte answers below are cuts through those nine bytes.

group 'how long a string is' do
  check '"あいう".length', bytes: 9
  check '"あいう".size', bytes: 9
  check '"あいう".bytesize'
  check '"".length'
  check '"\xC3".length'
end

group 'String#[]' do
  check '"あいう"[1]', bytes: "\x81"
  check '"あいう"[-1]', bytes: "\x86"
  check '"あいう"[0, 2]', bytes: "\xE3\x81"
  check '"あいう"[1, 1]', bytes: "\x81"
  check '"あいう"[3]', bytes: "\xE3"
  check '"あいう"[9]'
  check '"あいう"[0..1]', bytes: "\xE3\x81"
  check '"あいう".slice(1, 2)', bytes: "\x81\x82"
end

group 'where a substring is' do
  check '"あいう".index("い")', bytes: 3
  check '"あいう".rindex("う")', bytes: 6
  check '"あいう".index("え")'
  check '"あいう".byteindex("い")',
        note: 'byteindex answers in bytes under either model, which is the point of it'
  check '"あいう".byterindex("う")'
  check '"あいう".byteindex("い", -9)'
  check '"あいう".byteindex("い", -10)'
  check '"あいう".byteindex("", 9)'
  check '"あいう".byteindex("", 10)'
end

group 'walking the characters' do
  check '"あいう".chars', bytes: ["\xE3", "\x81", "\x82", "\xE3", "\x81", "\x84", "\xE3", "\x81", "\x86"]
  check '"あいう".reverse', bytes: "\x86\x81\xE3\x84\x81\xE3\x82\x81\xE3"
  check '"あ".ord', bytes: 227
  check '"あいう".each_char.to_a', bytes: ["\xE3", "\x81", "\x82", "\xE3", "\x81", "\x84", "\xE3", "\x81", "\x86"]
  check '"あいう".bytes'
end

group 'padding counts what the string is read as' do
  check '"あ".center(5, "-")', bytes: '-あ-'
  check '"あ".ljust(5, "-")', bytes: 'あ--'
  check '"あ".rjust(5, "-")', bytes: '--あ'
end

group 'a string that spells no character' do
  check '"\xC3ABC".bytesize'
  check '"\xC3ABC"[0]', bytes: "\xC3"
  check '"\xC3ABC".length', bytes: 4
end
