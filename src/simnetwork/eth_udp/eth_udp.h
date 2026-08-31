// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_UDP_TUNNEL_H)
#    define SIM_UDP_TUNNEL_H

#include "sim_defs.h"
#include "sim_ether.h"
#include "simnetwork/eth_backends.h"

/* UDP tunnel wait implementation */
int eth_wait_udp(eth_backend_t *backend, ETH_DEV *dev);
/* UDP tunnel reader */
int eth_reader_udp(eth_backend_t *backend, ETH_DEV *dev);
/* UDP tunnel writer */
int eth_writer_udp(ETH_DEV *dev, const ETH_PACK *packet);
#endif