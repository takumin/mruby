/*
** process_hal_features.h - what the POSIX port of mruby-process implements
**
** See Copyright Notice in mruby.h
**
** include/process_hal.h reads this before it declares anything.  A macro
** defined here guards three things at once: the prototype there, the
** implementation in process_hal.c, and the method definition under src/.
** A port that declared a capability and did not implement it would fail to
** link, and one that declares nothing owes nothing.
*/

#ifndef MRUBY_PROCESS_HAL_FEATURES_H
#define MRUBY_PROCESS_HAL_FEATURES_H

/* A process may create another everywhere this port runs but iOS, where the
   platform lets no process do so whatever the configuration asks for. */
#if defined(__APPLE__)
# include <TargetConditionals.h>
#endif
#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
# define MRB_HAL_PROCESS_HAS_SPAWN
#endif

#ifdef MRB_HAL_PROCESS_HAS_SPAWN
/* What a child of this port can be started with beyond its arguments,
   environment and descriptors.  Each is an option of Process.spawn that is
   refused as one that does not exist where the macro is not defined, which
   is what CRuby does on a platform without the call behind it. */

/* setpgid(2) is POSIX, and posix_spawn(3) carries the same request as
   POSIX_SPAWN_SETPGROUP. */
# define MRB_HAL_PROCESS_HAS_PGROUP

/* umask(2), setuid(2) and setgid(2) are POSIX.  Each is a call only the
   child can make, so a spawn given one takes the fork path. */
# define MRB_HAL_PROCESS_HAS_UMASK
# define MRB_HAL_PROCESS_HAS_UID

/* setrlimit(2) is XSI rather than base POSIX, and <sys/resource.h> is where
   it and the resource names are declared.  Whether a target has the header
   is asked of the compiler by mrbgem.rake, as it is for Process.times. */
# ifdef HAVE_SYS_RESOURCE_H
#  define MRB_HAL_PROCESS_HAS_RLIMIT
# endif
#endif

#endif /* MRUBY_PROCESS_HAL_FEATURES_H */
