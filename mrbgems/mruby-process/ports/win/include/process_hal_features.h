/*
** process_hal_features.h - what the Windows port of mruby-process implements
**
** See Copyright Notice in mruby.h
**
** The gem's include/process_hal.h reads this before it declares anything.  A macro defined
** here guards three things at once: the prototype there, the implementation
** in process_hal.c, and the method definition under src/.  A port that declared a
** capability and did not implement it would fail to link, and one that
** declares nothing owes nothing.
*/

#ifndef MRUBY_PROCESS_HAL_FEATURES_H
#define MRUBY_PROCESS_HAL_FEATURES_H

/* No wait.  Win32 waits on a handle, and a handle is got by opening a
   process ID, which succeeds for any process this one may open and says
   nothing about parentage: waiting on one would report a stranger's exit
   code as a child's.  A port learns of its children when it creates them,
   so `Process.wait` and its three other spellings are answerable once
   spawn exists and not before. */

/* No resource limits.  Win32 has no rlimit: what it limits, it limits per
   job object rather than per process, and a job is a different thing with
   different rules for who may set one.  The C runtime's `_setmaxstdio` is
   the one neighbour, and it moves a limit on the CRT's own stdio table
   rather than on the process, so answering `Process.getrlimit(:NOFILE)`
   from it would report something else under the name.  `Process.getrlimit`
   and `Process.setrlimit` are therefore unimplemented here rather than
   emulated. */

#endif /* MRUBY_PROCESS_HAL_FEATURES_H */
