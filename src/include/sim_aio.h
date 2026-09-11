// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

//
#if !defined(SIM_AIO_H_)
#    define SIM_AIO_H_ 1

#    include <stdint.h>

#    include "sim_defs.h"
#    include "sim_threads.h"
#    include "sim_atomic.h"

#    define SIM_ASYNCH_CLOCKS 1

extern sim_mutex_t sim_asynch_lock;
extern sim_cond_t sim_asynch_wake;
extern sim_mutex_t sim_timer_lock;
extern sim_cond_t sim_timer_wake;
extern bool sim_timer_event_canceled;
extern int32_t sim_tmxr_poll_count;
extern sim_cond_t sim_tmxr_poll_cond;
extern sim_mutex_t sim_tmxr_poll_lock;
extern pthread_t sim_asynch_main_threadid;
// FIXME: Replace with sim_tailq_t:
extern UNIT *volatile sim_asynch_queue;
extern volatile bool sim_idle_wait;
extern int32_t sim_asynch_check;
extern int32_t sim_asynch_latency;
extern int32_t sim_asynch_inst_latency;

extern bool aio_queue_check(UNIT *volatile queue, sim_mutex_t *lock);

/* Executing in the simulator's thread? */
static inline bool is_simulator_thread() {
    return sim_thread_equal(sim_thread_self(), sim_asynch_main_threadid);
}

#    define AIO_LOCK sim_mutex_lock(&sim_asynch_lock)
#    define AIO_UNLOCK sim_mutex_unlock(&sim_asynch_lock)
#    define AIO_IS_ACTIVE(uptr) (((uptr)->a_is_active ? (uptr)->a_is_active(uptr) : false) || ((uptr)->a_next))
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
#    define AIO_EVENT_BEGIN(uptr)                                                                                      \
        do {                                                                                                           \
            int __was_poll = uptr->dynflags & UNIT_TM_POLL
#    define AIO_EVENT_COMPLETE(uptr, reason)                                                                           \
        if (__was_poll) {                                                                                              \
            sim_mutex_lock(&sim_tmxr_poll_lock);                                                                       \
            uptr->a_polling_now = false;                                                                               \
            if (uptr->a_poll_waiter_count) {                                                                           \
                sim_tmxr_poll_count -= uptr->a_poll_waiter_count;                                                      \
                uptr->a_poll_waiter_count = 0;                                                                         \
                if (0 == sim_tmxr_poll_count)                                                                          \
                    sim_cond_broadcast(&sim_tmxr_poll_cond);                                                           \
            }                                                                                                          \
            sim_mutex_unlock(&sim_tmxr_poll_lock);                                                                     \
        }                                                                                                              \
        AIO_UPDATE_QUEUE;                                                                                              \
        }                                                                                                              \
        while (0)

#    if defined(_WIN32) || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
#        define USE_AIO_INTRINSICS 1
#    endif
/* Provide a way to test both Intrinsic and Lock based queue manipulations  */
/* when both are available on a particular platform                         */
#    if defined(DONT_USE_AIO_INTRINSICS) && defined(USE_AIO_INTRINSICS)
#        undef USE_AIO_INTRINSICS
#    endif
#    ifdef USE_AIO_INTRINSICS
/* This approach uses intrinsics to manage access to the link list head     */
/* sim_asynch_queue.  This implementation is a completely lock free design  */
/* which avoids the potential ABA issues.                                   */
#        define AIO_QUEUE_MODE "Lock free asynchronous event queue"
#        define AIO_INIT                                                                                               \
            do {                                                                                                       \
                sim_asynch_main_threadid = sim_thread_self();                                                          \
                /* Empty list/list end uses the point value (void *)1.                                                 \
                   This allows NULL in an entry's a_next pointer to                                                    \
                   indicate that the entry is not currently in any list */                                             \
                sim_asynch_queue = QUEUE_LIST_END;                                                                     \
            } while (0)
#        define AIO_CLEANUP                                                                                            \
            do {                                                                                                       \
                sim_mutex_destroy(&sim_asynch_lock);                                                                   \
                sim_cond_destroy(&sim_asynch_wake);                                                                    \
                sim_mutex_destroy(&sim_timer_lock);                                                                    \
                sim_cond_destroy(&sim_timer_wake);                                                                     \
                sim_mutex_destroy(&sim_tmxr_poll_lock);                                                                \
                sim_cond_destroy(&sim_tmxr_poll_cond);                                                                 \
            } while (0)
#        ifdef _WIN32
#        elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
#            define InterlockedCompareExchangePointer(Destination, Exchange, Comparand)                                \
                __sync_val_compare_and_swap(Destination, Comparand, Exchange)
#        else
#            error                                                                                                     \
                "Implementation of function InterlockedCompareExchangePointer() is needed to build with USE_AIO_INTRINSICS"
#        endif
#        define AIO_ILOCK AIO_LOCK
#        define AIO_IUNLOCK AIO_UNLOCK
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
#        define AIO_INIT                                                                                               \
            do {                                                                                                       \
                pthread_mutexattr_t attr;                                                                              \
                                                                                                                       \
                pthread_mutexattr_init(&attr);                                                                         \
                pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);                                             \
                pthread_mutex_init(&sim_asynch_lock, &attr);                                                           \
                pthread_mutexattr_destroy(&attr);                                                                      \
                sim_asynch_main_threadid = pthread_self();                                                             \
                /* Empty list/list end uses the point value (void *)1.                                                 \
                   This allows NULL in an entry's a_next pointer to                                                    \
                   indicate that the entry is not currently in any list */                                             \
                sim_asynch_queue = QUEUE_LIST_END;                                                                     \
            } while (0)
#        define AIO_CLEANUP                                                                                            \
            do {                                                                                                       \
                pthread_mutex_destroy(&sim_asynch_lock);                                                               \
                pthread_cond_destroy(&sim_asynch_wake);                                                                \
                pthread_mutex_destroy(&sim_timer_lock);                                                                \
                pthread_cond_destroy(&sim_timer_wake);                                                                 \
                pthread_mutex_destroy(&sim_tmxr_poll_lock);                                                            \
                pthread_cond_destroy(&sim_tmxr_poll_cond);                                                             \
            } while (0)
#        define AIO_ILOCK AIO_LOCK
#        define AIO_IUNLOCK AIO_UNLOCK
#        define AIO_QUEUE_VAL sim_asynch_queue
#        define AIO_QUEUE_SET(newval, oldval) ((sim_asynch_queue = newval), oldval)
#        define AIO_UPDATE_QUEUE sim_aio_update_queue()
#        define AIO_ACTIVATE(caller, uptr, event_time)                                                                 \
            if (!is_simulator_thread()) {                                                                              \
                sim_debug(SIM_DBG_AIO_QUEUE, sim_dflt_dev, "Queueing Asynch event for %s after %d instructions\n",     \
                          sim_uname(uptr), event_time);                                                                \
                AIO_LOCK;                                                                                              \
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
                        AIO_UNLOCK;                                                                                    \
                        sim_debug(TIMER_DBG_IDLE, &sim_timer_dev, "waking due to event on %s after %d instructions\n", \
                                  sim_uname(uptr), event_time);                                                        \
                        AIO_LOCK;                                                                                      \
                    }                                                                                                  \
                    pthread_cond_signal(&sim_asynch_wake);                                                             \
                }                                                                                                      \
                AIO_UNLOCK;                                                                                            \
                return SCPE_OK;                                                                                        \
            } else                                                                                                     \
                (void)0
#    endif /* USE_AIO_INTRINSICS */
#    define AIO_VALIDATE(uptr)                                                                                         \
        if (!is_simulator_thread()) {                                                                                  \
            sim_printf("Improper thread context for operation on %s in %s line %d\n", sim_uname(uptr), __FILE__,       \
                       __LINE__);                                                                                      \
            abort();                                                                                                   \
        } else                                                                                                         \
            (void)0
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
