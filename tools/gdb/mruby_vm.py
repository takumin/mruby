"""GDB helpers to view the C stack and the mruby VM call-info stack together.

Usage (on a build made with `conf.enable_debug`):

    gdb -x tools/gdb/mruby_vm.py --args build/host/bin/mruby foo.rb
    (gdb) break mrb_exc_raise
    (gdb) run
    (gdb) mrbbt          # unified C + VM backtrace
    (gdb) mrbci          # raw call-info dump
    (gdb) mrbimpl String#[]   # what implements a method

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
MRB_PROC_ALIAS = 8192
MRB_METHOD_FUNC_FL = 1 << 24
VM_EXEC = "mrb_vm_exec"

# Methods the VM may answer inline, without consulting the method table.
INLINE_OPS = {
    "[]": "OP_GETIDX/OP_GETIDX0 (Array, Hash, String receivers)",
    "[]=": "OP_SETIDX (Array, Hash receivers)",
    "+": "OP_ADD/OP_ADDI", "-": "OP_SUB/OP_SUBI", "*": "OP_MUL", "/": "OP_DIV",
    "<": "OP_LT", "<=": "OP_LE", ">": "OP_GT", ">=": "OP_GE", "==": "OP_EQ",
}


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
            if host is not None and host > i:
                # C called straight from the dispatch loop: an inline opcode
                # (OP_GETIDX, OP_ADD, ...) or a VM helper. No call-info frame
                # is pushed for these, so they exist only on the C stack.
                for fr in cf[i:host]:
                    print(c_line(fr, "  <- inline from the VM (no call-info frame)"))
                i = host
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


def symbol_of(value):
    """Render a function pointer as `name` when GDB can resolve it."""
    text = str(value)
    return text.split("<", 1)[1].rstrip(">") if "<" in text else text


class MrbImpl(gdb.Command):
    """mrbimpl Class#method | Class.method - show the code implementing a method.

    Resolves the method the way the VM does (`mrb_method_search_vm`) and reports
    whether it lands on a C function, a cfunc-backed proc, or Ruby bytecode.
    Requires a live inferior; the class must already be defined at this point.
    """

    def __init__(self):
        super(MrbImpl, self).__init__("mrbimpl", gdb.COMMAND_DATA)

    @staticmethod
    def class_name(mrb, cls):
        """Name of an RClass; singleton classes have no path, so fall back."""
        try:
            name = gdb.parse_and_eval("mrb_class_name(%d, (struct RClass*)%d)"
                                      % (int(mrb), cls))
            if int(name):
                return name.string()
        except gdb.error:
            pass
        return "0x%x" % cls

    @staticmethod
    def parse(spec):
        for sep, singleton in (("#", False), (".", True)):
            if sep in spec:
                cls, _, meth = spec.partition(sep)
                return cls.strip(), meth.strip(), singleton
        parts = spec.split(None, 1)
        if len(parts) != 2:
            raise gdb.GdbError("usage: mrbimpl Class#method (or Class.method)")
        return parts[0], parts[1].strip(), False

    def invoke(self, arg, from_tty):
        mrb = find_mrb("")
        cls_name, meth, singleton = self.parse(arg.strip())

        if not int(gdb.parse_and_eval('mrb_class_defined(%d, "%s")' % (int(mrb), cls_name))):
            raise gdb.GdbError("class %s is not defined (yet) in this process" % cls_name)
        cls = gdb.parse_and_eval('mrb_class_get(%d, "%s")' % (int(mrb), cls_name))
        start = cls["c"] if singleton else cls
        sym = int(gdb.parse_and_eval('mrb_intern_cstr(%d, "%s")' % (int(mrb), meth)))

        # mrb_method_search_vm() needs an in/out RClass**; borrow inferior memory.
        # Everything read out of it must be turned into Python values before the
        # free(), because gdb.Value reads memory lazily.
        cp = gdb.parse_and_eval("(struct RClass**)malloc(sizeof(void*))")
        try:
            gdb.parse_and_eval("*(struct RClass**)%d = (struct RClass*)%d"
                               % (int(cp), int(start)))
            m = gdb.parse_and_eval("mrb_method_search_vm(%d, (struct RClass**)%d, %d)"
                                   % (int(mrb), int(cp), sym))
            owner = int(gdb.parse_and_eval("*(struct RClass**)%d" % int(cp)))
            flags = int(m["flags"])
            target = int(m["as"]["proc"])
            func = symbol_of(m["as"]["func"]) if flags & MRB_METHOD_FUNC_FL else None
            aliased = None
            if target and not func:
                proc = m["as"]["proc"]
                if int(proc["flags"]) & MRB_PROC_ALIAS:
                    aliased = sym_name(mrb, proc["body"]["mid"])
                    proc = proc["upper"]      # the alias target carries the code
                pflags = int(proc["flags"]) if int(proc) else 0
                cfunc = symbol_of(proc["body"]["func"]) \
                    if pflags & MRB_PROC_CFUNC_FL else None
                irep = int(proc["body"]["irep"]) if int(proc) and not cfunc else 0
        finally:
            gdb.parse_and_eval("(void)free((void*)%d)" % int(cp))

        print("%s%s%s" % (cls_name, "." if singleton else "#", meth))
        if target == 0:
            print("  undefined")
            return
        print("  defined in: %s" % self.class_name(mrb, owner))
        if aliased:
            print("  alias of: %s" % aliased)

        if func:
            print("  C function: %s" % func)
        elif cfunc:
            print("  C function: %s (via RProc 0x%x)" % (cfunc, target))
        else:
            pos = position(mrb, irep, 0) if irep else None
            print("  Ruby method: %s (irep 0x%x)"
                  % ("%s:%d" % (shorten(pos[0]), pos[1]) if pos else "<no debug info>",
                     irep))
        if meth in INLINE_OPS:
            print("  note: the VM may answer this inline via %s, bypassing this\n"
                  "        entry entirely - break on the C function to be sure"
                  % INLINE_OPS[meth])


MrbBacktrace()
MrbCallinfo()
MrbImpl()
