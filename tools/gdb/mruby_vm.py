"""GDB helpers to view the C stack and the mruby VM call-info stack together.

Usage (on a build made with `conf.enable_debug`):

    gdb -x tools/gdb/mruby_vm.py --args build/host/bin/mruby foo.rb
    (gdb) break mrb_exc_raise
    (gdb) run
    (gdb) mrbbt          # unified C + VM backtrace
    (gdb) mrbci          # raw call-info dump

The VM runs Ruby-to-Ruby calls without recursing into C (`mrb_vm_exec` loops
via `JUMP`), so many Ruby frames live inside a single C frame; a C function
that re-enters the VM (`mrb_funcall`, `mrb_yield`, ...) starts a nested
`mrb_vm_exec`.  `mrbbt` reconstructs that interleaving: it walks `mrb->c->ci`
and the C frames together, anchoring on the `mrb_vm_exec` frames.
"""

import os

import gdb

CINFO_NONE, CINFO_SKIP, CINFO_DIRECT, CINFO_RESUMED = 0, 1, 2, 3
CCI_NAME = {CINFO_NONE: "NONE", CINFO_SKIP: "SKIP",
            CINFO_DIRECT: "DIRECT", CINFO_RESUMED: "RESUMED"}
MRB_PROC_CFUNC_FL = 128
VM_EXEC = "mrb_vm_exec"


def find_mrb(arg):
    """Locate the mrb_state*: explicit expression, else the nearest frame with `mrb`."""
    if arg:
        return gdb.parse_and_eval(arg)
    frame = gdb.newest_frame()
    while frame:
        try:
            v = frame.read_var("mrb")
            if v and str(v.type).replace(" ", "").endswith("mrb_state*"):
                return v
        except (ValueError, gdb.error):
            pass
        frame = frame.older()
    raise gdb.GdbError("no mrb_state* in scope; pass one, e.g. `mrbbt mrb`")


def sym_name(mrb, sym):
    if not int(sym):
        return None
    try:
        s = gdb.parse_and_eval("mrb_sym_name(%d, %d)" % (int(mrb), int(sym)))
        return s.string() if int(s) else None
    except gdb.error:
        return "sym#%d" % int(sym)


def position(mrb, irep, idx):
    """(filename, lineno) for an iseq index, or None when there is no debug info."""
    try:
        f = gdb.parse_and_eval("mrb_debug_get_filename(%d, %d, %d)"
                               % (int(mrb), int(irep), idx))
        ln = int(gdb.parse_and_eval("mrb_debug_get_line(%d, %d, %d)"
                                    % (int(mrb), int(irep), idx)))
    except gdb.error:
        return None
    return (f.string(), ln) if int(f) else None


def shorten(path):
    cwd = os.getcwd() + os.sep
    return path[len(cwd):] if path.startswith(cwd) else path


class VMFrame(object):
    """One decoded mrb_callinfo entry."""

    def __init__(self, mrb, ci, index):
        self.index = index
        self.cci = int(ci["cci"])
        self.mid = sym_name(mrb, ci["mid"])
        self.irep = None
        self.pos = None
        self.idx = None
        self.cname = None       # C function name, filled in by the merge pass
        proc = ci["proc"]
        if int(proc) == 0:
            # C method installed by mrb_define_method(): no RProc is allocated
            self.kind = "cfunc"
        elif int(proc["flags"]) & MRB_PROC_CFUNC_FL:
            self.kind = "cfunc"
            self.cname = str(proc["body"]["func"]).split("<")[-1].rstrip(">")
        else:
            self.kind = "irep"
            self.irep = proc["body"]["irep"]
            if int(self.irep) and int(ci["pc"]):
                # ci->pc already points past the instruction being executed
                self.idx = int(ci["pc"] - self.irep["iseq"]) - 1
                self.pos = position(mrb, self.irep, self.idx)

    @property
    def entry(self):
        """True when a (nested) mrb_vm_exec was started for this frame."""
        return self.cci in (CINFO_SKIP, CINFO_RESUMED)

    def where(self):
        if self.kind == "cfunc":
            return "<cfunc %s>" % (self.cname or "?")
        if self.pos:
            return "%s:%d" % (shorten(self.pos[0]), self.pos[1])
        return "<no debug info>" if int(self.irep or 0) else "<no irep>"

    def label(self):
        if self.mid:
            return self.mid + (" [fiber]" if self.cci == CINFO_RESUMED else "")
        return "(toplevel)" if self.index == 0 else "(no mid)"


def collect_vm_frames(mrb, ctx=None):
    c = ctx if ctx is not None else mrb["c"]
    if not int(c) or not int(c["cibase"]):
        return []
    depth = int(c["ci"] - c["cibase"])
    return [VMFrame(mrb, (c["cibase"] + i).dereference(), i)
            for i in range(depth, -1, -1)]


def collect_c_frames():
    out, f, i = [], gdb.newest_frame(), 0
    while f:
        sal = f.find_sal()
        loc = "%s:%d" % (shorten(sal.symtab.filename), sal.line) \
            if sal and sal.symtab else ""
        out.append((i, f.name() or "??", loc))
        f = f.older()
        i += 1
    return out


def next_vm_exec(c_frames, start):
    for k in range(start, len(c_frames)):
        if c_frames[k][1] == VM_EXEC:
            return k
    return None


def c_line(fr, note=""):
    return ("  C   #%-3d %-26s %-28s%s" % (fr[0], fr[1], fr[2], note)).rstrip()


class MrbBacktrace(gdb.Command):
    """mrbbt [mrb-expr] - unified C + mruby VM backtrace, most recent call first."""

    def __init__(self):
        super(MrbBacktrace, self).__init__("mrbbt", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        mrb = find_mrb(arg.strip())
        ctx = mrb["c"]
        vm = collect_vm_frames(mrb, ctx)
        cf = collect_c_frames()
        i = 0        # cursor into the C frames, innermost first
        folded = 0   # Ruby frames seen since the last mrb_vm_exec boundary

        print("=== unified backtrace: %d VM frames over %d C frames ==="
              % (len(vm), len(cf)))
        for f in vm:
            if f.kind == "cfunc":
                # C frames up to (excluding) the enclosing mrb_vm_exec are this
                # C method's body plus the bridge (mrb_yield/mrb_funcall/...)
                stop = next_vm_exec(cf, i)
                stop = len(cf) if stop is None else stop
                for fr in cf[i:stop]:
                    print(c_line(fr))
                if stop > i:
                    f.cname = f.cname or cf[stop - 1][1]
                i = stop
                print("  VM  ci[%-2d] %-22s %-30s cci=%s"
                      % (f.index, f.label(), f.where(), CCI_NAME.get(f.cci, "?")))
                continue

            host = next_vm_exec(cf, i)
            folded += 1
            print("  VM  ci[%-2d] %-22s %-30s cci=%-6s %s"
                  % (f.index, f.label(), f.where(), CCI_NAME.get(f.cci, "?"),
                     "in C #%d" % cf[host][0] if host is not None else "(no exec frame)"))
            if (f.entry or f.index == 0) and host is not None:
                print(c_line(cf[host],
                             "  <- %d Ruby frame(s) folded into this C frame" % folded))
                i, folded = host + 1, 0
        for fr in cf[i:]:
            print(c_line(fr))
        if int(ctx["prev"]):
            print("  ... suspended context(s) below: mrb->c->prev = %s "
                  "(use `mrbci mrb->c->prev`)" % ctx["prev"])


class MrbCallinfo(gdb.Command):
    """mrbci [ctx-expr] - raw mrb_callinfo dump (default: mrb->c)."""

    def __init__(self):
        super(MrbCallinfo, self).__init__("mrbci", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        arg = arg.strip()
        mrb = find_mrb("")
        ctx = gdb.parse_and_eval(arg) if arg else mrb["c"]
        print("context %s  stack=%s..%s  ci=%s..%s"
              % (ctx, ctx["stbase"], ctx["stend"], ctx["cibase"], ctx["ci"]))
        for f in collect_vm_frames(mrb, ctx):
            print("  ci[%-2d] cci=%-7s %-5s %-20s %-30s iseq_idx=%s"
                  % (f.index, CCI_NAME.get(f.cci, "?"), f.kind, f.label(),
                     f.where(), f.idx if f.idx is not None else "-"))


MrbBacktrace()
MrbCallinfo()
