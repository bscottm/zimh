// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

//
#if !defined(SIM_AIO_H_)
#    define SIM_AIO_H_ 1

#    include <stdint.h>

#    include "sim_defs.h"
#    include "sim_threads.h"
#    include "sim_atomic.h"
#include "sim_atomic_ptr.h"

#    define SIM_ASYNCH_CLOCKS 1

extern sim_mutex_t sim_asynch_lock;
extern sim_cond_t sim_asynch_wake;

extern sim_mutex_t sim_timer_lock;
extern sim_cond_t sim_timer_wake;

extern bool sim_timer_event_canceled;

extern int32_t sim_tmxr_poll_count;
extern sim_cond_t sim_tmxr_poll_cond;
extern sim_mutex_t sim_tmxr_poll_lock;

// Simulator thread ID. Might not actually be the process' main thread.
extern sim_thread_t sim_asynch_main_threadid;

/* Async I/O (threading) preference: If true, async I/O is preferred and can be enabled or disabled via
 * sym_asynch_enabled. If false, ZIMH will not use async I/O. It cannot be changed via the CLI.
 *
 * In effect, this preference acts as a gate to whether threads will be used. In a uniprocessor environment,
 * sim_async_preference will be false. */
extern bool sim_async_preference;

/* Async I/O (threading) enabled flag: If true, async I/O is enabled. If false, ZIMH will use polling instead
 * of threads for I/O. */
extern bool sim_asynch_enabled;

// Pending asynchronous UNIT service requests. FIXME: Replace with sim_tailq_t:
extern UNIT *volatile sim_asynch_queue;

extern volatile bool sim_idle_wait;

extern int32_t sim_asynch_check;
extern int32_t sim_asynch_latency;
extern int32_t sim_asynch_inst_latency;

/* Async I/O queue sanity check function. (Note: Potentially not used.) */
extern bool aio_queue_check(UNIT *volatile queue, sim_mutex_t *lock);

/* Async I/O enabled and active? predicate */
static inline bool aio_enabled_and_active() {
    return (sim_async_preference && sim_asynch_enabled);
}

/* Executing in the simulator's thread? */
static inline bool is_simulator_thread() {
    return sim_thread_equal(sim_thread_self(), sim_asynch_main_threadid);
}

/* Does the unit have pending asynchronous I/O? */
static inline bool is_unit_aio_active(const UNIT *unit) {
    return ((unit->a_is_active != NULL ? unit->a_is_active(unit) : false) || unit->a_next != NULL);
}

/* Ensure that an AIO-related function is executing in the simulator's thread.
 *
 * Replacement for the AIO_VALIDATE macro.
 */
static inline void is_simulator_thread_assert(UNIT * const unit)
{
    if (!is_simulator_thread()) {
        sim_printf("Improper thread context for operation on %s in %s line %d\n", sim_uname(unit), __FILE__, __LINE__);
        abort();
    }
}

static inline void aio_global_lock() {
    sim_mutex_lock(&sim_asynch_lock);
}

static inline void aio_global_unlock() {
    sim_mutex_unlock(&sim_asynch_lock);
}

//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
// Extern functions:
//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=

/* Initialize async I/O. */
extern void aio_init();
/* Clean up async I/O. */
extern void aio_cleanup();

/* NEW: Process events from the min-heap with time <= current_time
 * Should be called from sim_process_event() after AIO_UPDATE_QUEUE */
extern int sim_aio_process_heap(int32_t current_time);

#    if defined(SIM_ASYNCH_MUX)
#        define AIO_CANCEL(uptr)                                                                                       \
            if (((uptr)->dynflags & UNIT_TM_POLL) && !((uptr)->next) && !((uptr)->a_next)) {                           \
                (uptr)->a_polling_now = false;                                                                         \
                sim_tmxr_poll_count -= (uptr)->a_poll_waiter_count;                                                    \
                (uptr)->a_poll_waiter_count = 0;                                                                       \
            }
#    endif /* defined(SIM_ASYNCH_MUX) */
#    if !defined(AIO_CANCEL)
#        define AIO_CANCEL(uptr)
#    endif /* !defined(AIO_CANCEL) */

#    ifdef USE_AIO_INTRINSICS
/* This approach uses intrinsics to manage access to the link list head     */
/* sim_asynch_queue.  This implementation is a completely lock free design  */
/* which avoids the potential ABA issues.                                   */
#        define AIO_QUEUE_MODE "Lock free asynchronous event queue"
#        ifdef _WIN32
#        elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
#            define InterlockedCompareExchangePointer(Destination, Exchange, Comparand)                                \
                __sync_val_compare_and_swap(Destination, Comparand, Exchange)
#        else
#            error                                                                                                     \
                "Implementation of function InterlockedCompareExchangePointer() is needed to build with USE_AIO_INTRINSICS"
#        endif
#        define AIO_ILOCK aio_global_lock()
#        define AIO_IUNLOCK aio_global_unlock()
#        define AIO_QUEUE_VAL                                                                                          \
            (UNIT *)(InterlockedCompareExchangePointer((void *volatile *)&sim_asynch_queue, (void *)sim_asynch_queue,  \
                                                       NULL))
#        define AIO_QUEUE_SET(newval, oldval)                                                                          \
            (UNIT *)(InterlockedCompareExchangePointer((void *volatile *)&sim_asynch_queue, (void *)newval, oldval))
#        define AIO_UPDATE_QUEUE sim_aio_update_queue()
#        define AIO_ACTIVATE(caller, uptr, event_time)                                                                 \
            if (!is_simulator_thread()) {                                      \
                sim_aio_activate((ACTIVATE_API)caller, uptr, event_time);                                              \
                return SCPE_OK;                                                                                        \
            } else                                                                                                     \
                (void)0
#    else /* !USE_AIO_INTRINSICS */
/* This approach uses a pthread mutex to manage access to the link list     */
/* head sim_asynch_queue.  It will always work, but may be slower than the  */
/* lock free approach when using USE_AIO_INTRINSICS                         */
#        define AIO_QUEUE_MODE "Lock based asynchronous event queue"
#        define AIO_ILOCK aio_global_lock()
#        define AIO_IUNLOCK aio_global_unlock()
#        define AIO_QUEUE_VAL sim_asynch_queue
#        define AIO_QUEUE_SET(newval, oldval) ((sim_asynch_queue = newval), oldval)
#        define AIO_UPDATE_QUEUE sim_aio_update_queue()
#        define AIO_ACTIVATE(caller, uptr, event_time)                                                                 \
            if (!is_simulator_thread()) {                                                                              \
                sim_debug(SIM_DBG_AIO_QUEUE, sim_dflt_dev, "Queueing Asynch event for %s after %d instructions\n",     \
                          sim_uname(uptr), event_time);                                                                \
                aio_global_lock();                                                                                              \
                if (uptr->a_next) { /* already queued? */                                                              \
                    uptr->a_activate_call = sim_activate_abs;                                                          \
                } else {                                                                                               \
                    uptr->a_next = sim_asynch_queue;                                                                   \
                    uptr->a_event_time = event_time;                                                                   \
                    uptr->a_activate_call = (ACTIVATE_API) & caller;                                                   \
                    sim_asynch_queue = uptr;                                                                           \
                }                                                                                                      \
                sim_asynch_check = 0;                                                                                  \
                if (sim_idle_wait) {                                                                                   \
                    if (sim_deb) { /* only while debug do lock/unlock overhead */                                      \
                        aio_global_unlock();                                                                                    \
                        sim_debug(TIMER_DBG_IDLE, &sim_timer_dev, "waking due to event on %s after %d instructions\n", \
                                  sim_uname(uptr), event_time);                                                        \
                        aio_global_lock();                                                                                      \
                    }                                                                                                  \
                    pthread_cond_signal(&sim_asynch_wake);                                                             \
                }                                                                                                      \
                aio_global_unlock();                                                                                            \
                return SCPE_OK;                                                                                        \
            } else                                                                                                     \
                (void)0
#    endif /* USE_AIO_INTRINSICS */
#    define AIO_CHECK_EVENT                                                                                            \
        if (0 > --sim_asynch_check) {                                                                                  \
            AIO_UPDATE_QUEUE;                                                                                          \
            sim_asynch_check = sim_asynch_inst_latency;                                                                \
        } else                                                                                                         \
            (void)0
#    define AIO_SET_INTERRUPT_LATENCY(instpersec)                                                                      \
        do {                                                                                                           \
            sim_asynch_inst_latency = (int32_t)((((double)(instpersec)) * sim_asynch_latency) / 1000000000);           \
            if (sim_asynch_inst_latency == 0)                                                                          \
                sim_asynch_inst_latency = 1;                                                                           \
        } while (0)
#endif /* SIM_AIO_H_ */
