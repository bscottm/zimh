// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet writer thread implementation */

#ifndef ETH_WRITERS_H
#define ETH_WRITERS_H

#include "sim_ether.h"

#if defined(USE_READER_THREAD)

#include "sim_threads.h"

/* Writer thread entry point */
THREAD_FUNC_DEFN(_eth_writer);

#endif /* USE_READER_THREAD */

#endif /* ETH_WRITERS_H */
