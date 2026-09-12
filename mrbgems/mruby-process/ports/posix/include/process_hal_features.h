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
**
** Every `Process::Sys` method is defined whatever this header says.  What a
** macro decides is whether the method has a body or is the mark of an
** unimplemented one, which answers `respond_to?` with false and raises
** NotImplementedError, as CRuby's own unimplemented methods do.
**
** One macro a call, one more for reading a name, and one platform named,
** to take an answer back.  Which of the `Process::Sys` calls a host has is
** asked of its compiler and linker by mrbgem.rake, whether <unistd.h>
** declares the call and the C library defines it (check_func), and answered
** here as HAVE_*, one a call as CRuby's configure has them.
** The ten POSIX names are on every host this port builds on; the rest are
** not a system's to name.  setruid(2) and setrgid(2) are 4.2BSD's, kept by
** Darwin, FreeBSD and DragonFly, never in glibc, removed by OpenBSD and
** slated for removal by NetBSD.  setresuid(2) and setresgid(2) began on
** HP-UX, Linux and most of the BSDs took them up and POSIX.1-2024 made them
** XSI, and neither macOS nor NetBSD has them, so a list of systems could
** only fall behind the standard.  issetugid(2) is a C library's call rather
** than an operating system's: the BSDs, Darwin and Solaris have it, glibc
** never has, and musl declares it on the same Linux, so a name would not
** tell one build from the other.  Reading the answer rather than inferring
** it is what keeps both mistakes out: a false negative costs a method, and
** a false positive costs a link error.
*/

#ifndef MRUBY_PROCESS_HAL_FEATURES_H
#define MRUBY_PROCESS_HAL_FEATURES_H

/* waitpid(2): `Process.wait`, `Process.waitpid`, `Process.wait2` and
   `Process.waitpid2`. */
#define MRB_HAL_PROCESS_HAS_WAIT

/* getuid(2), geteuid(2), getgid(2) and getegid(2): `Process::Sys.getuid`,
   `.geteuid`, `.getgid` and `.getegid`. */
#ifdef HAVE_GETUID
# define MRB_HAL_PROCESS_HAS_GETUID
#endif
#ifdef HAVE_GETEUID
# define MRB_HAL_PROCESS_HAS_GETEUID
#endif
#ifdef HAVE_GETGID
# define MRB_HAL_PROCESS_HAS_GETGID
#endif
#ifdef HAVE_GETEGID
# define MRB_HAL_PROCESS_HAS_GETEGID
#endif

/* NetBSD still links setruid(2) and setrgid(2), so the probe finds them,
   but its manual has them deprecated and slated for removal, and CRuby's
   process.c takes its configure's answer back on that one system.  The
   port does the same, so `Process::Sys.setruid` is unimplemented there
   as CRuby's is. */
#ifdef __NetBSD__
# undef HAVE_SETRUID
# undef HAVE_SETRGID
#endif

/* setuid(2), seteuid(2), setruid(2), setgid(2), setegid(2) and setrgid(2):
   `Process::Sys.setuid`, `.seteuid`, `.setruid`, `.setgid`, `.setegid` and
   `.setrgid`.  A port that declares any call taking an ID also says which
   numbers name one; whether it reads a name is declared at the end. */
#ifdef HAVE_SETUID
# define MRB_HAL_PROCESS_HAS_SETUID
#endif
#ifdef HAVE_SETEUID
# define MRB_HAL_PROCESS_HAS_SETEUID
#endif
#ifdef HAVE_SETRUID
# define MRB_HAL_PROCESS_HAS_SETRUID
#endif
#ifdef HAVE_SETGID
# define MRB_HAL_PROCESS_HAS_SETGID
#endif
#ifdef HAVE_SETEGID
# define MRB_HAL_PROCESS_HAS_SETEGID
#endif
#ifdef HAVE_SETRGID
# define MRB_HAL_PROCESS_HAS_SETRGID
#endif

/* setreuid(2) and setregid(2): `Process::Sys.setreuid` and `.setregid`. */
#ifdef HAVE_SETREUID
# define MRB_HAL_PROCESS_HAS_SETREUID
#endif
#ifdef HAVE_SETREGID
# define MRB_HAL_PROCESS_HAS_SETREGID
#endif

/* setresuid(2) and setresgid(2): `Process::Sys.setresuid` and `.setresgid`.
   No host is known to have one of the two without the other, but the pair
   is asked about one at a time, as CRuby's configure asks. */
#ifdef HAVE_SETRESUID
# define MRB_HAL_PROCESS_HAS_SETRESUID
#endif
#ifdef HAVE_SETRESGID
# define MRB_HAL_PROCESS_HAS_SETRESGID
#endif

/* issetugid(2): `Process::Sys.issetugid`.  A glibc build is the one that
   finds the method missing. */
#ifdef HAVE_ISSETUGID
# define MRB_HAL_PROCESS_HAS_ISSETUGID
#endif

/* getpwnam_r(3) and getgrnam_r(3): a name in place of a number, in the
   `Process::Sys` methods that take a user ID and in those that take a group
   ID.  Not calls of their own but what the port reads names with, and one
   macro each, since the two read different tables and a host that keeps one
   need not keep the other; CRuby asks about <pwd.h> and <grp.h> the same way
   and separately.  Without the macro a method takes numbers alone and a name
   is a TypeError, as CRuby's built without <pwd.h> answers.  Every host with
   the setters has had the two since POSIX.1-2001, and they are asked about
   rather than assumed for the same reason the calls are. */
#ifdef HAVE_GETPWNAM_R
# define MRB_HAL_PROCESS_HAS_UID_BY_NAME
#endif
#ifdef HAVE_GETGRNAM_R
# define MRB_HAL_PROCESS_HAS_GID_BY_NAME
#endif

#endif /* MRUBY_PROCESS_HAL_FEATURES_H */
