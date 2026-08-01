// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet processing threads implementation */

#ifndef ETH_THREADS_H
#define ETH_THREADS_H

#include "sim_defs.h"

/* Forward declaration */
struct eth_device;
typedef struct eth_device ETH_DEV;

/* Check if threading support is available at compile time */
#if defined(HAVE_PTHREAD) || defined(_WIN32)
#  define ETH_THREADING_AVAILABLE 1
#else
#  define ETH_THREADING_AVAILABLE 0
#endif

#if ETH_THREADING_AVAILABLE

/* Threads abstractions. */
#include "sim_threads.h"

/* Reader thread entry point */
THREAD_FUNC_DEFN(_eth_reader);
/* Writer thread entry point */
THREAD_FUNC_DEFN(_eth_writer);

/* Initialize threading structures (queues, mutexes) without starting threads.
 * Always called during eth_open() when ETH_THREADING_AVAILABLE. */
t_stat eth_init_threading_structures(ETH_DEV *dev);

/* Start reader/writer threads.
 * Called when switching to async mode. */
t_stat eth_start_threads(ETH_DEV *dev);

/* Stop threads gracefully.
 * Called when switching to sync mode or during eth_close(). */
void eth_stop_threads(ETH_DEV *dev);

/* Clean up threading structures.
 * Called during eth_close() after threads are stopped. */
void eth_destroy_threading_structures(ETH_DEV *dev);

#endif /* ETH_THREADING_AVAILABLE */

#endif /* ETH_THREADS_H */
