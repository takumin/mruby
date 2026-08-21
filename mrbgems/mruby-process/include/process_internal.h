/*
** process_internal.h - shared declarations within mruby-process
**
** See Copyright Notice in mruby.h
**
** Not part of the HAL and not for other gems: these are the things
** process.c, spawn.c, child.c and status.c say to each other.
**
** The centre of it is the child record.  A pid is a label the OS may hand to
** another process once the first has been reaped, so it cannot be what a
** child *is*; the record is, and the record is what every wait goes through.
*/

#ifndef MRUBY_PROCESS_INTERNAL_H
#define MRUBY_PROCESS_INTERNAL_H

#include <mruby.h>
#include "process_hal.h"

MRB_BEGIN_DECL

/* Refuse `v` with a RangeError naming `what` unless it fits the `int` a port
   has to narrow it to.  A no-op where mruby's own Integer is no wider. */
mrb_int mrb_process_int_arg(mrb_state *mrb, mrb_int v, const char *what);

typedef struct mrb_process_record mrb_process_record;
typedef struct mrb_process_table mrb_process_table;

/* A record exists exactly while its child owes a reap, so there is no state
   to ask it about.  A wait that ends the child ends the record with it; a
   wait *event* that does not, such as a child that stopped, leaves both. */
struct mrb_process_record {
  mrb_int pid;                        /* the label the platform gave the child */
  mrb_hal_process_child *hal_child;   /* what the child actually is */
  mrb_process_table *table;
  mrb_process_record *next;           /* the table's list, in creation order */
};

/*
 * The table of children this interpreter owns
 *
 * It holds the records, in creation order, and the port context they were
 * spawned into.  A record ends by leaving the table, so the list is the
 * index: a pid found here is a child that still owes a reap, and a pid that
 * is not is not this interpreter's to wait on by identity.
 */

void mrb_process_table_init(mrb_state *mrb, struct RClass *process);
void mrb_process_table_final(mrb_state *mrb);
mrb_process_table *mrb_process_table_get(mrb_state *mrb);
mrb_hal_process_context *mrb_process_table_context(mrb_process_table *table);

/* Take ownership of a freshly spawned child.  The only way a record is made. */
mrb_process_record *mrb_process_record_register(mrb_state *mrb, mrb_process_table *table,
                                                mrb_hal_process_child *hal_child);

/* The one lookup, and it finds LIVE records only. */
mrb_process_record *mrb_process_record_find(mrb_process_table *table, mrb_int pid);

/* Wait on one child, by what it is rather than by the number labelling it.
   Returns a Process::Status, or nil when MRB_PROCESS_WAIT_NOHANG found
   nothing; sets `$?` either way.  A terminal event ends the record. */
mrb_value mrb_process_record_wait(mrb_state *mrb, mrb_process_record *record,
                                  unsigned int flags);

/* Wait on every live child at once, for whichever produces an event first. */
mrb_value mrb_process_wait_set(mrb_state *mrb,
                               const mrb_process_wait_target *target,
                               unsigned int flags);

/* Let go of a live child without waiting for it. */
void mrb_process_record_detach(mrb_state *mrb, mrb_process_record *record);

/*
 * Process::Status
 */

void mrb_process_status_init(mrb_state *mrb, struct RClass *process);

/* Build a Process::Status from a decoded status.  The status is a snapshot:
   it outlives the record it came from, and the port that produced it is the
   only thing that ever read the platform's bits. */
mrb_value mrb_process_status_new(mrb_state *mrb, const mrb_process_status *status);

/* The decoded status inside a Process::Status. */
const mrb_process_status *mrb_process_status_ptr(mrb_state *mrb, mrb_value status);

/*
 * Process.__spawn
 */

void mrb_process_spawn_init(mrb_state *mrb, struct RClass *process);

/* `$?`.  Written only by the layer that performs a wait. */
void mrb_process_set_last_status(mrb_state *mrb, mrb_value status);

/* Answer the clock reading `t` in the unit `unit` names.  `resolution` says
   whether `t` is a resolution rather than a moment, which is what makes
   `:hertz` a unit it can be asked for.  Shared with the gem's tests, which
   hand it readings no clock reports: the ends of an int64_t and of this
   build's Integer are decided here, so that is where they can be asked
   about. */
mrb_value mrb_process_clock_result(mrb_state *mrb, mrb_value unit,
                                   const mrb_process_clock_time *t,
                                   mrb_bool resolution);

MRB_END_DECL

#endif /* MRUBY_PROCESS_INTERNAL_H */
