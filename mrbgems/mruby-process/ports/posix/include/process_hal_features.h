/*
** process_hal_features.h - what the POSIX port of mruby-process implements
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

/* waitpid(2): `Process.wait`, `Process.waitpid`, `Process.wait2` and
   `Process.waitpid2`. */
#define MRB_HAL_PROCESS_HAS_WAIT

/* getrlimit(2) and setrlimit(2): `Process.getrlimit` and `Process.setrlimit`.
   Both are XSI extensions rather than base POSIX, as the <sys/resource.h>
   they are declared in is, so mrbgem.rake asks the compiler and the linker
   for each and answers here as HAVE_*, one a call as CRuby's configure has
   them.  Asked one at a time, again as CRuby asks, so that a host with the
   reader and not the writer keeps the reader. */
#ifdef HAVE_GETRLIMIT
# define MRB_HAL_PROCESS_HAS_GETRLIMIT
#endif
#ifdef HAVE_SETRLIMIT
# define MRB_HAL_PROCESS_HAS_SETRLIMIT
#endif

#endif /* MRUBY_PROCESS_HAL_FEATURES_H */
