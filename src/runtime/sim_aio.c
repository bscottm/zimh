// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"

#if defined(SIM_ASYNCH_IO)
#include "sim_threads.h"
#include "sim_aio.h"

/* AIO condvar lock */
sim_mutex_t sim_asynch_lock;
/* AIO condvar */
sim_cond_t sim_asynch_wake;

/* Debugging output serializer mutex. */
sim_mutex_t sim_debug_lock;

sim_mutex_t sim_timer_lock;
sim_cond_t sim_timer_wake;
bool sim_timer_event_canceled;

int32_t sim_tmxr_poll_count;
sim_cond_t sim_tmxr_poll_cond;
sim_mutex_t sim_tmxr_poll_lock;

sim_thread_t sim_asynch_main_threadid;

/* Atomic pointer to the pending asynchronous UNIT events */
sim_atomic_ptr_t sim_asynch_queue;
/* Atomic masquerading as a bool. */
sim_atomic_value_t sim_idle_wait;

int32_t sim_asynch_check;
int32_t sim_asynch_latency = 4000;    /* 4 usec interrupt latency */
int32_t sim_asynch_inst_latency = 20; /* assume 5 mip simulator */

bool sim_asynch_enabled = true;

int32_t sim_asynch_check;

#else

/* AIO not enabled. */
bool sim_asynch_enabled = true;

#endif

/* AIO initialization: Initializes thread state when SIM_ASYNCH_IO is defined. Otherwise, it's a NOP. */
t_stat aio_init()
{
#if defined(SIM_ASYNCH_IO)   
            sim_mutex_recursive(&sim_asynch_lock);
            sim_cond_init(&sim_asynch_wake);
            sim_mutex_init(&sim_debug_lock);
            sim_mutex_init(&sim_timer_lock);
            sim_cond_init(&sim_timer_wake);
            sim_timer_event_canceled = false;
            sim_tmxr_poll_count = 0;
            sim_cond_init(&sim_tmxr_poll_cond);
            sim_mutex_init(&sim_tmxr_poll_lock);

            sim_asynch_main_threadid = sim_thread_self();

            sim_atomic_ptr_init(&sim_asynch_queue);
            sim_atomic_ptr_put(&sim_asynch_queue, QUEUE_LIST_END);
            sim_atomic_init(&sim_idle_wait);

            /* Paranoia, defensive coding. */
            sim_atomic_put(&sim_idle_wait, (sim_atomic_type_t) false);
#endif

    return SCPE_OK;
}

/* AIO cleanup/shutdown: Releases thread state when SIM_ASYNCH_IO is defined. Otherwise, it's a NOP. */
t_stat aio_cleanup()
{
#if defined(SIM_ASYNCH_IO)   
            sim_mutex_destroy(&sim_asynch_lock);
            sim_cond_destroy(&sim_asynch_wake);
            sim_mutex_destroy(&sim_debug_lock);
            sim_mutex_destroy(&sim_timer_lock);
            sim_cond_destroy(&sim_timer_wake);
            sim_timer_event_canceled = false;
            sim_tmxr_poll_count = 0;
            sim_cond_destroy(&sim_tmxr_poll_cond);
            sim_mutex_destroy(&sim_tmxr_poll_lock);

            sim_atomic_ptr_destroy(&sim_asynch_queue);
            sim_atomic_destroy(&sim_idle_wait);
#endif

    return SCPE_OK;
}

/* Initialize AIO fields in a UNIT structure.
 * Should be called during device reset or unit initialization. */
void aio_init_unit(UNIT *uptr)
{
#if defined(SIM_ASYNCH_IO)
    sim_atomic_init(&uptr->a_is_queued);
    sim_atomic_put(&uptr->a_is_queued, (sim_atomic_type_t)false);
#else
    (void)uptr;  /* Suppress unused parameter warning */
#endif
}

int sim_aio_update_queue(void)
{
    int migrated = 0;

#if defined(SIM_ASYNCH_IO)
    if (sim_atomic_ptr_get(&sim_asynch_queue) != QUEUE_LIST_END) { /* List !Empty */
        int32_t a_event_time;
        UNIT *volatile qhead;

        for (;;) { /* Grab current queue */
            qhead = (UNIT *)sim_atomic_ptr_get(&sim_asynch_queue);
            if (sim_atomic_ptr_cas(&sim_asynch_queue, (void **)&qhead, QUEUE_LIST_END))
                break;

            /* Retry... queue head changed... */
        }

        UNIT *q = qhead, *uptr;

        while (q != QUEUE_LIST_END) { /* List !Empty */
            sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev, "Migrating Asynch event for %s after %d %s\n", sim_uname(q),
                      q->a_event_time, sim_vm_interval_units);
            ++migrated;
            uptr = q;
            q = q->a_next;
            sim_atomic_put(&uptr->a_is_queued, (sim_atomic_type_t)false); /* Clear queued flag */
            uptr->a_next = NULL; /* hygiene */
            if (uptr->a_activate_call != &sim_activate_notbefore) {
                a_event_time = uptr->a_event_time - ((sim_asynch_inst_latency + 1) / 2);
                if (a_event_time < 0)
                    a_event_time = 0;
            } else
                a_event_time = uptr->a_event_time;

            uptr->a_activate_call(uptr, a_event_time);
            if (uptr->a_check_completion != NULL) {
                sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev, "Calling Completion Check for asynch event on %s\n",
                          sim_uname(uptr));
                uptr->a_check_completion(uptr);
            }
        }
    }
#endif

    return migrated;
}

void sim_aio_activate (ACTIVATE_API caller, UNIT *uptr, int32_t event_time)
{
#if defined(SIM_ASYNCH_IO)
    sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev, "Queueing Asynch event for %s after %d %s\n", sim_uname(uptr),
              event_time, sim_vm_interval_units);

    /* Atomically check and claim this unit for enqueueing */
    sim_atomic_type_t expected = (sim_atomic_type_t)false;
    if (sim_atomic_cas(&uptr->a_is_queued, &expected, (sim_atomic_type_t)true)) {
        /* Successfully claimed - this unit is not currently queued */
        UNIT * volatile qhead;

        uptr->a_event_time = event_time;
        uptr->a_activate_call = caller;

        for (;;) {
            qhead = (UNIT *volatile)sim_atomic_ptr_get(&sim_asynch_queue);
            /* Assign a_next prior to the CAS to commit the update. */
            uptr->a_next = qhead;
            if (sim_atomic_ptr_cas(&sim_asynch_queue, (void **)&qhead, uptr))
                break;
        }
    } else {
        /* Already queued - just update the callback to use absolute activation */
        uptr->a_activate_call = sim_activate_abs;
    }

    sim_asynch_check = 0;     /* try to force check */
    if ((bool) sim_atomic_get(&sim_idle_wait)) {
        sim_debug(TIMER_DBG_IDLE, &sim_timer_dev, "waking due to event on %s after %d %s\n", sim_uname(uptr),
                  event_time, sim_vm_interval_units);

        sim_mutex_lock(&sim_asynch_lock);
        sim_cond_signal(&sim_asynch_wake);
        sim_mutex_unlock(&sim_asynch_lock);
    }
#endif
}

#if defined(SIM_ASYNCH_IO)
/* Check an AIO queue's sanity -- the list should be continuous and terminated by a QUEUE_LIST_END sentinel, not
 * NULL. */
void aio_queue_check(sim_atomic_ptr_t *queue, sim_mutex_t *lock)
{
    UNIT * volatile _cptr;

    if (lock != NULL)
        sim_mutex_lock(lock);

    for (_cptr = sim_atomic_ptr_get(queue); _cptr != QUEUE_LIST_END; _cptr = _cptr->next) {
        if (_cptr->next == NULL) {
            if (sim_deb) {
                sim_debug(SIM_DBG_EVENT, sim_dflt_dev, "Queue Corruption detected\n");
                fclose(sim_deb);
            }
            sim_printf("Queue Corruption detected in %s line %d\n", __FILE__, __LINE__);
            abort();
        }
    }

    if (lock != NULL)
        sim_mutex_unlock(lock);
}
#endif

void aio_in_main_thread(UNIT *unit)
{
#if defined(SIM_ASYNCH_IO)
    if (!aio_main_thread()) {
        sim_printf("Improper thread context for operation on %s in %s line %d\n", sim_uname(unit), __FILE__, __LINE__);
        abort();
    }
#endif
}

/* Unit has AIO pending?
 *
 * NOTE: Was the AIO_IS_ACTIVE macro, converted to inline, now a regular function.
 */
bool unit_has_aio_pending(UNIT *uptr)
{
#if defined(SIM_ASYNCH_IO)
    return ((uptr->a_is_active != NULL && uptr->a_is_active(uptr)) ||
            (bool)sim_atomic_get(&uptr->a_is_queued));
#else
    /* Never have pending AIO in the !SIM_ASYNCH_IO path. */
    (void)uptr;  /* Suppress unused parameter warning */
    return false;
#endif
}
