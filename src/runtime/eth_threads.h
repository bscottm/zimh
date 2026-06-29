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

#endif /* USE_READER_THREAD */

#endif /* ETH_THREADS_H */
