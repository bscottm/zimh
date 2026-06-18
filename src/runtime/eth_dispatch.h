// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet dispatch tables and function types */

#ifndef ETH_DISPATCH_H
#define ETH_DISPATCH_H

#include "sim_ether.h"

/* Dispatch tables - one entry per API type, indexed by eth_api_t */
extern const eth_reader_dispatch_fn eth_reader_dispatch_table[ETH_API_COUNT];
extern const eth_writer_dispatch_fn eth_writer_dispatch_table[ETH_API_COUNT];

#endif /* ETH_DISPATCH_H */
