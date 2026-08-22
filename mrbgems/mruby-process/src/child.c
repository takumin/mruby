/*
** child.c - the table of children this interpreter owes a wait for
**
** See Copyright Notice in mruby.h
**
** What a wait actually waits on.  The resource being owned is the exit
** status slot the OS keeps for a terminated child, together with the handle
** that keeps that slot readable on Windows.  A pid only labels that slot,
** and the OS is free to hand the label to another process once the slot is
** released, so the label cannot be the identity.  The record below is, and
** these invariants say what that buys:
**
**   - a record is created only by spawning (nothing adopts a pid it did not
**     create), and it is let go of at exactly one place, record_finish();
**   - the table holds every record and nothing else, so it is at once the
**     list teardown walks and the index a pid is looked up in;
**   - the platform child is released exactly once, in record_finish();
**   - a wait event is not the end of a child: only EXITED and SIGNALED end a
**     record, and a child that stopped keeps its resource and stays waitable.
**
** So a pid found in the table is a child that still owes a reap, and one
** that is not found is not this interpreter's to wait on by identity.  What
** the child did outlives the record, in the Process::Status the wait built;
** the record itself is only the obligation, and ends when it is discharged.
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/variable.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>

struct mrb_process_table {
  mrb_hal_process_context *hal;
  mrb_process_record *live;   /* creation order */
  mrb_process_record **tail;
};

/*
 * The table
 */

static void table_dispose(mrb_state *mrb, mrb_process_table *table);

static void
table_free(mrb_state *mrb, void *ptr)
{
  mrb_process_table *table = (mrb_process_table*)ptr;

  if (table == NULL) return;
  table_dispose(mrb, table);
  mrb_free(mrb, table);
}

static const mrb_data_type table_type = { "mruby/process/children", table_free };

static mrb_value
table_holder(mrb_state *mrb)
{
  struct RClass *process = mrb_module_get_id(mrb, MRB_SYM(Process));

  return mrb_iv_get(mrb, mrb_obj_value(process), MRB_IVSYM(children));
}

void
mrb_process_table_init(mrb_state *mrb, struct RClass *process)
{
  mrb_hal_process_context *hal;
  mrb_process_table *table;
  struct RData *data;

  if (mrb_hal_process_context_init(mrb, &hal) != 0) {
    mrb_sys_fail(mrb, "process context");
  }
  table = (mrb_process_table*)mrb_malloc_simple(mrb, sizeof(mrb_process_table));
  if (table == NULL) {
    mrb_hal_process_context_free(mrb, hal);
    mrb_raise(mrb, E_RUNTIME_ERROR, "out of memory");
  }
  table->hal = hal;
  table->live = NULL;
  table->tail = &table->live;

  data = mrb_data_object_alloc(mrb, mrb->object_class, table, &table_type);
  mrb_iv_set(mrb, mrb_obj_value(process), MRB_IVSYM(children), mrb_obj_value(data));
}

void
mrb_process_table_final(mrb_state *mrb)
{
  mrb_value holder = table_holder(mrb);
  mrb_process_table *table;

  if (mrb_type(holder) != MRB_TT_CDATA) return;
  table = (mrb_process_table*)DATA_PTR(holder);
  if (table == NULL) return;

  /* Do it here rather than leaving it to the data object's own free, so
     that the order is the documented one: children are dealt with while the
     interpreter is still whole. */
  table_dispose(mrb, table);
  mrb_free(mrb, table);
  DATA_PTR(holder) = NULL;
}

mrb_process_table*
mrb_process_table_get(mrb_state *mrb)
{
  mrb_value holder = table_holder(mrb);
  mrb_process_table *table = NULL;

  if (mrb_type(holder) == MRB_TT_CDATA) {
    table = (mrb_process_table*)DATA_PTR(holder);
  }
  if (table == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Process has no child context");
  }
  return table;
}

mrb_hal_process_context*
mrb_process_table_context(mrb_process_table *table)
{
  return table->hal;
}

/*
 * Records
 */

mrb_process_record*
mrb_process_record_register(mrb_state *mrb, mrb_process_table *table,
                            mrb_hal_process_child *hal_child)
{
  mrb_process_record *record =
    (mrb_process_record*)mrb_malloc_simple(mrb, sizeof(mrb_process_record));

  if (record == NULL) {
    mrb_hal_process_child_release(mrb, table->hal, hal_child);
    mrb_raise(mrb, E_RUNTIME_ERROR, "out of memory");
  }
  record->pid = mrb_hal_process_child_pid(hal_child);
  record->hal_child = hal_child;
  record->table = table;
  record->next = NULL;

  /* Creation order, because that is the order teardown deals with them in. */
  *table->tail = record;
  table->tail = &record->next;

  /* How a wait-any event finds its way back here without a lookup. */
  mrb_hal_process_child_set_udata(hal_child, record);
  return record;
}

mrb_process_record*
mrb_process_record_find(mrb_process_table *table, mrb_int pid)
{
  mrb_process_record *record;

  for (record = table->live; record != NULL; record = record->next) {
    if (record->pid == pid) return record;
  }
  return NULL;
}

/*
 * The one place a record is let go of.
 *
 * It leaves the live list, the platform child is released, and the record is
 * freed.  Nothing outside the table holds one, so leaving the table is the
 * end of it: what the child did is already in the Process::Status the wait
 * built, which outlives the record it came from.
 */
static void
record_finish(mrb_state *mrb, mrb_process_record *record)
{
  mrb_process_table *table = record->table;
  mrb_process_record **link;

  for (link = &table->live; *link != NULL; link = &(*link)->next) {
    if (*link == record) {
      *link = record->next;
      if (table->tail == &record->next) table->tail = link;
      break;
    }
  }
  record->next = NULL;

  mrb_hal_process_child_release(mrb, table->hal, record->hal_child);
  mrb_free(mrb, record);
}

/* Whether an event is the end of the child, as opposed to news about it. */
static mrb_bool
event_is_terminal(mrb_process_event_kind kind)
{
  return kind == MRB_PROCESS_EVENT_EXITED || kind == MRB_PROCESS_EVENT_SIGNALED;
}

mrb_value
mrb_process_record_wait(mrb_state *mrb, mrb_process_record *record, unsigned int flags)
{
  mrb_process_table *table = record->table;
  mrb_process_wait_target target;
  mrb_process_event event;
  mrb_value status;

  target.scope = MRB_PROCESS_WAIT_SCOPE_CHILD;
  target.child = record->hal_child;
  if (mrb_hal_process_wait(mrb, table->hal, &target, flags, &event) != 0) {
    mrb_sys_fail(mrb, "waitpid");
  }
  if (event.kind == MRB_PROCESS_EVENT_NONE) {
    mrb_process_set_last_status(mrb, mrb_nil_value());
    return mrb_nil_value();
  }

  status = mrb_process_status_new(mrb, &event.status);
  if (event_is_terminal(event.kind)) {
    record_finish(mrb, record);
  }
  mrb_process_set_last_status(mrb, status);
  return status;
}

mrb_value
mrb_process_wait_set(mrb_state *mrb, const mrb_process_wait_target *target,
                     unsigned int flags)
{
  mrb_process_table *table = mrb_process_table_get(mrb);
  mrb_process_event event;
  mrb_process_record *record;
  mrb_value status;

  if (mrb_hal_process_wait(mrb, table->hal, target, flags, &event) != 0) {
    mrb_sys_fail(mrb, "waitpid");
  }
  if (event.kind == MRB_PROCESS_EVENT_NONE) {
    mrb_process_set_last_status(mrb, mrb_nil_value());
    return mrb_nil_value();
  }

  status = mrb_process_status_new(mrb, &event.status);
  /* The set can hold a child this interpreter never spawned, one the host
     application forked itself.  Its status is still worth reporting; there is
     simply no record to move. */
  record = (event.child != NULL)
             ? (mrb_process_record*)mrb_hal_process_child_udata(event.child)
             : NULL;
  if (record != NULL && event_is_terminal(event.kind)) {
    record_finish(mrb, record);
  }
  mrb_process_set_last_status(mrb, status);
  return status;
}

/*
 * Teardown
 *
 * One non-blocking wait per child still owing a reap, and then it is let go
 * of either way.  A blocking wait here would let a child that never finishes
 * hang mrb_close, which is worse than the zombie the one left behind is on
 * POSIX until the host process exits.
 */
static void
table_dispose(mrb_state *mrb, mrb_process_table *table)
{
  while (table->live != NULL) {
    mrb_process_record *record = table->live;
    mrb_process_event event;

    mrb_process_wait_target target;

    target.scope = MRB_PROCESS_WAIT_SCOPE_CHILD;
    target.child = record->hal_child;
    /* A child that has finished is reaped; one that has not, or a wait that
       could not be performed at all, leaves the reap unpaid.  Either way the
       record is let go of, which is what keeps this loop finite. */
    mrb_hal_process_wait(mrb, table->hal, &target, MRB_PROCESS_WAIT_NOHANG, &event);
    record_finish(mrb, record);
  }
  mrb_hal_process_context_free(mrb, table->hal);
  table->hal = NULL;
}
