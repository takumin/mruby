# The alphabet the audit reads answers in.
#
# The driver evaluates this file in the host CRuby and prepends it to the
# script every mruby binary runs, so an answer is spelled the same on both
# sides and a difference in the report is a difference in behavior rather
# than one in `inspect`. That is why a string is dumped as its bytes: a build
# with MRB_UTF8_STRING inspects a non-ASCII string one way, a build without it
# another, and CRuby a third, while the bytes are the thing under audit.
#
# Everything here has to run on a byte-indexed mruby with nothing but the core
# string methods, so it stays on `bytesize`, `getbyte` and `while`, and reaches
# a MatchData or a Regexp through its class name rather than the constant,
# which a build without mruby-regexp does not have.

def __audit_hex(str)
  digits = "0123456789abcdef"
  out = ""
  i = 0
  n = str.bytesize
  while i < n
    b = str.getbyte(i)
    out << digits[b >> 4] << digits[b & 15]
    i += 1
  end
  out
end

def __audit_try
  yield
rescue Exception
  "?"
end

def __audit_list(ary)
  out = "["
  i = 0
  while i < ary.size
    out << ", " if i > 0
    out << __audit_dump(ary[i])
    i += 1
  end
  out << "]"
end

def __audit_match_data(md)
  groups = __audit_try { __audit_dump(md.to_a) }
  starts = __audit_try {
    a = []
    i = 0
    while i < md.size
      a << md.begin(i)
      i += 1
    end
    __audit_list(a)
  }
  stops = __audit_try {
    a = []
    i = 0
    while i < md.size
      a << md.end(i)
      i += 1
    end
    __audit_list(a)
  }
  pre = __audit_try { __audit_dump(md.pre_match) }
  post = __audit_try { __audit_dump(md.post_match) }
  "#<MatchData groups=" + groups + " begin=" + starts + " end=" + stops +
    " pre=" + pre + " post=" + post + ">"
end

def __audit_dump(v)
  case v
  when nil then "nil"
  when true then "true"
  when false then "false"
  when String then "s:" + __audit_hex(v)
  when Symbol then "y:" + __audit_hex(v.to_s)
  when Integer then v.to_s
  when Array then __audit_list(v)
  when Hash
    out = "{"
    first = true
    v.each do |k, val|
      out << ", " unless first
      first = false
      out << __audit_dump(k) << "=>" << __audit_dump(val)
    end
    out << "}"
  when Range
    out = "(" + __audit_dump(v.begin)
    out << (v.exclude_end? ? "..." : "..")
    out << __audit_dump(v.end) << ")"
  else
    # Float is read off the class name rather than matched as a constant, so
    # the probe still runs on a build that defines MRB_NO_FLOAT and has no
    # Float class for a `when` to name.
    name = __audit_try { v.class.to_s }
    if name == "Float"
      v.to_s
    elsif name == "MatchData"
      __audit_match_data(v)
    elsif name == "Regexp"
      "#<Regexp source=" + __audit_try { "s:" + __audit_hex(v.source) } +
        " options=" + __audit_try { v.options.to_s } + ">"
    else
      "#<" + name + " " + __audit_try { "s:" + __audit_hex(v.to_s) } + ">"
    end
  end
end

# An answer is either a value or the exception raised instead of one. The
# exception carries its message along, but the driver compares the class alone
# unless a case pins the message: the same refusal is worded differently by
# each engine, and a wording is not what a case is about.
def __audit_capture
  ["v", __audit_dump(yield)]
rescue Exception => e
  ["e", __audit_try { e.class.to_s } + " s:" + __audit_hex(__audit_try { e.message.to_s })]
end

# The generated script is a run of these, one per case. The block gives each
# case its own scope, so a local one case assigns is not there for the next,
# and a case reads the same whether it runs in the batch or alone.
def __audit_emit(index)
  answer = __audit_capture { yield }
  puts index.to_s + "\t" + answer[0] + "\t" + answer[1]
end
