// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_platform.h"
#include "sim_threads.h"
#include "sim_aio.h"

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
