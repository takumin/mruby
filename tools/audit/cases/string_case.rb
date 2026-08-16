# What the case methods answer for a character that is not ASCII.
#
# A build that reads a string as characters is expected to answer what CRuby
# answers, so most of these are one line. A build that reads it as bytes has
# no character to convert above ASCII and leaves those bytes alone, which is
# what the `bytes:` answers spell out.

group 'String#downcase' do
  check '"ÄÖÜ".downcase', bytes: 'ÄÖÜ'
  check '"İ".downcase', bytes: 'İ'
  check '"K".downcase', bytes: "K",
        note: 'U+212A KELVIN SIGN, which looks like an ASCII K; three bytes down to one'
  check '"ΣΟΦΟΣ".downcase', bytes: 'ΣΟΦΟΣ',
        note: 'word-final sigma reads its neighbours, and CRuby does not apply it either'
  check '"aBc".downcase'
  check '"\xC3ABC".downcase', bytes: "\xC3abc",
        note: 'a byte-indexed build has no character to refuse and lowercases the ASCII'
end

group 'String#upcase' do
  check '"ß".upcase', bytes: 'ß'
  check '"ﬁ".upcase', bytes: 'ﬁ'
  check '"ı".upcase', bytes: 'ı'
  check '"ა".upcase', bytes: 'ა'
  check '"aBc".upcase'
end

group 'String#capitalize' do
  check '"ǳabc".capitalize', bytes: 'ǳabc'
  check '"ა".capitalize'
  check '"aBC".capitalize'
end

group 'String#swapcase' do
  check '"ßA".swapcase', bytes: 'ßa'
  check '"ǅ".swapcase', bytes: 'ǅ',
        note: 'swaps to what neither of its cases spells'
  check '"aBc".swapcase'
end

group 'String#casecmp and #casecmp?' do
  check '"ä".casecmp("Ä")', note: 'casecmp orders by ASCII case in CRuby too'
  check '"ä".casecmp?("Ä")', bytes: false
  check '"ß".casecmp?("ss")', bytes: false
  check '"İ".casecmp?("i")'
  check '"\xC3".casecmp("a")', note: 'a byte of 0x80 or above is not a negative one'
  check '"abc".casecmp?(1)'
end

group 'the receiver is left as it was' do
  check 's = "ÄÖÜ"; s.downcase; s', bytes: 'ÄÖÜ'
  check 's = "\xC3ABC"; (s.downcase rescue nil); s', bytes: "\xC3ABC"
  check 's = "ÄÖÜ"; s.downcase!; s', bytes: 'ÄÖÜ'
  check 's = "äöü"; [s.downcase!, s]', bytes: [nil, 'äöü']
end
