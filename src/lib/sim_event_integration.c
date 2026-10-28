/* sim_event_integration.c: Phase 2 integration of heap with existing APIs */
// SPDX-FileCopyrightText: 2024 B. Scott Michel
// SPDX-License-Identifier: X11

/*
   Phase 2: Integrate min-heap with existing sim_activate/sim_cancel APIs

   Strategy:
   - Replace linked list traversal with heap operations
   - Maintain UNIT.next and UNIT.time for backward compatibility
   - Keep existing API signatures unchanged
   - Add mutex for thread safety using sim_atomic.h spinlock

   This file provides drop-in replacements for:
   - _sim_activate() - heap insert instead of linked list
   - sim_cancel() - heap remove instead of list traversal
   - sim_process_event() - heap pop instead of list pop
   - sim_is_active() - check UNIT.next != NULL
*/

#include "sim_defs.h"
#include "sim_events.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "sim_atomic.h"

/* Forward declarations */
static void update_sim_interval_from_heap(void);

/* Simple spinlock using atomics (Phase 1 - will profile and upgrade if needed) */
static sim_atomic_value_t event_heap_spinlock;
static int spinlock_initialized = 0;

static inline void event_heap_spinlock_init(void)
{
    if (!spinlock_initialized) {
        sim_atomic_init(&event_heap_spinlock);
        sim_atomic_put(&event_heap_spinlock, 0);
        spinlock_initialized = 1;
    }
}

static inline void event_heap_lock_acquire(void)
{
    sim_atomic_type_t expected = 0;
    event_heap_spinlock_init();
    while (!sim_atomic_cas(&event_heap_spinlock, &expected, 1)) {
        /* Spin - reset expected for next attempt */
        expected = 0;
    }
}

static inline void event_heap_lock_release(void)
{
    sim_atomic_put(&event_heap_spinlock, 0);
}

/* Helper: Ensure heap is initialized (lazy init) */
static inline void ensure_heap_initialized(void)
{
    if (sim_event_heap_count() == 0 && sim_event_heap_peek() == NULL) {
        /* Heap exists but may need initialization */
        /* sim_event_heap_init() removed - heap self-initializes */
    }
}

/* Phase 2 replacement for _sim_activate

   Uses min-heap insert instead of linked list traversal.
   Maintains UNIT.next and UNIT.time for compatibility with code
   that reads these fields.
*/

t_stat _sim_activate_heap(UNIT *uptr, int32_t event_time)
{
    UNIT_EVENT *evt;
    t_stat status;

    /* Check if already active */
    if (uptr->next != NULL)
        return SCPE_OK;

    /* Allocate event structure */
    evt = (UNIT_EVENT *)calloc(1, sizeof(UNIT_EVENT));
    if (evt == NULL)
        return SCPE_MEM;

    /* Fill in event */
    evt->unit = uptr;
    evt->fire_time = (uint64_t)sim_gtime() + event_time;
    evt->is_timer_event = false;
    evt->heap_index = 0; /* will be set by heap_insert */

    /* Insert into heap (thread-safe) */
    event_heap_lock_acquire();

    status = sim_event_heap_insert(evt);
    if (status != SCPE_OK) {
        event_heap_lock_release();
        free(evt);
        return status;
    }

    /* Maintain backward compatibility fields */
    uptr->next = (UNIT *)1;  /* non-NULL sentinel: "I am scheduled" */
    uptr->time = event_time; /* for sim_activate_time() compatibility */

    /* Update sim_interval from heap minimum */
    update_sim_interval_from_heap();

    event_heap_lock_release();

    sim_debug(SIM_DBG_ACTIVATE, &sim_scp_dev, "Heap activating %s delay=%d fire_time=%lu\n", sim_uname(uptr),
              event_time, (unsigned long)evt->fire_time);

    return SCPE_OK;
}

/* Phase 2 replacement for sim_cancel

   Uses heap remove instead of linked list traversal.
*/

t_stat sim_cancel_heap(UNIT *uptr)
{
    UNIT_EVENT *evt;
    t_stat status;

    /* Not active? */
    if (uptr->next == NULL)
        return SCPE_OK;

    event_heap_lock_acquire();

    /* Find event in heap by matching unit pointer */
    evt = sim_event_heap_find_by_unit(uptr);

    if (evt == NULL) {
        event_heap_lock_release();
        /* Not found - clear fields and return OK */
        uptr->next = NULL;
        uptr->time = 0;
        return SCPE_OK;
    }

    /* Remove from heap */
    status = sim_event_heap_remove_at(evt->heap_index);

    /* Clear compatibility fields */
    uptr->next = NULL;
    uptr->time = 0;
    uptr->usecs_remaining = 0;

    /* Update sim_interval */
    update_sim_interval_from_heap();

    event_heap_lock_release();

    /* Free event */
    free(evt);

    sim_debug(SIM_DBG_EVENT, &sim_scp_dev, "Heap canceled event for %s\n", sim_uname(uptr));

    return status;
}

/* Phase 2 replacement for sim_process_event

   Uses heap pop instead of linked list pop.
*/

t_stat sim_process_event_heap(void)
{
    UNIT_EVENT *evt;
    UNIT *uptr;
    t_stat reason;
    uint64_t event_time;

    event_heap_lock_acquire();

    /* Process all events at the current time in one batch */
    do {
        /* Check if heap is empty */
        if (sim_event_heap_count() == 0) {
            event_heap_lock_release();
            /* noqueue_time is static in scp.c - only update sim_interval */
            sim_interval = NOQUEUE_WAIT;
            return SCPE_OK;
        }

        /* Peek at next event to check its fire time */
        evt = sim_event_heap_peek();
        if (evt == NULL) {
            event_heap_lock_release();
            return SCPE_IERR;
        }

        /* Check if event is ready to fire */
        if (evt->fire_time >= (uint64_t)sim_gtime()) {
            /* No more events ready, update interval and return */
            update_sim_interval_from_heap();
            event_heap_lock_release();
            return SCPE_OK;
        }

        /* Pop the event (it's ready to fire) */
        evt = sim_event_heap_pop();
        if (evt == NULL) {
            event_heap_lock_release();
            return SCPE_IERR;
        }

        uptr = evt->unit;
        event_time = evt->fire_time;

        /* Advance time to event fire time */
        sim_event_heap_set_now(evt->fire_time);

        /* Update sim_interval for next event */
        update_sim_interval_from_heap();

        event_heap_lock_release();

        /* Clear compatibility fields */
        uptr->next = NULL;
        uptr->time = 0;

        /* Save usecs_remaining before freeing event, then clear it */
        double saved_usecs = uptr->usecs_remaining;
        uptr->usecs_remaining = 0.0;

        /* Free event structure */
        free(evt);

        /* Execute event action */
        sim_debug(SIM_DBG_EVENT, &sim_scp_dev, "Heap processing event for %s\n", sim_uname(uptr));

        if (saved_usecs != 0.0) {
            /* Clamp negative values to zero (rounding errors from time calculations) */
            if (saved_usecs < 0.0) {
                sim_debug(SIM_DBG_EVENT, &sim_scp_dev, "Clamping negative usecs_remaining %.0f to 0.0 for %s\n",
                          saved_usecs, sim_uname(uptr));
                saved_usecs = 0.0;
            }
            if (saved_usecs > 0.0) {
                sim_debug(SIM_DBG_EVENT, &sim_scp_dev, "Requeueing %s after %.0f usecs\n", sim_uname(uptr),
                          saved_usecs);
                reason = sim_timer_activate_after(uptr, saved_usecs);
            } else {
                /* saved_usecs was negative, now clamped to 0 - just run the action */
                if (uptr->action != NULL)
                    reason = uptr->action(uptr);
                else
                    reason = SCPE_OK;
            }
        } else {
            if (uptr->action != NULL)
                reason = uptr->action(uptr);
            else
                reason = SCPE_OK;
        }

        /* If action returned an error/stop, return it */
        if (reason != SCPE_OK) {
            return reason;
        }

        /* Re-acquire lock for next iteration */
        event_heap_lock_acquire();

        /* Continue processing events at the same time */
    } while (1);
}

/* Helper: Update sim_interval based on next event in heap */
static void update_sim_interval_from_heap(void)
{
    UNIT_EVENT *next_evt;
    uint64_t now;
    int32_t delta;

    /* Must be called with lock held */

    if (sim_event_heap_count() == 0) {
        sim_interval = NOQUEUE_WAIT;
        sim_event_heap_set_prev_interval(sim_interval);
        return;
    }

    next_evt = sim_event_heap_peek();
    if (next_evt == NULL) {
        sim_interval = NOQUEUE_WAIT;
        sim_event_heap_set_prev_interval(sim_interval);
        return;
    }

    /* Calculate delta to next event */
    now = (uint64_t)sim_gtime();
    if (next_evt->fire_time <= now)
        delta = 0; /* overdue */
    else {
        uint64_t diff = next_evt->fire_time - now;
        delta = (diff > INT32_MAX) ? INT32_MAX : (int32_t)diff;
    }

    sim_interval = delta;
    sim_event_heap_set_prev_interval(sim_interval);
}

/* Helper: Sync UNIT fields from event (for compatibility) */
static void sync_unit_fields_from_event(UNIT *uptr, UNIT_EVENT *evt)
{
    uint64_t now = (uint64_t)sim_gtime();

    if (evt->fire_time <= now)
        uptr->time = 0;
    else {
        uint64_t delta = evt->fire_time - now;
        uptr->time = (delta > INT32_MAX) ? INT32_MAX : (int32_t)delta;
    }
}
