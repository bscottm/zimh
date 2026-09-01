// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "simnetwork/eth_pcap/eth_pcap.h"

/* PCAP wait implementation */
int eth_wait_pcap(eth_backend_t *backend, ETH_DEV *dev, int timeout_ms)
{
    (void)dev;
#if defined(_WIN32)
    /* Windows: Use event-based waiting */
    return (WAIT_OBJECT_0 == WaitForSingleObject(pcap_getevent(backend->state.pcap), timeout_ms) ? 1 : 0);
#else
    return poll_eth_socket(backend, timeout_ms);
#endif
}

/* libpcap's reader callback. */
static void pcap_eth_callback(u_char *info, const struct pcap_pkthdr *header, const uint8_t *data)
{
    eth_process_received_packet((ETH_DEV *)info, data, header->len, header->caplen);
}

int eth_reader_pcap(eth_backend_t *backend, ETH_DEV *dev)
{
    if (dev == NULL) {
        return 0;
    }

#if ETH_THREADING_AVAILABLE
    if (backend == NULL || backend->state.pcap == NULL) {
        return 0;
    }

    return pcap_dispatch(backend->state.pcap, -1, pcap_eth_callback, (u_char *)dev);
#else
    (void)backend;
    return 0;
#endif
}

int eth_writer_pcap(ETH_DEV *dev, const ETH_PACK *packet)
{
#if ETH_THREADING_AVAILABLE
    return pcap_sendpacket(dev->backend->state.pcap, (u_char *)packet->msg, packet->len);
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}
