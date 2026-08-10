/* sim_ether_internal.h: private helpers shared within Ethernet support */
// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#ifndef SIM_ETHER_INTERNAL_H
#define SIM_ETHER_INTERNAL_H

#include <stdint.h>

#include "sim_ether.h"

/*
 * This header is for private Ethernet helpers that are shared by multiple
 * Ethernet implementation files, but are not part of the public simulator
 * Ethernet API. Declarations belong here when they describe common sim_ether
 * behavior, policy, or data interpretation needed by more than one backend.
 *
 * Backend-specific entry points belong in that backend's private header, and
 * public test-harness controls belong in sim_ether_test.h.
 */

/* Return whether a non-BPF receive path should deliver a packet to dev. */
bool eth_packet_matches_filter(ETH_DEV *dev, const uint8_t *data);

/* Internal functions used by dispatch tables and test backend.
 * Callers must include <pcap.h> before this header when HAVE_PCAP_NETWORK is defined. */
#if defined(HAVE_PCAP_NETWORK)
/* Forward declaration at file scope to ensure type compatibility.
 * The full definition comes from <pcap.h> which callers must include first. */
struct pcap_pkthdr;
void eth_callback(u_char* info, const struct pcap_pkthdr* header, const uint8_t *data);
#endif

t_stat _eth_write(ETH_DEV* dev, ETH_PACK* packet, ETH_PCALLBACK routine);
void _eth_error(ETH_DEV* dev, const char* where);

#endif
