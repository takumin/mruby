<!-- summary: Virtual Machine Internals -->

# Virtual Machine Internals

This document describes mruby's virtual machine for developers
working on `src/vm.c` and related code.

**Read this if you are:** debugging method dispatch or call frame
issues, working on exception handling, implementing new opcodes,
modifying fiber/coroutine behavior, or optimizing the dispatch loop.

For the instruction set, see [opcode.md](opcode.md). For the
compiler that generates bytecode, see [compiler.md](compiler.md).

## Execution Model

mruby uses a **register-based VM**. Local variables and temporaries
occupy fixed register slots determined at compile time. Each method
call gets its own register window on a shared value stack.

## Execution Context

The VM state is stored in `mrb_context`:

```text
mrb_context
+-- prev             previous context (fiber chain)
+-- stbase..stend    value stack (mrb_value[])
+-- ci               current call frame pointer
+-- cibase..ciend    call info stack (mrb_callinfo[])
+-- status           fiber state (4 bits, enum mrb_fiber_state)
+-- vmexec           context runs in a nested mrb_vm_exec from C
+-- fib              owning RFiber (NULL for the root context)
```

The value stack and call info stack grow independently. Each fiber
has its own `mrb_context`.

### Stack Sizing

- Initial value stack: 128 entries (`STACK_INIT_SIZE`)
- Initial call info stack: 32 entries (`CALLINFO_INIT_SIZE`)
- Growth factor: 1.5x (or 2x with `MRB_STACK_EXTEND_DOUBLING`)
- Minimum growth: 128 entries (`MRB_STACK_GROWTH`)
- Max stack depth: `MRB_STACK_MAX` (0x40000 - 128)
- Max call depth: `MRB_CALL_LEVEL_MAX` (512, or 128 with ASAN)

Exceeding either limit raises `SystemStackError`.

When the value stack is reallocated, all `REnv` objects and
`mrb_callinfo` stack pointers are adjusted by the delta
(`envadjust` function).

## Call Frames

Each method or block call pushes a `mrb_callinfo` frame:

```text
mrb_callinfo
+-- n:4          positional argument count (0-14, 15 = varargs)
+-- nk:4         keyword argument count (0-14, 15 = varargs)
+-- cci          call context info (NONE, SKIP, DIRECT, RESUMED)
+-- vis          packed flags: 4(zero):1(module_function)
|                :1(separate module):2(method visibility)
+-- mid          method symbol
+-- proc         current RProc
+-- blk          block argument (RProc*)
+-- stack        pointer into value stack
+-- pc           program counter (bytecode position)
+-- u.env        closure environment (REnv*)
+-- u.target_class  class the method was found in
+-- u.keep_context  NULL marks a fiber switch (internal use)
```

The low 3 bits of `vis` are copied into the frame's `REnv` when one
is created; from that point the env's copy takes precedence
(`MRB_ENV_COPY_FLAGS_FROM_CI`).

### Stack Layout Per Frame

```text
ci->stack:
  [0]      self (receiver)
  [1..n]   positional arguments
  [n+1..]  keyword argument pairs (key, value, key, value, ...)
  [bidx]   block argument
  [bidx+1..] local variables and temporaries
```

### Argument Count Encoding

The `n` and `nk` fields are 4 bits each (0-15). When `n == 15`,
positional arguments are packed into a single Array in register 1.
When `nk == 15`, keyword arguments are packed into a single Hash.

The block index is calculated by `mrb_bidx(n, nk)`:

```text
if n == 15: n = 1 (array)
if nk == 15: n += 1 (hash)
else: n += nk * 2 (key-value pairs)
return n + 1 (skip self)
```

### Call Context Info (cci)

| Value | Name            | Meaning                                   |
| ----- | --------------- | ----------------------------------------- |
| 0     | `CINFO_NONE`    | Called from the VM, without a C boundary  |
| 1     | `CINFO_SKIP`    | The VM was ignited from C (entry frame)   |
| 2     | `CINFO_DIRECT`  | Method called from C (`mrb_funcall` etc.) |
| 3     | `CINFO_RESUMED` | Resumed by `Fiber.yield` / fiber resume   |

`cci` records how the frame was entered, which decides how a return
or an exception leaves it. A `CINFO_NONE` frame returns inside the
dispatch loop; `CINFO_SKIP` and `CINFO_DIRECT` frames return out of
`mrb_vm_exec()` to their C caller.

Note that `OP_SEND` pushes its frame as `CINFO_DIRECT` and only
resets `cci` to `CINFO_NONE` after method lookup and the visibility
check have finished, so the frame is not treated as a VM-to-VM call
while it is still being set up.

## Dispatch Loop

The main loop lives in `mrb_vm_exec()`, which decodes and dispatches
opcodes. `mrb_vm_run()` is a thin wrapper that prepares the stack
(`self`, `stack_keep`, `irep->nregs`) and then calls it; unlike
`mrb_vm_exec()`, it asserts that the context is not switched.

Two dispatch strategies are available:

- **Computed goto** (default on GCC/Clang): a jump table of label
  addresses (`optable[]`) for direct dispatch. Faster due to
  better branch prediction.
- **Switch-based** (`MRB_USE_VM_SWITCH_DISPATCH`): a standard
  `switch(insn)` statement. Default on MSVC and other compilers.

Both strategies advance through `CALL_CODE_HOOKS()`, which fetches
the next opcode and runs `CODE_FETCH_HOOK` (used by the debugger
gem).

The dispatch loop is wrapped in `MRB_TRY`/`MRB_CATCH` for exception
handling (see [Exception Handling](#exception-handling)).

## Method Dispatch

When `OP_SEND` (or `OP_SSEND`, `OP_SUPER`) executes:

### 1. Prepare Arguments

Determine argument layout. When the operand `c` is below
`CALL_MAXARGS` (15) it is a plain positional count and the fast path
uses inline registers directly. Otherwise `c` is read as
`n | (nk << 4)`: the compiler has already packed positionals into an
Array when `n == 15`, and `OP_SEND` packs any loose keyword
arguments into a Hash here, recomputing the block index.

### 2. Push Call Frame

```c
ci = cipush(mrb, a, CINFO_DIRECT, NULL, NULL, BLK_PTR(blk), 0, c);
```

The new frame's stack starts at the previous frame's stack + `a`
(the receiver's register index). `mid` is filled in after lookup
succeeds, so that `method_missing` handling can substitute its own.

### 3. Method Lookup

`mrb_vm_find_method()` performs the lookup:

1. **Method cache check**: a 2-way set-associative cache indexed by
   `hash((class >> 4) ^ mid)`. `MRB_METHOD_CACHE_SIZE` (default 256)
   counts entries, so there are `MRB_METHOD_CACHE_SIZE / 2` sets of
   two ways. Each entry also caches `c0`, the class the method was
   actually found in, so the walk result is reused as well.
2. **Method table walk**: if the cache misses, search the receiver's
   class method table (`mt`), then walk the superclass chain.
3. **Cache store**: on successful lookup, store in the cache.

The cache is invalidated per class (`mrb_mc_clear_by_class`, also
called from GC when a class is swept) and per method id
(`mc_clear_by_id`, on define/alias/undef). Building with
`MRB_NO_METHOD_CACHE` compiles the cache out entirely.

If lookup fails, `prepare_missing()` rewrites the frame to call
`method_missing` (or raises `NoMethodError` when that is undefined
too).

### 4. Visibility Check

Visibility is checked only when the method is found; the
`method_missing` fallback dispatches regardless of its own
visibility, as in CRuby. Only `OP_SEND`/`OP_SEND0`/`OP_SENDB` are
checked — `OP_SSEND*` is a self call and skips the check.

Private methods are only callable without an explicit receiver.
Protected methods are callable when the _caller's_ `self` is a kind
of the class the method was found in. Violations go through
`vis_error()`, which raises `NoMethodError`.

### 5. Invoke

- **Ruby method** (irep-based): resolve aliases
  (`MRB_PROC_RESOLVE_ALIAS`), extend the stack to `irep->nregs`
  (minimum 4), set `ci->pc` to `irep->iseq`, and jump to the new
  bytecode.
- **C function**: call `func(mrb, recv)` directly, then shrink the
  GC arena, pop the call frame, and store the return value.

#### Attribute Accessor Fast Path

`attr_reader`/`attr_writer` methods are cfuncs holding the ivar name
in `MRB_PROC_ENV(p)->stack[0]`. When such a method is called with no
block and no keyword arguments, the VM reads or writes the instance
variable in place and pops the frame without a real cfunc call.

The fast path is restricted to cases that cannot raise: reads never
do, and writes are limited to unfrozen `MRB_TT_OBJECT` receivers.
Arity mismatches and frozen receivers fall back to the normal call.

## Exception Handling

### setjmp/longjmp

By default, mruby uses `setjmp`/`longjmp` for exception control
flow:

```c
MRB_TRY(&c_jmp) {
  mrb->jmp = &c_jmp;
  /* dispatch loop */
}
MRB_CATCH(&c_jmp) {
  /* handle exception */
}
MRB_END_EXC(&c_jmp);
```

With `MRB_USE_CXX_EXCEPTION`, C++ `try`/`catch` is used instead.

### Handler Table

Each irep contains a catch handler table (appended after iseq in
memory) with entries for `rescue` and `ensure` blocks:

```text
mrb_irep_catch_handler
+-- type       RESCUE (0) or ENSURE (1)
+-- begin[4]   start PC of protected range
+-- end[4]     end PC of protected range
+-- target[4]  jump target when handler matches
```

### Unwinding Process

When an exception occurs, the VM jumps to `L_RAISE`:

1. Search the current irep's catch handler table
   (`catch_handler_find`, linear scan in reverse order, so the
   innermost matching handler wins) for a handler covering the
   current PC
2. If a handler is found: extend the stack to `irep->nregs` and set
   `ci->pc` to the handler's target, which runs the `rescue` or
   `ensure` body (an `ensure` body may re-raise)
3. If no handler is found: pop the call frame (`cipop`) and repeat
   with the parent frame. Frames whose `proc` is a cfunc, or whose
   irep has no catch table (`clen < 1`), are skipped outright
4. If the popped frame was `CINFO_SKIP`, the VM was ignited from C
   at that point: restore `mrb->jmp` and `MRB_THROW(prev_jmp)` to
   hand the exception to the C caller
5. At `cibase` of the root context, return the exception object.
   At `cibase` of a fiber, terminate the fiber and continue
   unwinding in the parent context

`CINFO_DIRECT` frames are destroyed as a group in `cipop`'s fiber
path, when the context has switched out from under them.

Non-local exits (`break`, `return` from a block) reuse the same
machinery through `RBreak` objects and the `CHECKPOINT_RESTORE` /
`CHECKPOINT_MAIN` / `UNWIND_ENSURE` macros, so that `ensure` blocks
still run on the way out.

## Block and Closure Handling

### REnv (Environment)

Closures capture their enclosing scope's variables through `REnv`:

```text
REnv
+-- stack      pointer to captured variable values
+-- cxt        owning context (NULL if detached from stack)
+-- mid        method symbol
+-- flags      20 bits: 1(zero):1(separate module):2(visibility)
               :8(cioff/bidx):8(stack_len)
```

Bits 16-19 (visibility, visibility-break, module_function) are
copied from the owning `mrb_callinfo::vis` when the env is created.

While the defining scope is active, `REnv::stack` points directly
into the VM value stack (shared). This avoids copying.

### Environment Unsharing

When a closure outlives its defining scope, `mrb_env_unshare()`
copies the captured variables from the stack to a heap-allocated
buffer:

```c
mrb_env_unshare(mrb, env, noraise);
```

After unsharing, `MRB_ENV_CLOSE(env)` sets `cxt = NULL` to indicate
the environment is detached. A write barrier is issued for GC
correctness.

### Proc Types

| Flag                | Value | Meaning                             |
| ------------------- | ----- | ----------------------------------- |
| `MRB_PROC_CFUNC_FL` | 128   | C function (not irep-based)         |
| `MRB_PROC_STRICT`   | 256   | Lambda (strict argument check)      |
| `MRB_PROC_ORPHAN`   | 512   | Outlived its yielding frame         |
| `MRB_PROC_ENVSET`   | 1024  | `e` holds an `REnv`, not a class    |
| `MRB_PROC_SCOPE`    | 2048  | Defines a new variable scope        |
| `MRB_PROC_NOARG`    | 4096  | cfunc declared `MRB_ARGS_NONE()`    |
| `MRB_PROC_ALIAS`    | 8192  | `body.mid` names the aliased method |

`MRB_PROC_ORPHAN` is set when a block is captured past the lifetime
of the method that yielded it — by `Proc.new`, by `Proc#dup`, or by
`cipop` when the yielding frame is popped. `OP_BREAK` raises
`LocalJumpError` for an orphan proc instead of unwinding.

## Fiber Switching

Fibers are lightweight coroutines. Each fiber has its own
`mrb_context` with separate value and call info stacks.

### Fiber States

`enum mrb_fiber_state` has six values:

```text
CREATED --> RUNNING --> SUSPENDED --> TERMINATED
                |           ^
                +-----------+
                  (yield/resume)

RESUMED     (a fiber that resumed another fiber)
TRANSFERRED (entered via Fiber#transfer)
```

`RESUMED` marks the _resuming_ side of `Fiber#resume`: that context
is neither running nor suspended, and resuming it again raises
`FiberError`.

### Context Switch

On `Fiber#resume`:

1. Save current context state
2. Set `mrb->c` to the fiber's context
3. Push arguments onto the fiber's stack
4. Continue execution in the fiber

On `Fiber.yield`:

1. Save fiber context
2. Restore the parent context (`mrb->c = c->prev`)
3. Return yield values to the parent

### Fiber Termination

When a fiber completes (`fiber_terminate` in `src/vm.c`):

1. Set status to `TERMINATED`
2. Free the call info stack and clear `cibase`/`ciend`/`ci`
3. Hand off the value stack: if no env references it, free it.
   Otherwise the buffer is `realloc`'d down to `MRB_ENV_LEN(env)`
   and adopted by the env, which is then closed (a write barrier is
   issued first)
4. Switch to the previous context

The fiber's `mrb_context` itself is not freed here — it is reclaimed
by GC along with the `RFiber` object.

### C Function Boundary

Fibers cannot yield across C function boundaries. You cannot call
`Fiber.yield` from within a C-implemented method (except via
`mrb_fiber_yield` at return). This is because C call frames cannot
be suspended and resumed.

## GC Integration

The VM saves the arena index at the start of the dispatch loop:

```c
int ai = mrb_gc_arena_save(mrb);
```

After each C function call, the arena is shrunk back:

```c
mrb_gc_arena_shrink(mrb, ai);
```

This prevents temporary objects created by C functions from
accumulating in the arena. Return paths use
`mrb_gc_arena_restore(mrb, ai)` instead, since the frame is going
away. Return values that are not immediates are passed to
`mrb_gc_protect()` before `cipop`, because popping a frame can
allocate (env unsharing) and the popped frame's slots are no longer
scanned.

Write barriers are issued when environments are detached or closed,
ensuring the incremental GC correctly tracks live references.

## Source Files

| File                       | Contents                                                               |
| -------------------------- | ---------------------------------------------------------------------- |
| `src/vm.c`                 | Dispatch loop, method invocation, stack/frame management (~4000 lines) |
| `src/class.c`              | `mrb_vm_find_method`, method cache                                     |
| `src/proc.c`               | `RProc` construction, orphan handling                                  |
| `include/mruby.h`          | `mrb_state`, `mrb_callinfo`, `mrb_context`                             |
| `include/mruby/proc.h`     | `RProc`, `REnv` structures and flags                                   |
| `include/mruby/irep.h`     | `mrb_irep`, catch handler table                                        |
| `include/mruby/throw.h`    | `MRB_TRY`/`MRB_CATCH` macros                                           |
| `mrbgems/mruby-fiber/src/` | `Fiber` methods and context switching                                  |
