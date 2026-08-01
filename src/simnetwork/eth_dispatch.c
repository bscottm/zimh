// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet reader/writer dispatch functions - one per API type */

#include "sim_ether.h"
#include "sim_ether_internal.h"
#include "sim_sock.h"
#include "simnetwork/eth_backends.h"
#include "simnetwork/eth_dispatch.h"

/*============================================================================*/
/*                         Writer Dispatch Functions                          */
/*============================================================================*/

int eth_writer_nat(ETH_DEV *dev, const ETH_PACK *packet)
{
#if ETH_THREADING_AVAILABLE && defined(HAVE_SLIRP_NETWORK)
    sim_slirp_network *slirp = dev->backend->state.slirp;
    int status;

    status = sim_slirp_send(slirp, (char *)packet->msg, (size_t)packet->len, 0);

    return ((status == packet->len) ? 0 : 1);
#else
    (void)dev;
    (void)packet;
    return 0;
#endif
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

int eth_reader_nat(eth_backend_t *backend, ETH_DEV *dev)
{
#if ETH_THREADING_AVAILABLE && defined(HAVE_SLIRP_NETWORK)
    sim_slirp_network *slirp = dev->backend->state.slirp;

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
