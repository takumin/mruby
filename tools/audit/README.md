# String / Regexp behavior audit

Runs a list of expressions through the host CRuby and through every mruby
under `build/*/bin`, and reports where the answers part company. It is meant
for the throwaway script that gets written by hand before a String or Regexp
pull request: write the expressions once, keep them, and run them against
every build each time.

```console
$ MRUBY_CONFIG=ci/gcc-clang rake            # whatever builds you mean to audit
$ ruby tools/audit/behavior.rb
String / Regexp behavior audit
  oracle  ruby 3.3.6 (2024-11-05 revision 75015d4c1f) [x86_64-linux]
  cases   83 from 3 files

  build          model  traits                            ok  diff  skip crash
  bintest        chars  regexp unicode-case               81     0     2     0
  byte-string    bytes  regexp                            81     0     2     0
  cxx_abi        chars  regexp unicode-case               81     0     2     0
  full-debug     chars  regexp unicode-case /i-unicode    83     0     0     0

83 cases x 4 builds: 326 ok, 0 diff, 6 skip, 0 crash
```

It exits non-zero when a build answers something the audit did not expect, so
it can gate a commit as well as be read.

## Running it

|                                        |                                                 |
| -------------------------------------- | ----------------------------------------------- |
| `ruby tools/audit/behavior.rb`         | every file under `tools/audit/cases`            |
| `ruby tools/audit/behavior.rb FILE...` | those files, or every `.rb` under a directory   |
| `--build NAME`                         | audit that build alone; repeatable              |
| `--mruby PATH`                         | audit a binary that is not under `build/`       |
| `--ruby PATH`                          | the CRuby that answers for the cases            |
| `--format md`                          | a table to paste into a pull request            |
| `--isolate`                            | one process per case, rather than one per build |
| `--list`                               | say what builds were found and what they are    |
| `--timeout SECONDS`                    | how long one run may take before it is killed   |

## Writing a case

A case is an expression, and unless it says otherwise the answer to compare
it against is whatever the host CRuby answers. That is the whole of the usual
case:

```ruby
group 'String#downcase' do
  check '"aBc".downcase'
end
```

`check` takes four things beside the expression:

- **`bytes:`** — what a build that indexes a string by byte answers instead.
  `"あ".length` is 1 where `MRB_UTF8_STRING` is on and 3 where it is off, and
  the case says the second answer itself:

  ```ruby
  check '"あいう".length', bytes: 9
  check '"あいう"[1]', bytes: "\x81"
  check '"あいう".index("い")', bytes: 3
  ```

  A byte-indexed build that diverges without such a declaration **fails**, and
  the report says the declaration is what is missing. `bytes: :same` says the
  answer does not move and silences that; `bytes: :skip` drops the case there.

- **`want:`** — an answer to pin, when CRuby is not the authority. The audit
  still asks CRuby, and reports every pinned case whose answer CRuby does not
  give, so a divergence someone decided to keep stays visible:

  ```ruby
  check '"あいう".match("\xC3")', want: nil,
        note: 'CRuby refuses a pattern that spells no character; mruby finds nothing'
  ```

  `raises(Klass)` pins a refusal, and `raises(Klass, "message")` pins its
  wording too.

- **`needs:`** — what the build has to be for the case to mean anything, as
  one symbol or several. A build that is not it skips the case rather than
  failing it. `:regexp`, `:unicode_case`, `:regexp_unicode_case`, `:chars`,
  `:bytes`. `group` takes it as well, and a case adds to what its group asks.

- **`note:`** — why the answer is what it is. It is printed with the case
  whenever the case has something to report.

An expression is a string, so it can be several statements, and the last one
is the answer:

```ruby
check 's = "ÄÖÜ"; s.downcase; s', bytes: 'ÄÖÜ'
```

Write a character that is easy to misread as an escape rather than as itself:
`"K"` is the KELVIN SIGN and looks exactly like an ASCII `K` in a case
file.

## What is compared

A string is compared as its bytes. `inspect` is no good for this: a build
spells a non-ASCII string one way with `MRB_UTF8_STRING`, another way without
it, and CRuby a third, and none of that is the behavior under audit. The
report renders the bytes back into something readable.

An exception is compared by its class alone, since the same refusal is worded
differently by each engine. `raises(Klass, "message")` is how a case says it
means the wording too.

## What a build is asked

A build is asked what it is rather than having it read off its name, so
`--mruby` on some other binary works the same way:

| trait                 | the question                           |
| --------------------- | -------------------------------------- |
| `model`               | `"あ".length` — `chars` or `bytes`     |
| `regexp`              | is there a `Regexp` class              |
| `unicode_case`        | does `"Ä".downcase` answer `"ä"`       |
| `regexp_unicode_case` | does `/i` fold a character above ASCII |

## Statuses

|         |                                                          |
| ------- | -------------------------------------------------------- |
| `ok`    | the build answered what the case expects                 |
| `diff`  | it answered something else                               |
| `skip`  | the build is not what the case needs                     |
| `crash` | the process died, timed out, or exited without answering |

The run is one process per build, which is fast; a build that dies takes the
rest of that run with it, so whatever is left unanswered is asked again one
case at a time until the case that killed it is named. `--isolate` starts
there instead.

## What it does not do

It audits behavior and nothing else: the test suites, the binary size and
the per-commit build are still `rake test` and a shell loop. A case file is
also compiled as a whole, so a build without `mruby-regexp` fails to compile
a file holding a regexp literal rather than skipping those cases — reach for
`Regexp.new` in a case that has to survive that build.
