// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet processing threads implementation */

#ifndef ETH_THREADS_H
#define ETH_THREADS_H

#include "sim_ether.h"

#if defined(USE_READER_THREAD)

/* Threads abstractions. */
#include "sim_threads.h"

/* Reader thread entry point */
THREAD_FUNC_DEFN(_eth_reader);
/* Writer thread entry point */
THREAD_FUNC_DEFN(_eth_writer);

/* Initialize threading infrastructure and start reader/writer threads.
 * Requires that dev->backend is already initialized and queues/mutexes are set up.
 * Returns SCPE_OK on success, or an error code on failure. */
t_stat eth_start_threads(ETH_DEV *dev);

#endif /* USE_READER_THREAD */

#endif /* ETH_THREADS_H */
