// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_threads.h"
#include "sim_aio.h"

sim_mutex_t sim_asynch_lock;
sim_cond_t sim_asynch_wake;
sim_mutex_t sim_timer_lock;
sim_cond_t sim_timer_wake;

sim_mutex_t sim_tmxr_poll_lock;
sim_cond_t sim_tmxr_poll_cond;
int32_t sim_tmxr_poll_count;

sim_thread_t sim_asynch_main_threadid;

UNIT *volatile sim_asynch_queue;
bool sim_asynch_enabled = true;

int32_t sim_asynch_check;
int32_t sim_asynch_latency = 4000;    /* 4 usec interrupt latency */
int32_t sim_asynch_inst_latency = 20; /* assume 5 mip simulator */

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

int sim_aio_update_queue(void)
{
    int migrated = 0;

    AIO_ILOCK;
    if (SIM_LIKELY(AIO_QUEUE_VAL != QUEUE_LIST_END)) { /* List !Empty */
        UNIT *q, *uptr;
        int32_t a_event_time;
        do {                                           /* Grab current queue */
            q = AIO_QUEUE_VAL;
        } while (q != AIO_QUEUE_SET(QUEUE_LIST_END, q));
        while (q != QUEUE_LIST_END) {                  /* List !Empty */
            sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev, "Migrating Asynch event for %s after %d %s\n", sim_uname(q),
                      q->a_event_time, sim_vm_interval_units);
            ++migrated;
            uptr = q;
            q = q->a_next;
            uptr->a_next = NULL; /* hygiene */
            if (uptr->a_activate_call != &sim_activate_notbefore) {
                a_event_time = uptr->a_event_time - ((sim_asynch_inst_latency + 1) / 2);
                if (a_event_time < 0)
                    a_event_time = 0;
            } else
                a_event_time = uptr->a_event_time;
            AIO_IUNLOCK;
            uptr->a_activate_call(uptr, a_event_time);
            if (uptr->a_check_completion) {
                sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev, "Calling Completion Check for asynch event on %s\n",
                          sim_uname(uptr));
                uptr->a_check_completion(uptr);
            }
            AIO_ILOCK;
        }
    }
    AIO_IUNLOCK;
    return migrated;
}

void sim_aio_activate(ACTIVATE_API caller, UNIT *uptr, int32_t event_time)
{
    AIO_ILOCK;
    sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev, "Queueing Asynch event for %s after %d %s\n", sim_uname(uptr),
              event_time, sim_vm_interval_units);
    if (uptr->a_next) {
        uptr->a_activate_call = sim_activate_abs;
    } else {
        UNIT *q;
        uptr->a_event_time = event_time;
        uptr->a_activate_call = caller;
        do {
            q = AIO_QUEUE_VAL;
            uptr->a_next = q; /* Mark as on list */
        } while (q != AIO_QUEUE_SET(uptr, q));
    }
    sim_asynch_check = 0;     /* try to force check */
    if (sim_idle_wait) {
        sim_debug(TIMER_DBG_IDLE, &sim_timer_dev, "waking due to event on %s after %d %s\n", sim_uname(uptr),
                  event_time, sim_vm_interval_units);
        pthread_cond_signal(&sim_asynch_wake);
    }
    AIO_IUNLOCK;
}
