/*
** process_hal_features.h - what the Windows port of mruby-process implements
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

/* CreateProcessW() is there on every Windows this port runs on. */
#define MRB_HAL_PROCESS_HAS_SPAWN

/* CREATE_NEW_PROCESS_GROUP is what Windows has in place of setpgid(2), and
   `new_pgroup` is the option CRuby gives it there.  A process group to
   join, an identity and resource limits are not things a Windows process
   can be created with, so those options do not exist on this port, as they
   do not in CRuby's Windows build.  Nor is a umask: the C runtime's is
   state of this process that no child inherits, so the option CRuby's
   Windows build accepts and cannot pass on is refused here. */
#define MRB_HAL_PROCESS_HAS_NEW_PGROUP

#endif /* MRUBY_PROCESS_HAL_FEATURES_H */
