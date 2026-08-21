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

/* Who owes the reap.  A wait *event* is not one of these: a child that
   stopped is still LIVE and still owes one. */
typedef enum mrb_process_lifecycle {
  MRB_PROCESS_LIVE     = 0,  /* holds the platform resource and owes a reap */
  MRB_PROCESS_REAPED   = 1,  /* a terminal event was observed; status is kept */
  MRB_PROCESS_DETACHED = 2,  /* the obligation was let go of without one */
} mrb_process_lifecycle;

typedef struct mrb_process_record mrb_process_record;
typedef struct mrb_process_table mrb_process_table;

struct mrb_process_record {
  mrb_int pid;                        /* label; meaningful while LIVE */
  mrb_process_lifecycle lifecycle;
  mrb_bool has_status;
  mrb_process_status status;          /* the terminal status, once REAPED */
  mrb_hal_process_child *hal_child;   /* NULL once released */
  mrb_process_table *table;           /* NULL once out of LIVE */
  mrb_process_record *next;           /* the table's live list, in creation order */
  unsigned int refc;                  /* the table's reference, plus each Process::Child's */
};

/*
 * The table of children this interpreter owns
 *
 * It holds the LIVE records, in creation order, and the port context they
 * were spawned into.  A record that leaves LIVE leaves the table with it, so
 * the list is the live index: a pid found here is a child that still owes a
 * reap, and a pid that is not is not this interpreter's to wait on.
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

void mrb_process_record_ref(mrb_process_record *record);
void mrb_process_record_unref(mrb_state *mrb, mrb_process_record *record);

/* Wait on one child.  Returns a Process::Status, or nil when
   MRB_PROCESS_WAIT_NOHANG found nothing; sets `$?` either way.  Idempotent:
   a record already REAPED answers from its stored status without a system
   call, which is what makes reaching the same child twice harmless. */
mrb_value mrb_process_record_wait(mrb_state *mrb, mrb_process_record *record,
                                  unsigned int flags);

/* Wait on every live child at once, for whichever produces an event first. */
mrb_value mrb_process_wait_any(mrb_state *mrb, unsigned int flags);

/* Let go of a live child without waiting for it. */
void mrb_process_record_detach(mrb_state *mrb, mrb_process_record *record);

/*
 * Process::Child
 */

void mrb_process_child_init(mrb_state *mrb, struct RClass *process);
mrb_value mrb_process_child_new(mrb_state *mrb, mrb_process_record *record);

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

#endif /* MRUBY_PROCESS_INTERNAL_H */
