// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet reader/writer dispatch functions - one per API type */

#include "sim_ether.h"
#include "sim_ether_internal.h"
#include "sim_sock.h"
#include "eth_dispatch.h"

/*============================================================================*/
/*                         Writer Dispatch Functions                          */
/*============================================================================*/

static int eth_writer_dispatch_pcap(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(HAVE_PCAP_NETWORK)
    return pcap_sendpacket(dev->backend.state.pcap, (u_char *)packet->msg, packet->len);
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}

static int eth_writer_dispatch_tap(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(HAVE_TAP_NETWORK)
    return (((int)packet->len == write(dev->fd_handle, (void *)packet->msg, packet->len)) ? 0 : -1);
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}

static int eth_writer_dispatch_vde(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(HAVE_VDE_NETWORK)
    int status = vde_send(dev->backend.state.vde, (void *)packet->msg, packet->len, 0);
    if ((status == (int)packet->len) || (status == 0))
        return 0;
    if ((status == -1) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
        return 0;
    return 1;
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}

static int eth_writer_dispatch_nat(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(HAVE_SLIRP_NETWORK)
    int status = sim_slirp_send(dev->backend.state.slirp, (char *)packet->msg, (size_t)packet->len, 0);
    return ((status == (int)packet->len) || (status == 0)) ? 0 : 1;
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}

static int eth_writer_dispatch_udp(ETH_DEV *dev, const ETH_PACK *packet)
{
    return (
        ((int32_t)packet->len == sim_write_sock(dev->fd_handle, (char *)packet->msg, (int32_t)packet->len))
            ? 0
            : -1);
}

static int eth_writer_dispatch_none(ETH_DEV *dev, const ETH_PACK *packet)
{
    (void)dev;
    (void)packet;
    return -1; /* Error: no API configured */
}

static int eth_writer_dispatch_test(ETH_DEV *dev, const ETH_PACK *packet)
{
    (void)dev;
    (void)packet;
    return 0; /* Test API handles writes differently */
}

/* Writer dispatch table - indexed by eth_api_t */
const eth_writer_dispatch_fn eth_writer_dispatch_table[ETH_API_COUNT] = {
    [ETH_API_NONE] = eth_writer_dispatch_none, [ETH_API_PCAP] = eth_writer_dispatch_pcap,
    [ETH_API_TAP] = eth_writer_dispatch_tap,   [ETH_API_VDE] = eth_writer_dispatch_vde,
    [ETH_API_UDP] = eth_writer_dispatch_udp,   [ETH_API_NAT] = eth_writer_dispatch_nat,
    [ETH_API_TEST] = eth_writer_dispatch_test};

#if defined(USE_READER_THREAD)

/*============================================================================*/
/*                         Reader Dispatch Functions                          */
/*============================================================================*/

static int eth_reader_dispatch_pcap(ETH_DEV *dev)
{
#    if defined(HAVE_PCAP_NETWORK)
    return pcap_dispatch(dev->backend.state.pcap, -1, &_eth_callback, (u_char *)dev);
#    else
    return 0;
#    endif
}

static int eth_reader_dispatch_tap(ETH_DEV *dev)
{
#    if defined(HAVE_TAP_NETWORK)
    struct pcap_pkthdr header;
    int len;
    u_char buf[ETH_MAX_JUMBO_FRAME];

    memset(&header, 0, sizeof(header));
    len = read(dev->fd_handle, buf, sizeof(buf));
    if (len > 0) {
        header.caplen = header.len = len;
        _eth_callback((u_char *)dev, &header, buf);
        return 1;
    }
    return (len < 0) ? -1 : 0;
#    else
    return 0;
#    endif
}

static int eth_reader_dispatch_vde(ETH_DEV *dev)
{
#    if defined(HAVE_VDE_NETWORK)
    struct pcap_pkthdr header;
    int len;
    u_char buf[ETH_MAX_JUMBO_FRAME];

    memset(&header, 0, sizeof(header));
    len = vde_recv(dev->backend.state.vde, buf, sizeof(buf), 0);
    if (len > 0) {
        header.caplen = header.len = len;
        _eth_callback((u_char *)dev, &header, buf);
        return 1;
    }
    return (len < 0) ? -1 : 0;
#    else
    (void)dev;
    return 0;
#    endif
}

static int eth_reader_dispatch_nat(ETH_DEV *dev)
{
#    if defined(HAVE_SLIRP_NETWORK)
    sim_debug(dev->dbit, dev->dptr, "NAT: eth_reader_dispatch_nat() called\n");
    sim_slirp_dispatch(dev->backend.state.slirp);
    sim_debug(dev->dbit, dev->dptr, "NAT: sim_slirp_dispatch() returned\n");
    return 1;
#    else
    return 0;
#    endif
}

static int eth_reader_dispatch_udp(ETH_DEV *dev)
{
    struct pcap_pkthdr header;
    int len;
    u_char buf[ETH_MAX_JUMBO_FRAME];

    memset(&header, 0, sizeof(header));
    len = (int)sim_read_sock(dev->fd_handle, (char *)buf, (int32_t)sizeof(buf));
    if (len > 0) {
        header.caplen = header.len = len;
        _eth_callback((u_char *)dev, &header, buf);
        return 1;
    }
    return (len < 0) ? -1 : 0;
}

static int eth_reader_dispatch_none(ETH_DEV *dev)
{
    (void)dev;
    return -1; /* Error: no API configured */
}

static int eth_reader_dispatch_test(ETH_DEV *dev)
{
    (void)dev;
    return 0; /* Test API handles reads differently */
}

/* Reader dispatch table - indexed by eth_api_type_t */
const eth_reader_dispatch_fn eth_reader_dispatch_table[ETH_API_COUNT] = {
    [ETH_API_NONE] = eth_reader_dispatch_none, [ETH_API_PCAP] = eth_reader_dispatch_pcap,
    [ETH_API_TAP] = eth_reader_dispatch_tap,   [ETH_API_VDE] = eth_reader_dispatch_vde,
    [ETH_API_UDP] = eth_reader_dispatch_udp,   [ETH_API_NAT] = eth_reader_dispatch_nat,
    [ETH_API_TEST] = eth_reader_dispatch_test};

#endif /* USE_READER_THREAD */
