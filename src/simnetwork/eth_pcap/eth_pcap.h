// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(ETH_PCAP_H)
#define ETH_PCAP_H

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_ether.h"
#include "simnetwork/eth_types.h"
#include "simnetwork/eth_backends.h"

int eth_wait_pcap(eth_backend_t *backend, ETH_DEV *dev, int timeout_ms);
int eth_reader_pcap(eth_backend_t *backend, ETH_DEV *dev);
int eth_writer_pcap(ETH_DEV *dev, const ETH_PACK *packet);

#endif /* ETH_PCAP_H */
