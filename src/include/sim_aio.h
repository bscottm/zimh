// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

//
#if !defined(SIM_AIO_H_)
#    define SIM_AIO_H_ 1

#    include <stdint.h>

#    include "sim_defs.h"
#    include "sim_threads.h"
#    include "sim_atomic.h"
#    include "sim_atomic_ptr.h"

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

extern sim_atomic_value_t sim_asynch_check;
extern int32_t sim_asynch_latency;
extern int32_t sim_asynch_inst_latency;

/* Async I/O queue sanity check function. (Note: Potentially not used.) */
extern bool aio_queue_check(UNIT *volatile queue, sim_mutex_t *lock);

/* Async I/O enabled and active? predicate */
static inline bool aio_enabled_and_active() {
    return (sim_async_preference && sim_asynch_enabled);
}

/* Get the simulator's asynchronous preference */
static inline const bool aio_async_preference() {
    return sim_async_preference;
}

/* Get the async enabled flag. */
static inline const bool aio_async_enabled() {
    return sim_asynch_enabled;
}

static inline void aio_set_async_enabled(const bool flag) {
    sim_asynch_enabled = flag;
}

    
/* Executing in the simulator's thread? */
static inline bool is_simulator_thread() {
    return sim_thread_equal(sim_thread_self(), sim_asynch_main_threadid);
}

/* Does the unit have pending asynchronous I/O?
 * NOTE: With the new MPSC queue, we can no longer check if a unit has pending
 * async I/O by looking at intrusive list pointers. Units must provide an
 * a_is_active callback if they need this functionality. */
static inline bool is_unit_aio_active(const UNIT *unit) {
    return (unit->a_is_active != NULL ? unit->a_is_active(unit) : false);
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

static inline void aio_check_event() {
    sim_atomic_type_t checkval = sim_atomic_dec(&sim_asynch_check);
    if (0 > checkval) {
        sim_atomic_put(&sim_asynch_check, sim_asynch_inst_latency);
        sim_aio_update_queue();
    }

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

/* Notes on macro replacements:
 *
 * AIO_CHECK_EVENT -> aio_check_event()
 * AIO_UPDATE_QUEUE -> sim_aio_update_queue()
 * AIO_ACTIVATE -> sim_aio_activate()
 * AIO_LOCK, AIO_ILOCK -> aio_global_lock()
 * AIO_LOCK_UNLOCK, AIO_IUNLOCK -> aio_global_unlock()
 */
#    if defined(SIM_ASYNCH_MUX)
#        define AIO_CANCEL(uptr)                                                                                       \
            if (((uptr)->dynflags & UNIT_TM_POLL) && !((uptr)->next)) {                                                \
                (uptr)->a_polling_now = false;                                                                         \
                sim_tmxr_poll_count -= (uptr)->a_poll_waiter_count;                                                    \
                (uptr)->a_poll_waiter_count = 0;                                                                       \
            }
#    endif /* defined(SIM_ASYNCH_MUX) */
#    if !defined(AIO_CANCEL)
#        define AIO_CANCEL(uptr)
#    endif /* !defined(AIO_CANCEL) */

#    ifdef USE_AIO_INTRINSICS
#        define AIO_ILOCK
#        define AIO_IUNLOCK
#    else
#        define AIO_ILOCK aio_global_lock()
#        define AIO_IUNLOCK aio_global_unlock()
#    endif /* USE_AIO_INTRINSICS */

/* AIO_ACTIVATE macro - calls sim_aio_activate() from async threads */
#    define AIO_ACTIVATE(caller, uptr, event_time)                                                                 \
        if (!is_simulator_thread()) {                                                                              \
            sim_aio_activate((ACTIVATE_API)caller, uptr, event_time);                                              \
            return SCPE_OK;                                                                                        \
        } else                                                                                                     \
            (void)0

#    define AIO_SET_INTERRUPT_LATENCY(instpersec)                                                                      \
        do {                                                                                                           \
            sim_asynch_inst_latency = (int32_t)((((double)(instpersec)) * sim_asynch_latency) / 1000000000);           \
            if (sim_asynch_inst_latency == 0)                                                                          \
                sim_asynch_inst_latency = 1;                                                                           \
        } while (0)
#endif /* SIM_AIO_H_ */
