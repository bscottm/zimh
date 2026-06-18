// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet reader thread implementation */

#ifndef ETH_READERS_H
#define ETH_READERS_H

#include "sim_ether.h"

#if defined(USE_READER_THREAD)

/* Threads abstractions. */
#include "sim_threads.h"

/* Reader thread entry point */
THREAD_FUNC_DEFN(_eth_reader);

#endif /* USE_READER_THREAD */

#endif /* ETH_READERS_H */
