# mruby-vfs

A virtual filesystem, and `require`, `require_relative` and `load` over it.

The `VFS` module is a table of backends mounted at path prefixes. A path is
answered by the mount with the longest prefix that covers it, and the mount at
`/`, the root, covers everything no other mount claims. A backend answers two
questions, what kind of entry a path names and what bytes a regular file holds,
and that is all the loaders ask; so a program can `require` from the machine's
disk, from a `Hash` of sources held in memory, from a table compiled into the
binary, or from anything else that can answer those two questions, with the
same `$LOAD_PATH` in front of all of them.

```ruby
$LOAD_PATH << "/app/lib"
require "greeting"                # /app/lib/greeting.rb, once
require "greeting"                #=> false
load "/app/config.rb"             # every time

# a library that lives in the binary rather than on a disk
VFS.mount("/builtin", VFS::Memory.new("colors.rb" => "COLORS = %w[red green]"))
$LOAD_PATH << "/builtin"
require "colors"
```

## Kernel methods

- `require(name)`: loads the feature once and answers `true`, or `false` when
  its path is already in `$LOADED_FEATURES`. A name that starts with `/`,
  `./` or `../` is taken as it is; any other is looked for under each
  directory of `$LOAD_PATH` in turn. Without an extension, `name.rb` is tried
  before `name.mrb`. Raises `LoadError`, whose `path` is the name, when
  nothing is found.
- `require_relative(name)`: like `require`, with the name taken relative to
  the directory of the file the call is written in. Raises `LoadError` when
  that file is not known, as it is not for code given to `eval` without a
  file name.
- `load(path)`: runs the file every time and records nothing. The path is
  taken as it is, with no extension added; a bare name not found as it is
  is looked for under `$LOAD_PATH`. The `wrap` argument of CRuby is not
  supported, and a true value raises `NotImplementedError`.
- `__dir__`: the directory of the file the call is written in, as the file
  was named when it was loaded; `nil` when it is not known.
- `$LOAD_PATH` (`$:`) and `$LOADED_FEATURES` (`$"`) are Arrays the loaders
  read on every call. The short names are the same Arrays under a second
  name, not aliases of the variables: assigning `$LOAD_PATH` a new Array
  leaves `$:` with the old one, so add to the Array rather than replace it.

A file runs at the top level, as the main program does: `self` is `main`, the
target class `Object`, the local variables its own. `__FILE__` is the path the
search ended at, cleaned of `.` and `..` segments and repeated slashes, and
that is also what `$LOADED_FEATURES` records; a relative path stays relative,
so a file reached once by an absolute path and once by a relative one is two
features. A file that raises is not recorded, and a later `require` tries it
again. A `require` that arrives at a file being loaded answers `false`.

Whether the content is Ruby source or RITE bytecode is read from the bytes,
not the extension. Ruby source needs `mruby-compiler` in the build; a build
without it, such as one around `mruby-bin-mrb`, loads `.mrb` files and raises
`LoadError` for source.

## VFS module

- `VFS.mount(prefix, backend)`: mounts the backend at an absolute path, in
  place of whatever was mounted there, and returns it.
- `VFS.umount(prefix)`: unmounts and returns the backend, or `nil`.
- `VFS.mounts`: the table as `[prefix, backend]` pairs, longest prefix first.
- `VFS.stat(path)`: `:file`, `:directory`, `:other` or `nil`.
- `VFS.exist?(path)`, `VFS.file?(path)`, `VFS.directory?(path)`.
- `VFS.read(path)`: the content of a regular file as a String. Raises
  `Errno::ENOENT` when there is no such file.

### Backends

A backend is any object with two methods. `stat(path)` answers `:file`,
`:directory`, `:other` or `nil` for no such entry; `read(path)` answers the
content of a regular file as a String, or `nil` for no such entry. The path
a backend sees is the part below its mount, starting with the slash; the root
mount sees the path as it was given, relative paths included, which is how a
`$LOAD_PATH` entry like `"lib"` reaches the host's current directory.

Three come with the gem:

- `VFS::Host`: the files of the machine mruby runs on, through the port. One
  is mounted at the root when the gem initializes, where the port has a host
  to read (see below).
- `VFS::Memory`: a `Hash` of paths to contents. Paths are normalized, so a
  file put in as `"lib/a.rb"` is read as `"/lib/a.rb"`, and every directory a
  file lies under exists. `VFS::Memory.new(files)`, `mem[path] = content`,
  `mem[path]`, `mem.delete(path)`, `mem.paths`.
- `VFS::Backend`: a backend written in C, from `mrb_vfs_backend_new()`.
  `VFS::Host` is one. It is not made from Ruby.

## C API

`<mruby/vfs.h>` declares:

```c
enum mrb_vfs_kind { MRB_VFS_NONE, MRB_VFS_FILE, MRB_VFS_DIRECTORY, MRB_VFS_OTHER };

typedef struct mrb_vfs_ops {
  int (*stat)(mrb_state *mrb, void *data, const char *path, enum mrb_vfs_kind *kind);
  mrb_value (*read)(mrb_state *mrb, void *data, const char *path);
  void (*dfree)(mrb_state *mrb, void *data);   /* may be NULL */
} mrb_vfs_ops;

mrb_value mrb_vfs_backend_new(mrb_state *mrb, const mrb_vfs_ops *ops, void *data);
mrb_value mrb_vfs_mount(mrb_state *mrb, const char *prefix, mrb_value backend);
mrb_value mrb_vfs_umount(mrb_state *mrb, const char *prefix);
int       mrb_vfs_stat(mrb_state *mrb, const char *path, enum mrb_vfs_kind *kind);
mrb_value mrb_vfs_read(mrb_state *mrb, const char *path);
mrb_value mrb_vfs_load(mrb_state *mrb, const char *path);
mrb_bool  mrb_vfs_require(mrb_state *mrb, const char *feature);
void      mrb_vfs_load_path_push(mrb_state *mrb, const char *dir);
```

`stat` writes what the path names and returns 0, with `MRB_VFS_NONE` for no
such entry, or returns -1 with `errno` set when it could not answer. `read`
returns the content as a String, `nil` for no such entry, and raises (through
`mrb_sys_fail()` when `errno` says why) for a file it cannot read. A table of
scripts compiled into the firmware is a backend of a dozen lines:

```c
static int
table_stat(mrb_state *mrb, void *data, const char *path, enum mrb_vfs_kind *kind)
{
  *kind = find_entry((const struct entry*)data, path) ? MRB_VFS_FILE : MRB_VFS_NONE;
  return 0;
}

static mrb_value
table_read(mrb_state *mrb, void *data, const char *path)
{
  const struct entry *e = find_entry((const struct entry*)data, path);
  return e ? mrb_str_new_static(mrb, e->bytes, e->size) : mrb_nil_value();
}

static const mrb_vfs_ops table_ops = { table_stat, table_read, NULL };

mrb_vfs_mount(mrb, "/rom", mrb_vfs_backend_new(mrb, &table_ops, (void*)entries));
mrb_vfs_load_path_push(mrb, "/rom");
```

## What the port declares

Whether the host's files are reachable is the port's to say, since the port is
what a build names and a `hal-vfs-<conf>` gem may stand in for the bundled
ones. Each port publishes a `vfs_hal_features.h` in its `include/`, which
`include/vfs_hal.h` reads before it declares anything. One macro there guards
the prototypes, the port's implementation, and the `VFS::Host` class with its
root mount, so a port that declares nothing has no host, mounts nothing, and
owes no function; the build then mounts what it has, from Ruby or C. A port
that declares the capability and does not implement it fails to link.

| macro                  | what it guards                            | posix | win |
| ---------------------- | ----------------------------------------- | ----- | --- |
| `MRB_HAL_VFS_HAS_HOST` | `VFS::Host`, mounted at the root on start | o     | o   |

The HAL a port implements is in `include/vfs_hal.h`: `mrb_hal_vfs_stat()`,
`mrb_hal_vfs_open()`, `mrb_hal_vfs_read()`, `mrb_hal_vfs_close()`, and
`mrb_hal_vfs_init()` and `mrb_hal_vfs_final()`. Paths are UTF-8; the Windows
port converts them to UTF-16 and reads through the wide CRT calls.

## License

MIT License. See Copyright Notice in mruby.h.
