// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(ETH_TEST_H)
#define ETH_TEST_H

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_ether.h"
#include "simnetwork/eth_types.h"
#include "simnetwork/eth_backends.h"

ETH_TEST_BACKEND *eth_test_find_backend(const char *name);
t_stat eth_test_get_backend(const char *name, ETH_TEST_BACKEND **backend);

/* Test Ethernet emulation select/poll wait API. */
int eth_wait_test(eth_backend_t *backend, ETH_DEV *dev, int timeout_ms);
/* Test Ethernet interface read API. */
int eth_test_read(ETH_DEV *dev, ETH_PACK *packet, ETH_PCALLBACK routine);
/* Test Ethernet interface write API. */
t_stat eth_test_write(ETH_DEV *dev, ETH_PACK *packet, ETH_PCALLBACK routine);

#endif /* ETH_TEST_H */
