// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/*
 * Refactored wallclock queue code originally in sim_timer.c.
 */

#if defined(SIM_ASYNCH_CLOCKS) && !defined(SIM_ASYNCH_IO)
#    undef SIM_ASYNCH_CLOCKS
#endif

#include "sim_defs.h"
#include "sim_timer.h"

#if defined(SIM_ASYNCH_CLOCKS)
#    include "sim_threads.h"
#    include "simevents/sim_wallclock.h"

sim_atomic_ptr_t sim_wallclock_queue;
sim_atomic_ptr_t sim_wallclock_entry;

sim_thread_t sim_timer_thread;
sim_cond_t sim_timer_startup_cond;
bool sim_timer_thread_running = false;

static bool _sim_wallclock_cancel(UNIT *uptr);
static bool _sim_wallclock_is_active(UNIT *uptr);

static int sim_timespec_compare(struct timespec *a, struct timespec *b)
{
    while (a->tv_nsec >= 1000000000) {
        a->tv_nsec -= 1000000000;
        ++a->tv_sec;
    }
    while (b->tv_nsec >= 1000000000) {
        b->tv_nsec -= 1000000000;
        ++b->tv_sec;
    }
    if (a->tv_sec < b->tv_sec)
        return -1;
    if (a->tv_sec > b->tv_sec)
        return 1;
    if (a->tv_nsec < b->tv_nsec)
        return -1;
    if (a->tv_nsec > b->tv_nsec)
        return 1;
    else
        return 0;
}

static THREAD_FUNC_DEFN(_timer_thread)
{
    /* Generic thread start signature.
       This implementation does not use every parameter. */
    (void)arg;

    sim_set_io_thread_affinity();
    sim_set_thread_name("Wallclock thread");

    sim_debug(TIMER_DBG_TIM, &sim_timer_dev, "_timer_thread() - starting\n");

    sim_mutex_lock(&sim_timer_lock);
    sim_timer_thread_running = true;
    sim_cond_signal(&sim_timer_startup_cond); /* Signal we're ready to go */

    while (sim_asynch_timer && sim_is_running) {
        struct timespec start_time, stop_time;
        struct timespec due_time;
        double wait_usec;
        int32_t inst_delay;
        double inst_per_sec;
        UNIT *uptr;
        SIM_UNIT_EVENT *tptr, *cptr, *prvptr;

        if ((tptr = (SIM_UNIT_EVENT *) sim_atomic_ptr_get(&sim_wallclock_entry)) != NULL) { /* something to insert in queue? */
            sim_debug(TIMER_DBG_TIM, &sim_timer_dev, "_timer_thread() - timing %s for %s\n",
                      sim_uname(tptr->unit),
                      sim_fmt_secs(tptr->usec_delay / 1000000.0));

            sim_atomic_ptr_put(&sim_wallclock_entry, NULL);

            prvptr = NULL;
            for (cptr = (SIM_UNIT_EVENT *) sim_atomic_ptr_get(&sim_wallclock_queue); cptr != NULL; cptr = cptr->next) {
                if (tptr->due_time < cptr->due_time)
                    break;
                prvptr = cptr;
            }
            if (prvptr == NULL) { /* insert at head */
                tptr->next = sim_wallclock_queue;
                sim_wallclock_queue = tptr;
            } else {
                tptr->next = prvptr->next; /* insert at prvptr */
                prvptr->next = tptr;
            }
        }

        /* determine wait time */
        if (sim_wallclock_queue != NULL) {
            /* due time adjusted by 1/2 a minimal sleep interval */
            /* the goal being to let the last fractional part of the due time */
            /* be done by counting instructions */
            sim_double_to_timespec(&due_time, sim_wallclock_queue->due_time - (((double)sim_idle_rate_ms) * 0.0005));
        } else {
            due_time.tv_sec = 0x7FFFFFFF; /* Sometime when 32 bit time_t wraps */
            due_time.tv_nsec = 0;
        }
        (void)sim_clock_gettime(CLOCK_REALTIME, &start_time);
        wait_usec = floor(1000000.0 * (sim_timespec_to_double(&due_time) - sim_timespec_to_double(&start_time)));
        if (sim_wallclock_queue == NULL)
            sim_debug(TIMER_DBG_TIM, &sim_timer_dev, "_timer_thread() - waiting forever\n");
        else
            sim_debug(TIMER_DBG_TIM, &sim_timer_dev, "_timer_thread() - waiting for %.0f usecs until %.6f for %s\n",
                      wait_usec, sim_wallclock_queue->due_time, sim_uname(sim_wallclock_queue->unit));
        if ((wait_usec <= 0.0) || (0 != sim_cond_timedwait(&sim_timer_wake, &sim_timer_lock, &due_time))) {

            if (sim_wallclock_queue == NULL) /* queue empty? */
                continue;                    /* wait again */
            inst_per_sec = sim_timer_inst_per_sec();

            tptr = sim_wallclock_queue;
            sim_wallclock_queue = tptr->next;
            uptr = tptr->unit;
            free(tptr); /* Free the timer event */

            (void)sim_clock_gettime(CLOCK_REALTIME, &stop_time);
            if (1 != sim_timespec_compare(&due_time, &stop_time))
                inst_delay = 0;
            else
                inst_delay =
                    (int32_t)(inst_per_sec * (sim_timespec_to_double(&due_time) - sim_timespec_to_double(&stop_time)));
            sim_debug(TIMER_DBG_TIM, &sim_timer_dev, "_timer_thread() - slept %.0fms - activating(%s,%d)\n",
                      1000.0 * (sim_timespec_to_double(&stop_time) - sim_timespec_to_double(&start_time)), sim_uname(uptr),
                      inst_delay);
            sim_activate(uptr, inst_delay);
        } else { /* Something wants to adjust the queue since the wait condition was signaled */
        }
    }
    sim_timer_thread_running = false;
    sim_mutex_unlock(&sim_timer_lock);

    sim_debug(TIMER_DBG_TIM, &sim_timer_dev, "_timer_thread() - exiting\n");

    return THREAD_FUNC_RETURN(0);
}
#endif


void sim_wallclock_init()
{
#if defined(SIM_ASYNCH_CLOCKS)
    sim_atomic_ptr_init(&sim_wallclock_queue);
    sim_atomic_ptr_init(&sim_wallclock_entry);
#endif
}

void sim_show_wallclock_queue(FILE *st, DEVICE *dptr, UNIT *uptr)
{
    if (!sim_asynch_timer)
        return;

#if defined(SIM_ASYNCH_CLOCKS)
    sim_mutex_lock(&sim_timer_lock);
    const char *tim;
    struct timespec due;
    time_t time_t_due;

    if (sim_wallclock_queue == NULL)
        fprintf(st, "%s wall clock event queue empty\n", sim_name);
    else {
        SIM_UNIT_EVENT *tptr;
        fprintf(st, "%s wall clock event queue status\n", sim_name);
        for (tptr = sim_wallclock_queue; tptr != NULL; tptr = tptr->next) {
            uptr = tptr->unit;
            if ((dptr = find_dev_from_unit(uptr)) != NULL) {
                fprintf(st, "  %s", sim_dname(dptr));
                if (dptr->numunits > 1)
                    fprintf(st, " unit %d", (int32_t)(uptr - dptr->units));
            } else
                fprintf(st, "  Unknown");
            tim = sim_fmt_secs(tptr->usec_delay / 1000000.0);
            sim_double_to_timespec(&due, tptr->due_time);
            time_t_due = (time_t)due.tv_sec;
            {
                char time_buf[16];

                ;
                fprintf(st, " after %s due at %s.%06d\n", tim,
                        sim_timer_format_hms(time_t_due, time_buf, sizeof(time_buf)), (int)(due.tv_nsec / 1000));
            }
        }
    }

    sim_mutex_unlock(&sim_timer_lock);
#endif /* SIM_ASYNCH_CLOCKS */
}
