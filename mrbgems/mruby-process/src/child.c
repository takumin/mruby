/*
** child.c - child ownership: the record table and Process::Child
**
** See Copyright Notice in mruby.h
**
** What a wait actually waits on.  The resource being owned is the exit
** status slot the OS keeps for a terminated child, together with -- on
** Windows -- the handle that keeps that slot readable.  A pid only labels
** that slot, and the OS is free to hand the label to another process once
** the slot is released, so the label cannot be the identity.  The record
** below is, and these invariants say what that buys:
**
**   - a record is created only by spawning (nothing adopts a pid it did not
**     create), and it leaves LIVE at exactly one place, record_finish();
**   - the table holds the LIVE records and nothing else, so it is at once
**     the list teardown walks and the index a pid is looked up in;
**   - the platform child is released exactly once, in record_finish();
**   - a wait event is not a lifecycle transition: only EXITED and SIGNALED
**     move a record, and a stopped child stays LIVE, still holding its
**     resource, still waitable.
**
** A record is reachable by reference, from each Process::Child that wraps
** it, and by pid, through the table -- but only while it is LIVE.  That is
** what makes reaping the same child twice harmless: the second reaper holds
** a reference, finds the record already REAPED, and answers from the status
** it stored rather than asking the OS about a pid that may since have been
** handed to someone else.
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/variable.h>
#include "process_hal.h"
#include "process_internal.h"

#include <errno.h>
#include <string.h>

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

void
mrb_process_record_ref(mrb_process_record *record)
{
  record->refc++;
}

void
mrb_process_record_unref(mrb_state *mrb, mrb_process_record *record)
{
  if (--record->refc == 0) {
    mrb_free(mrb, record);
  }
}

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
  record->lifecycle = MRB_PROCESS_LIVE;
  record->has_status = FALSE;
  memset(&record->status, 0, sizeof(record->status));
  record->hal_child = hal_child;
  record->table = table;
  record->next = NULL;
  record->refc = 1;   /* the table's own */

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
 * The one place a record leaves LIVE.
 *
 * It leaves the live list, the platform child is released, and the table
 * drops the reference it held -- which frees the record unless a
 * Process::Child still has it, in which case the status stored here is what
 * that object goes on answering from.
 */
static void
record_finish(mrb_state *mrb, mrb_process_record *record,
              mrb_process_lifecycle lifecycle, const mrb_process_status *status)
{
  mrb_process_table *table = record->table;
  mrb_process_record **link;

  if (status != NULL) {
    record->status = *status;
    record->has_status = TRUE;
  }
  record->lifecycle = lifecycle;

  for (link = &table->live; *link != NULL; link = &(*link)->next) {
    if (*link == record) {
      *link = record->next;
      if (table->tail == &record->next) table->tail = link;
      break;
    }
  }
  record->next = NULL;

  mrb_hal_process_child_release(mrb, table->hal, record->hal_child);
  record->hal_child = NULL;
  record->table = NULL;

  mrb_process_record_unref(mrb, record);
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
  mrb_process_table *table;
  mrb_process_event event;
  mrb_value status;

  if (record->lifecycle != MRB_PROCESS_LIVE) {
    /* Already accounted for.  A reaped child answers from what was stored
       when it was reaped; a detached one is not this interpreter's to
       report on any more. */
    if (!record->has_status) return mrb_nil_value();
    status = mrb_process_status_new(mrb, &record->status);
    mrb_process_set_last_status(mrb, status);
    return status;
  }

  table = record->table;
  if (mrb_hal_process_wait(mrb, table->hal, record->hal_child, flags, &event) != 0) {
    mrb_sys_fail(mrb, "waitpid");
  }
  if (event.kind == MRB_PROCESS_EVENT_NONE) {
    mrb_process_set_last_status(mrb, mrb_nil_value());
    return mrb_nil_value();
  }

  status = mrb_process_status_new(mrb, &event.status);
  if (event_is_terminal(event.kind)) {
    record_finish(mrb, record, MRB_PROCESS_REAPED, &event.status);
  }
  mrb_process_set_last_status(mrb, status);
  return status;
}

mrb_value
mrb_process_wait_any(mrb_state *mrb, unsigned int flags)
{
  mrb_process_table *table = mrb_process_table_get(mrb);
  mrb_process_event event;
  mrb_process_record *record;
  mrb_value status;

  if (mrb_hal_process_wait(mrb, table->hal, NULL, flags, &event) != 0) {
    mrb_sys_fail(mrb, "waitpid");
  }
  if (event.kind == MRB_PROCESS_EVENT_NONE) {
    mrb_process_set_last_status(mrb, mrb_nil_value());
    return mrb_nil_value();
  }

  status = mrb_process_status_new(mrb, &event.status);
  /* A wait-any can draw a child this interpreter never spawned -- one the
     host process forked itself.  Its status is still worth reporting; there
     is simply no record to move. */
  record = (event.child != NULL)
             ? (mrb_process_record*)mrb_hal_process_child_udata(event.child)
             : NULL;
  if (record != NULL && event_is_terminal(event.kind)) {
    record_finish(mrb, record, MRB_PROCESS_REAPED, &event.status);
  }
  mrb_process_set_last_status(mrb, status);
  return status;
}

void
mrb_process_record_detach(mrb_state *mrb, mrb_process_record *record)
{
  if (record->lifecycle != MRB_PROCESS_LIVE) return;
  record_finish(mrb, record, MRB_PROCESS_DETACHED, NULL);
}

/*
 * Teardown
 *
 * One non-blocking wait per live child, and then it is let go of either way.
 * A blocking wait here would let a child that never finishes hang mrb_close,
 * which is worse than the zombie a detached child can leave on POSIX until
 * the host process exits.
 */
static void
table_dispose(mrb_state *mrb, mrb_process_table *table)
{
  while (table->live != NULL) {
    mrb_process_record *record = table->live;
    mrb_process_event event;

    if (mrb_hal_process_wait(mrb, table->hal, record->hal_child,
                             MRB_PROCESS_WAIT_NOHANG, &event) == 0 &&
        event_is_terminal(event.kind)) {
      record_finish(mrb, record, MRB_PROCESS_REAPED, &event.status);
    }
    else {
      record_finish(mrb, record, MRB_PROCESS_DETACHED, NULL);
    }
  }
  mrb_hal_process_context_free(mrb, table->hal);
  table->hal = NULL;
}

/*
 * Process::Child
 */

static void
child_free(mrb_state *mrb, void *ptr)
{
  if (ptr != NULL) {
    mrb_process_record_unref(mrb, (mrb_process_record*)ptr);
  }
}

static const mrb_data_type child_type = { "Process::Child", child_free };

mrb_value
mrb_process_child_new(mrb_state *mrb, mrb_process_record *record)
{
  struct RClass *process = mrb_module_get_id(mrb, MRB_SYM(Process));
  struct RClass *klass = mrb_class_get_under_id(mrb, process, MRB_SYM(Child));
  struct RData *data = mrb_data_object_alloc(mrb, klass, NULL, &child_type);

  mrb_process_record_ref(record);
  data->data = record;
  return mrb_obj_value(data);
}

static mrb_process_record*
child_record(mrb_state *mrb, mrb_value self)
{
  mrb_process_record *record = (mrb_process_record*)mrb_data_get_ptr(mrb, self, &child_type);

  if (record == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "uninitialized Process::Child");
  }
  return record;
}

/*
 * call-seq:
 *   child.pid -> integer
 *
 * The process ID the child was given.  It is a label: once the child has
 * been waited for, the platform may hand the same number to another process,
 * and only this object still stands for the one that had it.
 */
static mrb_value
child_pid(mrb_state *mrb, mrb_value self)
{
  return mrb_int_value(mrb, child_record(mrb, self)->pid);
}

/*
 * call-seq:
 *   child.live? -> true or false
 *
 * Whether the child still owes a wait -- neither reaped nor detached.
 */
static mrb_value
child_live_p(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(child_record(mrb, self)->lifecycle == MRB_PROCESS_LIVE);
}

/*
 * call-seq:
 *   child.wait(flags = 0) -> Process::Status or nil
 *
 * Waits for the child and returns how it finished, setting <code>$?</code>
 * to the same status.  With Process::WNOHANG among +flags+, returns nil and
 * sets <code>$?</code> to nil when there is nothing to report yet.
 *
 * Waiting twice is not an error and does not reach the operating system a
 * second time: the status observed the first time is what is returned and
 * published again.  A child that was detached has no status to give and
 * answers nil.
 */
static mrb_value
child_wait(mrb_state *mrb, mrb_value self)
{
  mrb_int flags = 0;

  mrb_get_args(mrb, "|i", &flags);
  return mrb_process_record_wait(mrb, child_record(mrb, self), (unsigned int)flags);
}

/*
 * call-seq:
 *   child.detach -> nil
 *
 * Gives up the obligation to wait for the child, without waiting for it.
 * Whatever it does next is the host process's business, not this
 * interpreter's.
 */
static mrb_value
child_detach(mrb_state *mrb, mrb_value self)
{
  mrb_process_record_detach(mrb, child_record(mrb, self));
  return mrb_nil_value();
}

void
mrb_process_child_init(mrb_state *mrb, struct RClass *process)
{
  struct RClass *child = mrb_define_class_under_id(mrb, process, MRB_SYM(Child), mrb->object_class);

  /* A child object stands for a child that exists; there is no way to make
     one except by creating the process it stands for. */
  MRB_SET_INSTANCE_TT(child, MRB_TT_CDATA);
  mrb_undef_class_method_id(mrb, child, MRB_SYM(new));

  mrb_define_method_id(mrb, child, MRB_SYM(pid),     child_pid,     MRB_ARGS_NONE());
  mrb_define_method_id(mrb, child, MRB_SYM_Q(live),  child_live_p,  MRB_ARGS_NONE());
  mrb_define_method_id(mrb, child, MRB_SYM(wait),    child_wait,    MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, child, MRB_SYM(detach),  child_detach,  MRB_ARGS_NONE());
}
