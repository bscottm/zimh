// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIMEVENTS_SIM_WALLCLOCK_H)
#    define SIMEVENTS_SIM_WALLCLOCK_H

#    include "sim_defs.h"

typedef struct sim_event_heap {
    size_t last; /* Last heap element */
    size_t capacity; /* Total heap capacity */
    SIM_UNIT_EVENT *events;     /* The actual events */
} SIM_EVENT_HEAP;

/* Wallclock queue initialization. */
void sim_wallclock_init();

/* Show the queues. Note: Can be called if SIM_ASYNCH_CLOCKS isn't defined. */
void sim_show_wallclock_queue(FILE *st, DEVICE *dptr, UNIT *uptr);

/* Start the wallclock thread */
void sim_start_wallclock_thread_start();

#endif
