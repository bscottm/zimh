// SPDX-FileCopyrightText: 1993-2016 Robert M Supnik
// SPDX-License-Identifier: X11

/* Asynchronous I/O definitions and support */

#if !defined(SIM_AIO_H)
#    define SIM_AIO_H

#    include <stdint.h>
#    include <stdbool.h>

/* Forward declarations - these are fully defined in sim_defs.h */
#ifndef SIM_DEFS_H_
typedef struct UNIT UNIT;
typedef int32_t t_stat;
typedef t_stat (*ACTIVATE_API)(UNIT *unit, int32_t interval);
#endif

#    if defined(SIM_ASYNCH_IO)
#        include "sim_atomic.h"
#        include "sim_atomic_ptr.h"
#        include "sim_threads.h"

#        define SIM_ASYNCH_CLOCKS 1

extern sim_mutex_t sim_asynch_lock;
extern sim_cond_t sim_asynch_wake;
extern sim_mutex_t sim_debug_lock;
extern sim_mutex_t sim_timer_lock;
extern sim_cond_t sim_timer_wake;
extern bool sim_timer_event_canceled;
extern int32_t sim_tmxr_poll_count;
extern sim_cond_t sim_tmxr_poll_cond;
extern sim_mutex_t sim_tmxr_poll_lock;
extern sim_thread_t sim_asynch_main_threadid;
/* Pending UNIT AIO "events", atomic UNIT pointer. */
extern sim_atomic_ptr_t sim_asynch_queue;
/* Atomic masquerading as a bool. */
extern sim_atomic_value_t sim_idle_wait;
/* Pending countdown to periodic pending event check. */
extern int32_t sim_asynch_check;
extern int32_t sim_asynch_latency;
/* Instruction latency penalty between async event checks. */
extern int32_t sim_asynch_inst_latency;

/* Thread-local storage for asynchronous I/O diagnostics.

   The project baseline assumes a compiler with at least C11 support, so
   `_Thread_local` is the normal C spelling here.  MSVC is the remaining
   exception: its C11/C17 mode still documents `_Thread_local` as recognized
   but unsupported, so keep using `__declspec(thread)` there for now.

   This branch should be removable once our supported Windows C toolchain has
   portable `_Thread_local` support in C mode. */
#        if defined(_MSC_VER)
#            define AIO_TLS __declspec(thread)
#        else
#            define AIO_TLS _Thread_local
#        endif

#        if defined(SIM_ASYNCH_MUX)
#            define AIO_CANCEL(uptr)                                                                                   \
                if (((uptr)->dynflags & UNIT_TM_POLL) && !((uptr)->next) && !((uptr)->a_next)) {                       \
                    (uptr)->a_polling_now = false;                                                                     \
                    sim_tmxr_poll_count -= (uptr)->a_poll_waiter_count;                                                \
                    (uptr)->a_poll_waiter_count = 0;                                                                   \
                }
#        endif /* defined(SIM_ASYNCH_MUX) */

#        if !defined(AIO_CANCEL)
#            define AIO_CANCEL(uptr)
#        endif /* !defined(AIO_CANCEL) */

#        define AIO_EVENT_BEGIN(uptr)                                                                                  \
            do {                                                                                                       \
            int __was_poll = uptr->dynflags & UNIT_TM_POLL

#        define AIO_EVENT_COMPLETE(uptr, reason)                                                                       \
            if (__was_poll) {                                                                                          \
                sim_mutex_lock(&sim_tmxr_poll_lock);                                                                   \
                uptr->a_polling_now = false;                                                                           \
                if (uptr->a_poll_waiter_count) {                                                                       \
                    sim_tmxr_poll_count -= uptr->a_poll_waiter_count;                                                  \
                    uptr->a_poll_waiter_count = 0;                                                                     \
                    if (0 == sim_tmxr_poll_count)                                                                      \
                        sim_cond_broadcast(&sim_tmxr_poll_cond);                                                       \
                }                                                                                                      \
                sim_mutex_unlock(&sim_tmxr_poll_lock);                                                                 \
            }                                                                                                          \
            sim_aio_update_queue();                                                                                    \
            }                                                                                                          \
            while (0)

#            define AIO_QUEUE_MODE "Lock-free Asynchronous I/O mode"

/* AIO queue sanity check. */
void aio_queue_check(sim_atomic_ptr_t *queue, sim_mutex_t *lock);

#    else /* !SIM_ASYNCH_IO */
#        define AIO_QUEUE_MODE "Direct I/O mode"
#        define AIO_EVENT_BEGIN(uptr)
#        define AIO_EVENT_COMPLETE(uptr, reason)
#        define AIO_CANCEL(uptr)
#        define AIO_TLS
#    endif /* SIM_ASYNCH_IO */

/* AIO initialization. Can be called even when SIM_ASYNCH_IO isn't defined. */
t_stat aio_init();
/* AIO cleanup. Can be called when SIM_ASYNCH_IO isn't defined */
t_stat aio_cleanup();

/* Initialize AIO fields in a UNIT structure. Should be called during device reset. */
void aio_init_unit(UNIT *uptr);

/* Ensure that AIO processing takes place solely within the simulator's thread context, calling `abort`
 * and terminating if not in the simulator's thread. */
void aio_in_main_thread(UNIT *unit);

/* Remove pending UNIT events from the sim_asynch_queue, call the UNIT's activation function to
 * process the event. Processes all pending events */
int sim_aio_update_queue(void);

/* Enqueue a new UNIT event onto sim_asynch_queue. */
void sim_aio_activate (ACTIVATE_API caller, UNIT *uptr, int32_t event_time);

/* Simulator thread predicate. */
static inline bool aio_main_thread()
{
#    if defined(SIM_ASYNCH_IO)
    return sim_thread_equal(sim_thread_self(), sim_asynch_main_threadid);
#    else
    return true;
#    endif
}

/* Check if the event check countdown has expired and process events if needed. */
static inline void aio_check_event()
{
#    if defined(SIM_ASYNCH_IO)
    if (0 > --sim_asynch_check) {
        sim_aio_update_queue();
        sim_asynch_check = sim_asynch_inst_latency;
    }
#    endif
}

/* Update the instruction latency penalty from the simulator's instructions/sec. */
static inline void aio_set_interrupt_latency(uint32_t instpersec)
{
#    if defined(SIM_ASYNCH_IO)
    uintmax_t ips = (uintmax_t)instpersec;
    uintmax_t latency = (uintmax_t)sim_asynch_latency;
    const uintmax_t nsec_to_cycles = 1000000000;
    
    sim_asynch_inst_latency = (int32_t)((ips * latency) / nsec_to_cycles);
    if (sim_asynch_inst_latency == 0)
        sim_asynch_inst_latency = 1;
#    endif
}

/* Unit has AIO pending?
 *
 * NOTE: Was the AIO_IS_ACTIVE macro.
 */
bool unit_has_aio_pending(UNIT *uptr);

/* Acquire the debug lock to serialize debugging output. */
static inline void aio_debug_lock(void)
{
#    if defined(SIM_ASYNCH_IO)
    sim_mutex_lock(&sim_debug_lock);
#    endif
}

/* Release the debug lock. */
static inline void aio_debug_unlock(void)
{
#    if defined(SIM_ASYNCH_IO)
    sim_mutex_unlock(&sim_debug_lock);
#    endif
}
#endif /* SIM_AIO_H */
