// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet reader thread with state machine control flow */

#if defined(USE_READER_THREAD)

#include "sim_ether.h"
#include "sim_threads.h"

/* Include API-specific headers BEFORE sim_ether_internal.h */
#ifdef HAVE_PCAP_NETWORK
#include <pcap.h>
#endif
#ifdef HAVE_VDE_NETWORK
#include <libvdeplug.h>
#endif
#ifdef HAVE_SLIRP_NETWORK
#include "sim_slirp.h"
#endif

#include "sim_ether_internal.h"
#include "eth_readers.h"
#include "eth_dispatch.h"

/*============================================================================*/
/*                    STATE HANDLER IMPLEMENTATIONS                          */
/*============================================================================*/

static eth_reader_state_t eth_reader_init_handler(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    sim_debug(dev->dbit, dev->dptr, "Reader Thread Starting\n");
    sim_os_set_thread_priority(PRIORITY_ABOVE_NORMAL);

    /* Determine select mode and file descriptors based on API type */
    switch (dev->eth_api) {
    case ETH_API_PCAP:
#if defined(HAVE_PCAP_NETWORK) && defined(MUST_DO_SELECT)
        ctx->do_select = 1;
        ctx->select_fd = pcap_get_selectable_fd((pcap_t *)dev->handle);
#endif
#if defined(_WIN32)
        ctx->hWait = pcap_getevent((pcap_t*)dev->handle);
#endif
        break;
    case ETH_API_TAP:
    case ETH_API_VDE:
    case ETH_API_UDP:
    case ETH_API_NAT:
        ctx->do_select = 1;
        ctx->select_fd = dev->fd_handle;
        break;
    case ETH_API_TEST:
    case ETH_API_NONE:
    default:
        /* No special setup for test/none APIs */
        break;
    }

    /* Signal that reader thread is ready */
    sim_mutex_lock(&dev->startup_lock);
    dev->threads_ready++;
    sim_cond_signal(&dev->startup_cond);
    sim_mutex_unlock(&dev->startup_lock);

    return ETH_READER_SELECT_WAIT;
}

/*============================================================================*/
/*                    SELECT/WAIT DISPATCH TABLE                             */
/*============================================================================*/

/* Forward declarations of API-specific wait handlers */
static int eth_wait_pcap(eth_reader_context_t *ctx);
static int eth_wait_tap(eth_reader_context_t *ctx);
static int eth_wait_vde(eth_reader_context_t *ctx);
static int eth_wait_udp(eth_reader_context_t *ctx);
static int eth_wait_nat(eth_reader_context_t *ctx);
static int eth_wait_test(eth_reader_context_t *ctx);
static int eth_wait_none(eth_reader_context_t *ctx);

/* Dispatch table indexed by eth_api_t */
typedef int (*eth_wait_fn)(eth_reader_context_t *ctx);
static const eth_wait_fn wait_dispatch_table[ETH_API_COUNT] = {
    [ETH_API_NONE] = eth_wait_none,
    [ETH_API_PCAP] = eth_wait_pcap,
    [ETH_API_TAP]  = eth_wait_tap,
    [ETH_API_VDE]  = eth_wait_vde,
    [ETH_API_UDP]  = eth_wait_udp,
    [ETH_API_NAT]  = eth_wait_nat,
    [ETH_API_TEST] = eth_wait_test
};

/* PCAP wait implementation */
static int eth_wait_pcap(eth_reader_context_t *ctx)
{
#if defined(_WIN32)
    /* Windows: Use event-based waiting */
    if (WAIT_OBJECT_0 == WaitForSingleObject(ctx->hWait, 250))
        return 1;
    return 0;
#else
    /* Unix: Use select if configured */
    if (ctx->do_select) {
        fd_set setl;
        struct timeval timeout;

        FD_ZERO(&setl);
        FD_SET(ctx->select_fd, &setl);
        timeout.tv_sec = 0;
        timeout.tv_usec = 250*1000;
        return select(1+ctx->select_fd, &setl, NULL, NULL, &timeout);
    }
    return 1;  /* No select needed, data always available */
#endif
}

/* TAP wait implementation */
static int eth_wait_tap(eth_reader_context_t *ctx)
{
    if (ctx->do_select) {
        fd_set setl;
        struct timeval timeout;

        FD_ZERO(&setl);
        FD_SET(ctx->select_fd, &setl);
        timeout.tv_sec = 0;
        timeout.tv_usec = 250*1000;
        return select(1+ctx->select_fd, &setl, NULL, NULL, &timeout);
    }
    return 1;
}

/* VDE wait implementation */
static int eth_wait_vde(eth_reader_context_t *ctx)
{
    if (ctx->do_select) {
        fd_set setl;
        struct timeval timeout;

        FD_ZERO(&setl);
        FD_SET(ctx->select_fd, &setl);
        timeout.tv_sec = 0;
        timeout.tv_usec = 250*1000;
        return select(1+ctx->select_fd, &setl, NULL, NULL, &timeout);
    }
    return 1;
}

/* UDP wait implementation */
static int eth_wait_udp(eth_reader_context_t *ctx)
{
    if (ctx->do_select) {
        fd_set setl;
        struct timeval timeout;

        FD_ZERO(&setl);
        FD_SET(ctx->select_fd, &setl);
        timeout.tv_sec = 0;
        timeout.tv_usec = 250*1000;
        return select(1+ctx->select_fd, &setl, NULL, NULL, &timeout);
    }
    return 1;
}

/* NAT (SLiRP) wait implementation */
static int eth_wait_nat(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    sim_debug(dev->dbit, dev->dptr, "NAT: select_wait, do_select=%d, select_fd=%d\n",
              ctx->do_select, ctx->select_fd);

#ifdef HAVE_SLIRP_NETWORK
    if (ctx->do_select) {
        return sim_slirp_select((sim_slirp_handle *)dev->handle, 250);
    }
#endif
    return 1;
}

/* Test API wait implementation */
static int eth_wait_test(eth_reader_context_t *ctx)
{
    /* Test API doesn't wait, always return immediately */
    (void)ctx;
    return 1;
}

/* None API wait implementation */
static int eth_wait_none(eth_reader_context_t *ctx)
{
    /* No API configured, return immediately */
    (void)ctx;
    return 1;
}

static eth_reader_state_t eth_reader_select_wait_handler(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* Check if device is still open */
    if (!dev->handle)
        return ETH_READER_SHUTDOWN;

    /* Dispatch to API-specific wait handler */
    ctx->sel_ret = wait_dispatch_table[dev->eth_api](ctx);

    /* Handle select errors */
    if (ctx->sel_ret < 0 && errno != EINTR)
        return ETH_READER_ERROR_HANDLER;

    /* Transition based on select result */
    if (ctx->sel_ret > 0)
        return ETH_READER_DISPATCH_READ;
    else
        return ETH_READER_SELECT_WAIT;  /* Timeout or EINTR, continue waiting */
}

static eth_reader_state_t eth_reader_dispatch_read_handler(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* Final check before dispatch */
    if (!dev->handle)
        return ETH_READER_SHUTDOWN;

    /* USE DISPATCH TABLE - eliminates switch statement */
    ctx->status = eth_reader_dispatch_table[dev->eth_api](dev);

    /* Handle dispatch results */
    if (ctx->status > 0)
        return ETH_READER_CHECK_ASYNC;  /* Packets received */
    else if (ctx->status < 0)
        return ETH_READER_ERROR_HANDLER;  /* Error occurred */
    else
        return ETH_READER_SELECT_WAIT;  /* No data, continue waiting */
}

static eth_reader_state_t eth_reader_check_async_handler(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* If async I/O is enabled and queue has data, schedule a poll */
    if (dev->asynch_io && !sim_tailq_empty(&dev->read_queue)) {
        sim_debug(dev->dbit, dev->dptr, "Queueing automatic poll\n");
        sim_activate_abs(dev->dptr->units, dev->asynch_io_latency);
    }

    return ETH_READER_SELECT_WAIT;  /* Back to waiting for more packets */
}

static eth_reader_state_t eth_reader_error_handler(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    ++dev->receive_packet_errors;
    _eth_error(dev, "_eth_reader");

    /* Attempt to recover if device still attached */
    if (dev->handle) {
#if defined(_WIN32)
        ctx->hWait = (dev->eth_api == ETH_API_PCAP) ? pcap_getevent((pcap_t*)dev->handle) : NULL;
#endif
        if (ctx->do_select) {
            ctx->select_fd = dev->fd_handle;
#if !defined(_WIN32) && defined(HAVE_PCAP_NETWORK)
            if (dev->eth_api == ETH_API_PCAP)
                ctx->select_fd = pcap_get_selectable_fd((pcap_t *)dev->handle);
#endif
        }
        return ETH_READER_SELECT_WAIT;  /* Retry */
    }
    else {
        return ETH_READER_SHUTDOWN;  /* Unrecoverable */
    }
}

static eth_reader_state_t eth_reader_shutdown_handler(eth_reader_context_t *ctx)
{
    sim_debug(ctx->dev->dbit, ctx->dev->dptr, "Reader Thread Exiting\n");
    return ETH_READER_SHUTDOWN;  /* Stay in shutdown state */
}

/*============================================================================*/
/*                    STATE HANDLER DISPATCH TABLE                           */
/*============================================================================*/

static const eth_reader_state_handler_t reader_state_handlers[ETH_READER_STATE_COUNT] = {
    [ETH_READER_INIT]           = eth_reader_init_handler,
    [ETH_READER_SELECT_WAIT]    = eth_reader_select_wait_handler,
    [ETH_READER_DISPATCH_READ]  = eth_reader_dispatch_read_handler,
    [ETH_READER_CHECK_ASYNC]    = eth_reader_check_async_handler,
    [ETH_READER_ERROR_HANDLER]  = eth_reader_error_handler,
    [ETH_READER_SHUTDOWN]       = eth_reader_shutdown_handler
};

/*============================================================================*/
/*                    STATE MACHINE EXECUTOR                                 */
/*============================================================================*/

static void eth_reader_run_state_machine(eth_reader_context_t *ctx)
{
    while (ctx->current_state != ETH_READER_SHUTDOWN) {
        eth_reader_state_t next = reader_state_handlers[ctx->current_state](ctx);
        ctx->dev->reader_state = next;  /* Update ETH_DEV state */
        ctx->current_state = next;
    }
    /* Final state update */
    ctx->dev->reader_state = ETH_READER_SHUTDOWN;
}

/*============================================================================*/
/*                    READER THREAD ENTRY POINT                              */
/*============================================================================*/

THREAD_FUNC_DEFN(_eth_reader)
{
    eth_reader_context_t ctx;

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.dev = (ETH_DEV *)arg;
    ctx.current_state = ETH_READER_INIT;

    /* Run state machine until shutdown */
    eth_reader_run_state_machine(&ctx);

    return THREAD_FUNC_RETURN(0);
}

#endif /* USE_READER_THREAD */
