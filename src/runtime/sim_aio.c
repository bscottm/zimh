// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_threads.h"

#if defined(SIM_ASYNCH_IO)
sim_mutex_t sim_asynch_lock;
sim_cond_t sim_asynch_wake;

sim_mutex_t sim_timer_lock;
sim_cond_t sim_timer_wake;
sim_mutex_t sim_tmxr_poll_lock;
sim_cond_t sim_tmxr_poll_cond;

int32_t sim_tmxr_poll_count;

sim_thread_t sim_asynch_main_threadid;

UNIT * volatile sim_asynch_queue;
bool sim_asynch_enabled = true;
int32_t sim_asynch_check;
int32_t sim_asynch_latency = 4000;    /* 4 usec interrupt latency */
int32_t sim_asynch_inst_latency = 20; /* assume 5 mip simulator */
#else
bool sim_asynch_enabled = false;
#endif

t_stat aio_init()
{
#if defined(SIM_ASYNCH_IO)
    sim_mutex_recursive(&sim_asynch_lock);
    sim_cond_init(&sim_asynch_wake);

    sim_mutex_init(&sim_timer_lock);
    sim_cond_init(&sim_timer_wake);

    sim_mutex_init(&sim_tmxr_poll_lock);
    sim_cond_init(&sim_tmxr_poll_cond);

    sim_asynch_main_threadid = sim_thread_self();

    sim_asynch_queue = QUEUE_LIST_END;
#endif

    return SCPE_OK;
}

t_stat aio_cleanup()
{
#if defined(SIM_ASYNCH_IO)
    sim_mutex_destroy(&sim_asynch_lock);
    sim_cond_destroy(&sim_asynch_wake);

    sim_mutex_destroy(&sim_timer_lock);
    sim_cond_destroy(&sim_timer_wake);

    sim_mutex_destroy(&sim_tmxr_poll_lock);
    sim_cond_destroy(&sim_tmxr_poll_cond);
#endif

    return SCPE_OK;
}
