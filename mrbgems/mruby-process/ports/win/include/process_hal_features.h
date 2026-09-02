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

/* WaitForMultipleObjects(): `Process.wait`, `Process.waitpid`,
   `Process.wait2` and `Process.waitpid2`.  Win32 waits on a handle, and the
   only handles this port has are the ones it opened by spawning, so a wait
   here answers for a child this interpreter created and for nothing else: a
   handle got by opening a process ID stands for any process the caller may
   open and says nothing about parentage. */
#define MRB_HAL_PROCESS_HAS_WAIT

/* CreateProcessW(): `Process.spawn`.  It is there on every Windows this port
   runs on. */
#define MRB_HAL_PROCESS_HAS_SPAWN

#endif /* MRUBY_PROCESS_HAL_FEATURES_H */
