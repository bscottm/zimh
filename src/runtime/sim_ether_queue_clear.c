// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_ether.h"
#include "sim_threads.h"

/* Clear read and write queues without stopping threads.
 *
 * This is useful during device reset when you want to discard pending packets
 * but keep threads running. In contrast, eth_clr_async() stops threads entirely.
 *
 * Thread-safe: Can be called while reader/writer threads are running.
 */
void eth_clear_queues(ETH_DEV *dev)
{
    if (dev == NULL)
        return;

    /* Clear read queue (lock-free SPSC queue - single producer, single consumer) */
    eth_tailq_clear(&dev->read_queue);

    /* Clear write queue - need mutex since multiple producers possible */
    if (dev->threading_initialized) {
        sim_mutex_lock(&dev->writer_lock);
        eth_tailq_clear(&dev->write_requests);
        sim_mutex_unlock(&dev->writer_lock);
    }
}
