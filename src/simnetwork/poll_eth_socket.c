// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet reader thread with state machine control flow */

#if !ETH_THREADING_AVAILABLE
#    error "eth_threads.c MUST BE compiled with ETH_THREADING_AVAILABLE."
#endif

#include "sim_defs.h"
#include "sim_ether.h"
#include "sim_sock.h"
#include "sim_threads.h"
#include "poll_compat.h"

#include "sim_ether_internal.h"
#include "simnetwork/eth_backends.h"
#include "simnetwork/eth_threads.h"
#include "simnetwork/eth_dispatch.h"

#if SIM_USE_POLL
#    define POLL_NORMAL_EVENTS (POLLIN)
#    if !defined(_WIN32) && !defined(_WIN64)
#        define POLL_EXTRA_EVENTS (POLLPRI | POLLERR | POLLHUP)
#    else
         // Windows rejects POLLPRI, POLLERR, POLLHUP.
#        define POLL_EXTRA_EVENTS 0
#    endif
#endif

/*=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Poll the backend's `eth_socket` for read events, with a timeout, using either select() or poll() depending
 * on the platform. Common code across UDP, TAP, and VDE backends.
 * 
 * Returns:
 * > 0: One or more packets have arrived.
 *   0: No packet arrival, not an error.
 * < 0: Error waiting for packet arrival.
 *=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/
int poll_eth_socket(eth_backend_t *backend, long timeout_ms)
{
#if SIM_USE_SELECT
    fd_set setl;
    struct timeval timeout;

    FD_ZERO(&setl);
    FD_SET(backend->state.eth_socket, &setl);
    timeout.tv_sec = 0;
    timeout.tv_usec = timeout_ms * 1000;
    return select(backend->state.eth_socket + 1, &setl, NULL, NULL, &timeout);
#else
    sim_pollfd_t fds = {.fd = backend->state.eth_socket, .events = POLL_NORMAL_EVENTS | POLL_EXTRA_EVENTS, .revents = 0};
    return poll(&fds, 1, (sim_polltmo_t)timeout_ms);
#endif
}
