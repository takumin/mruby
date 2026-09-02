# mruby-io

`IO` and `File` classes for mruby

## Installation

Add the line below to your build configuration.

```
  conf.gem core: 'mruby-io'
```

## Implemented methods

### IO

- <https://doc.ruby-lang.org/ja/1.9.3/class/IO.html>

| method                     | mruby-io | memo     |
| -------------------------- | -------- | -------- |
| IO.binread                 |          |          |
| IO.binwrite                |          |          |
| IO.copy_stream             |          |          |
| IO.new, IO.for_fd, IO.open | o        |          |
| IO.foreach                 |          |          |
| IO.pipe                    | o        |          |
| IO.popen                   | o        |          |
| IO.read                    | o        |          |
| IO.readlines               |          |          |
| IO.select                  | o        |          |
| IO.sysopen                 | o        |          |
| IO.try_convert             |          |          |
| IO.write                   |          |          |
| IO#<<                      |          |          |
| IO#advise                  |          |          |
| IO#autoclose=              | o        |          |
| IO#autoclose?              | o        |          |
| IO#binmode                 |          |          |
| IO#binmode?                |          |          |
| IO#bytes                   |          | obsolete |
| IO#chars                   |          | obsolete |
| IO#clone, IO#dup           | o        |          |
| IO#close                   | o        |          |
| IO#close_on_exec=          | o        |          |
| IO#close_on_exec?          | o        |          |
| IO#close_read              |          |          |
| IO#close_write             |          |          |
| IO#closed?                 | o        |          |
| IO#codepoints              |          | obsolete |
| IO#each_byte               | o        |          |
| IO#each_char               | o        |          |
| IO#each_codepoint          |          |          |
| IO#each_line               | o        |          |
| IO#eof, IO#eof?            | o        |          |
| IO#external_encoding       |          |          |
| IO#fcntl                   |          |          |
| IO#fdatasync               |          |          |
| IO#fileno, IO#to_i         | o        |          |
| IO#flush                   | o        |          |
| IO#fsync                   |          |          |
| IO#getbyte                 | o        |          |
| IO#getc                    | o        |          |
| IO#gets                    | o        |          |
| IO#internal_encoding       |          |          |
| IO#ioctl                   |          |          |
| IO#isatty, IO#tty?         | o        |          |
| IO#lineno                  |          |          |
| IO#lineno=                 |          |          |
| IO#lines                   |          | obsolete |
| IO#pid                     | o        |          |
| IO#pos, IO#tell            | o        |          |
| IO#pos=                    | o        |          |
| IO#print                   | o        |          |
| IO#printf                  | o        |          |
| IO#putc                    |          |          |
| IO#puts                    | o        |          |
| IO#read                    | o        |          |
| IO#read_nonblock           |          |          |
| IO#readbyte                | o        |          |
| IO#readchar                | o        |          |
| IO#readline                | o        |          |
| IO#readlines               | o        |          |
| IO#readpartial             |          |          |
| IO#reopen                  |          |          |
| IO#rewind                  |          |          |
| IO#seek                    | o        |          |
| IO#set_encoding            |          |          |
| IO#stat                    |          |          |
| IO#sync                    | o        |          |
| IO#sync=                   | o        |          |
| IO#sysread                 | o        |          |
| IO#sysseek                 | o        |          |
| IO#syswrite                | o        |          |
| IO#to_io                   |          |          |
| IO#ungetbyte               | o        |          |
| IO#ungetc                  | o        |          |
| IO#write                   | o        |          |
| IO#write_nonblock          |          |          |

### File

- <https://doc.ruby-lang.org/ja/1.9.3/class/File.html>

| method                      | mruby-io | memo     |
| --------------------------- | -------- | -------- |
| File.absolute_path          |          |          |
| File.atime                  |          |          |
| File.basename               | o        |          |
| File.blockdev?              |          | FileTest |
| File.chardev?               |          | FileTest |
| File.chmod                  | o        |          |
| File.chown                  |          |          |
| File.ctime                  |          |          |
| File.delete, File.unlink    | o        |          |
| File.directory?             | o        | FileTest |
| File.dirname                | o        |          |
| File.executable?            |          | FileTest |
| File.executable_real?       |          | FileTest |
| File.exist?, exists?        | o        | FileTest |
| File.expand_path            | o        |          |
| File.extname                | o        |          |
| File.file?                  | o        | FileTest |
| File.fnmatch, File.fnmatch? |          |          |
| File.ftype                  |          |          |
| File.grpowned?              |          | FileTest |
| File.identical?             |          | FileTest |
| File.join                   | o        |          |
| File.lchmod                 |          |          |
| File.lchown                 |          |          |
| File.link                   |          |          |
| File.lstat                  |          |          |
| File.mtime                  |          |          |
| File.new, File.open         | o        |          |
| File.owned?                 |          | FileTest |
| File.path                   |          |          |
| File.pipe?                  | o        | FileTest |
| File.readable?              |          | FileTest |
| File.readable_real?         |          | FileTest |
| File.readlink               | o        |          |
| File.realdirpath            |          |          |
| File.realpath               | o        |          |
| File.rename                 | o        |          |
| File.setgid?                |          | FileTest |
| File.setuid?                |          | FileTest |
| File.size                   | o        |          |
| File.size?                  | o        | FileTest |
| File.socket?                | o        | FileTest |
| File.split                  |          |          |
| File.stat                   |          |          |
| File.sticky?                |          | FileTest |
| File.symlink                |          |          |
| File.symlink?               | o        | FileTest |
| File.truncate               |          |          |
| File.umask                  | o        |          |
| File.utime                  |          |          |
| File.world_readable?        |          |          |
| File.world_writable?        |          |          |
| File.writable?              |          | FileTest |
| File.writable_real?         |          | FileTest |
| File.zero?                  | o        | FileTest |
| File#atime                  | o        |          |
| File#chmod                  |          |          |
| File#chown                  |          |          |
| File#ctime                  | o        |          |
| File#flock                  | o        |          |
| File#lstat                  |          |          |
| File#mtime                  | o        |          |
| File#path                   | o        |          |
| File#size                   |          |          |
| File#truncate               |          |          |

## IO.popen and mruby-process

A pipe to a command is two things: a pipe, and a child process. This gem
provides the first and **mruby-process** provides the second, and `IO.popen`
is written in `mrblib` as the composition of `IO.pipe` and `Process.spawn`.

That has three consequences worth knowing about:

- `IO.popen` needs **mruby-process** in the build, and a build without it has
  no `IO.popen` at all rather than one that fails when it is called: the file
  defining the method is left out, so `IO.respond_to?(:popen)` answers for it
  the way it answers for any other method. A platform with no `IO.pipe`, iOS
  among them, leaves it out for the same reason, which is what this gem's old
  `MRB_NO_IO_POPEN` used to say. The one case where the method is there and
  raises `NotImplementedError` is a build of **mruby-process** made with
  `MRB_NO_PROCESS_SPAWN`: the gem is present and only its spawning is not.
- `IO#close` waits for the command at the other end and sets `$?` to a
  `Process::Status`, through the same `Process.waitpid` anyone else would
  call. A stream that is already closed is left as it is, so the wait happens
  once however many times `#close` is called; a command that was already
  waited for elsewhere leaves nothing to report, and `$?` keeps what that wait
  set.
- `IO#pid` names that command, and keeps naming it after the stream is closed.

The arguments are CRuby's, `IO.popen([env,] cmd, mode = "r" [, opt])`. The
command is whatever `Process.spawn` takes: a String, a `[cmdname, argv0]`
pair, or an Array of the command and its arguments, which may begin with an
environment Hash and end with an options Hash as `Process.spawn`'s arguments
may; a leading Hash outside the Array is the environment as well. The
options are `Process.spawn`'s too, `chdir:`, `pgroup:` and the redirections
included, plus `mode:` as another way to give the mode. The pipe's own ends
are the child's 0 and 1, so naming either of those again is refused with
`fd 1 specified twice`, as CRuby refuses it.

```ruby
IO.popen(["ls", "-l"], chdir: "/tmp") { |io| io.read }
IO.popen({"LANG" => "C"}, "date") { |io| io.read }
IO.popen("cat", mode: "r+") { |io| io.write "x"; io.close_write; io.read }
```

With nothing said about it, the command's standard error is left where this
process's is, as it is in Ruby: diagnostics do not arrive in the pipe the
command's output is read from. Pass `err:` an IO, a descriptor, a file name
or `[:child, :out]` to place it somewhere else.

Two things differ from CRuby. `IO.popen("-")`, Ruby's spelling of "fork
instead of executing a command", is not supported: mruby has no `fork`. And
an option that is neither `Process.spawn`'s nor `mode:` nor `binmode:` is
refused with `ArgumentError`, where CRuby lets one it does not recognise
pass unread; a stream here is bytes either way, so `binmode:` asks for what
it already is.

## Porting Note

If your (non Windows) platform does not support `getpwnam(3)` for some reason, define `MRB_IO_NO_PWNAM`.
See [mruby#5358](https://github.com/mruby/mruby/issues/5358).

## License

Copyright (c) 2013 Internet Initiative Japan Inc.
Copyright (c) 2017 mruby developers

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
