// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet reader/writer dispatch functions - one per API type */

#include "sim_ether.h"
#include "sim_ether_internal.h"
#include "sim_sock.h"
#include "eth_backends/eth_backends.h"
#include "eth_backends/eth_dispatch.h"

/*============================================================================*/
/*                         Writer Dispatch Functions                          */
/*============================================================================*/

int eth_writer_pcap(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(USE_READER_THREAD) && defined(HAVE_PCAP_NETWORK)
    return pcap_sendpacket(dev->backend.state.pcap, (u_char *)packet->msg, packet->len);
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}

int eth_writer_tap(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(USE_READER_THREAD) && defined(HAVE_TAP_NETWORK)
    return (((int)packet->len == write(dev->fd_handle, (void *)packet->msg, packet->len)) ? 0 : -1);
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}

int eth_writer_vde(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(USE_READER_THREAD) && defined(HAVE_VDE_NETWORK)
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

int eth_writer_nat(ETH_DEV *dev, const ETH_PACK *packet)
{
#if defined(USE_READER_THREAD) && defined(HAVE_SLIRP_NETWORK)
    sim_slirp_network *slirp = dev->backend.state.slirp;
    int status;

    status = sim_slirp_send(slirp, (char *)packet->msg, (size_t)packet->len, 0);

    return ((status == packet->len) ? 0 : 1);
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
}

int eth_writer_udp(ETH_DEV *dev, const ETH_PACK *packet)
{
    return (((int32_t)packet->len == sim_write_sock(dev->fd_handle, (char *)packet->msg, (int32_t)packet->len)) ? 0
                                                                                                                : -1);
}

int eth_writer_none(ETH_DEV *dev, const ETH_PACK *packet)
{
    (void)dev;
    (void)packet;
    return -1; /* Error: no API configured */
}

int eth_writer_test(ETH_DEV *dev, const ETH_PACK *packet)
{
    (void)dev;
    (void)packet;
    return 0; /* Test API handles writes differently */
}

/*============================================================================*/
/*                         Reader Dispatch Functions                          */
/*============================================================================*/

int eth_reader_pcap(eth_backend_t *backend, ETH_DEV *dev)
{
    (void)dev;

#if defined(USE_READER_THREAD) && defined(HAVE_PCAP_NETWORK)
    return pcap_dispatch(backend->state.pcap, -1, _eth_callback, (u_char *)dev);
#else
    (void)backend;
    return 0;
#endif
}

int eth_reader_tap(eth_backend_t *backend, ETH_DEV *dev)
{
#if defined(USE_READER_THREAD) && defined(HAVE_TAP_NETWORK)
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
#else
    return 0;
#endif
}

int eth_reader_vde(eth_backend_t *backend, ETH_DEV *dev)
{
#if defined(USE_READER_THREAD) && defined(HAVE_VDE_NETWORK)
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
#else
    (void)backend;
    (void)dev;
    return 0;
#endif
}

int eth_reader_nat(eth_backend_t *backend, ETH_DEV *dev)
{
#if defined(USE_READER_THREAD) && defined(HAVE_SLIRP_NETWORK)
    sim_slirp_network *slirp = dev->backend.state.slirp;

    /* The mutex serializes the reader and the writer threads. */
    pthread_mutex_lock(&slirp->libslirp_lock);
    slirp_pollfds_poll(slirp->slirp_cxn, 0, slirp_get_events_callback, slirp);
    pthread_mutex_unlock(&slirp->libslirp_lock);

    /* slirp_pollfds_poll() is void, so we can't tell from its return value
     * whether packets arrived. But packets delivered via _slirp_callback()
     * are queued to dev->read_queue, so check if the queue is non-empty. */
    return sim_tailq_empty(&dev->read_queue) ? 0 : 1;
#else
    (void)backend;
    (void)dev;
    return 0;
#endif
}

int eth_reader_udp(eth_backend_t *backend, ETH_DEV *dev)
{
#if defined(USE_READER_THREAD)
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
#else
    (void)backend;
    (void)dev;
    return 0;
#endif
}

int eth_reader_none(eth_backend_t *backend, ETH_DEV *dev)
{
    (void)backend;
    (void)dev;
    return -1; /* No backend. */
}

int eth_reader_test(eth_backend_t *backend, ETH_DEV *dev)
{
    (void)backend;
    (void)dev;
    return 0; /* Test API handles reads differently */
}
