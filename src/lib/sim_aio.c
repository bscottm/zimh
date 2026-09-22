// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_threads.h"
#include "sim_aio.h"
#include "sim_event_queue.h"

sim_mutex_t sim_asynch_lock;
sim_cond_t sim_asynch_wake;
sim_mutex_t sim_timer_lock;
sim_cond_t sim_timer_wake;

sim_mutex_t sim_tmxr_poll_lock;
sim_cond_t sim_tmxr_poll_cond;
int32_t sim_tmxr_poll_count;

sim_thread_t sim_asynch_main_threadid;

/* DEPRECATED: Old intrusive queue - NO LONGER USED
 * Kept for binary compatibility until UNIT.a_next can be removed */
UNIT *volatile sim_asynch_queue;

/* NEW: MPSC queue for producer-side enqueue */
static sim_event_mpsc_queue_t sim_event_queue;

/* NEW: Min-heap for consumer-side processing */
static sim_event_heap_t sim_event_heap;

/* Accessor for queue display - returns pointer to the async I/O event heap */
const sim_event_heap_t *sim_aio_get_event_heap(void)
{
    return &sim_event_heap;
}

// Async I/O preference gate.
bool sim_async_preference;
// Async I/O enabled flag.
bool sim_asynch_enabled;

sim_atomic_value_t sim_asynch_check;
int32_t sim_asynch_latency = 4000;    /* 4 usec interrupt latency */
int32_t sim_asynch_inst_latency = 20; /* assume 5 mip simulator */

void aio_init()
{
    // Thread support initialization:
    sim_mutex_recursive(&sim_asynch_lock);
    sim_cond_init(&sim_asynch_wake);
    sim_mutex_init(&sim_timer_lock);
    sim_cond_init(&sim_timer_wake);
    sim_mutex_init(&sim_tmxr_poll_lock);
    sim_cond_init(&sim_tmxr_poll_cond);

    // Set the async I/O preference based on the number of available CPUs.
    sim_async_preference = (sim_os_get_cpu_count() >= 2);
    // Async I/O is enabled if the preference is true. It can be disabled via the CLI.
    sim_asynch_enabled = sim_async_preference;

    /* OLD: Empty list/list end uses the point value (void *) 1. */
    sim_asynch_queue = QUEUE_LIST_END;

    /* NEW: Initialize MPSC queue and min-heap */
    sim_event_queue_init(&sim_event_queue);
    sim_event_heap_init(&sim_event_heap);

    /* Set the simulator thread's ID (since this is ALWAYS called by the simulator's thread,
     * set its default thread affinity. */
    sim_asynch_main_threadid = sim_thread_self();
    if (sim_async_preference) {
        /* Set the main thread's affinity: */
        sim_cpu_set_t main_set;

        sim_os_get_cpu_partition(&main_set, NULL, NULL);
        if (!sim_cpu_set_empty(&main_set))
            sim_os_set_thread_affinity(&main_set);
    }
}

void aio_cleanup()
{
    /* NEW: Destroy queue and heap */
    sim_event_heap_destroy(&sim_event_heap);
    sim_event_queue_destroy(&sim_event_queue);

    sim_mutex_destroy(&sim_asynch_lock);
    sim_cond_destroy(&sim_asynch_wake);
    sim_mutex_destroy(&sim_timer_lock);
    sim_cond_destroy(&sim_timer_wake);
    sim_mutex_destroy(&sim_tmxr_poll_lock);
    sim_cond_destroy(&sim_tmxr_poll_cond);
}

/* AIO queue corruption check */
bool aio_queue_check(UNIT *volatile queue, sim_mutex_t *lock)
{
    UNIT *cptr;

    if (lock != NULL)
        sim_mutex_lock(lock);

    for (cptr = queue; (cptr != QUEUE_LIST_END); cptr = cptr->next)
        if (!cptr->next) {
            if (sim_deb) {
                sim_debug(SIM_DBG_EVENT, sim_dflt_dev, "Queue Corruption detected\n");
                fclose(sim_deb);
            }
            sim_printf("Queue Corruption detected in %s line %d\n", __FILE__, __LINE__);
            abort();
        }

    if (lock != NULL)
        sim_mutex_unlock(lock);

    return true;
}

/* DEPRECATED: Old AIO queue update - kept for compatibility during transition
 * TODO: Remove this once all callers are updated to use the new system */
int sim_aio_update_queue(void)
{
    /* Migrate events from MPSC queue to min-heap */
    int migrated = sim_event_migrate_to_heap(&sim_event_queue, &sim_event_heap);
    if (migrated > 0) {
        sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev,
                  "Migrated %d async events from queue to heap\n", migrated);
    }

    return migrated;
}

/* NEW: Process events from the min-heap with time <= current_time
 * This should be called from sim_process_event() after AIO_UPDATE_QUEUE */
int sim_aio_process_heap(int32_t current_time)
{
    return sim_event_process_heap(&sim_event_heap, current_time);
}

/* Activation function for async I/O events from producer threads */
void sim_aio_activate(ACTIVATE_API caller, UNIT *uptr, int32_t event_time)
{
    sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev, "Queueing Asynch event for %s after %d %s\n", sim_uname(uptr),
              event_time, sim_vm_interval_units);

    /* Enqueue to lock-free MPSC queue */
    if (!sim_event_enqueue(&sim_event_queue, uptr, event_time, caller, uptr->a_check_completion)) {
        sim_printf("FATAL: Failed to enqueue async event for %s\n", sim_uname(uptr));
        abort();
    }

    sim_atomic_put(&sim_asynch_check, 0);  /* Force check */
    if (sim_idle_wait) {
        sim_debug(TIMER_DBG_IDLE, &sim_timer_dev, "waking due to event on %s after %d %s\n", sim_uname(uptr),
                  event_time, sim_vm_interval_units);
        sim_cond_signal(&sim_asynch_wake);
    }
}
