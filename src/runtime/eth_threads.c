// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet reader thread with state machine control flow */

#if defined(USE_READER_THREAD)

#include "sim_ether.h"
#include "sim_threads.h"

#include "sim_ether_internal.h"
#include "eth_threads.h"
#include "eth_dispatch.h"

/*============================================================================*/
/*                    STATE HANDLER IMPLEMENTATIONS                          */
/*============================================================================*/

static eth_reader_state_t eth_reader_init_handler(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;
    char reader_name[THREAD_NAME_MAX];

    sim_debug(dev->dbit, dev->dptr, "Reader Thread Starting\n");
    snprintf(reader_name, sizeof(reader_name), "r: %s", dev->name);
    sim_set_thread_name(reader_name);
    sim_os_set_thread_priority(PRIORITY_ABOVE_NORMAL);

    /* Determine select mode and file descriptors based on API type */
    switch (dev->backend.eth_api) {
    case ETH_API_PCAP:
#    if defined(HAVE_PCAP_NETWORK)
#        if !defined(_WIN32)
        ctx->select_fd = pcap_get_selectable_fd(dev->backend.state.pcap);
#        else
        ctx->hWait = pcap_getevent(dev->backend.state.pcap);
#        endif
#    endif
        break;
    case ETH_API_TAP:
    case ETH_API_VDE:
    case ETH_API_UDP:
    case ETH_API_NAT:
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
    return (WAIT_OBJECT_0 == WaitForSingleObject(ctx->hWait, 250) ? 1 : 0);
#else
    /* Unix: Use select if mandated */
    fd_set setl;
    struct timeval timeout;

    FD_ZERO(&setl);
    FD_SET(ctx->select_fd, &setl);
    timeout.tv_sec = 0;
    timeout.tv_usec = 250*1000;
    return select(1+ctx->select_fd, &setl, NULL, NULL, &timeout);
#endif
}

/* TAP wait implementation */
static int eth_wait_tap(eth_reader_context_t *ctx)
{
    fd_set setl;
    struct timeval timeout;

    FD_ZERO(&setl);
    FD_SET(ctx->select_fd, &setl);
    timeout.tv_sec = 0;
    timeout.tv_usec = 250 * 1000;
    return select(1 + ctx->select_fd, &setl, NULL, NULL, &timeout);
}

/* VDE wait implementation */
static int eth_wait_vde(eth_reader_context_t *ctx)
{
    fd_set setl;
    struct timeval timeout;

    FD_ZERO(&setl);
    FD_SET(ctx->select_fd, &setl);
    timeout.tv_sec = 0;
    timeout.tv_usec = 250 * 1000;
    return select(1 + ctx->select_fd, &setl, NULL, NULL, &timeout);
}

/* UDP wait implementation */
static int eth_wait_udp(eth_reader_context_t *ctx)
{
    fd_set setl;
    struct timeval timeout;

    FD_ZERO(&setl);
    FD_SET(ctx->select_fd, &setl);
    timeout.tv_sec = 0;
    timeout.tv_usec = 250 * 1000;
    return select(1 + ctx->select_fd, &setl, NULL, NULL, &timeout);
}

/* NAT (SLiRP) wait implementation */
static int eth_wait_nat(eth_reader_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    sim_debug(dev->dbit, dev->dptr, "NAT: select_wait, select_fd=%" PRIsocket "\n", ctx->select_fd);

#    ifdef HAVE_SLIRP_NETWORK
    return sim_slirp_select((sim_slirp_handle *)dev->backend.state.slirp, 250);
#    else
    return 1;
#    endif
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
    /* Dispatch to API-specific wait handler */
    ctx->sel_ret = wait_dispatch_table[ctx->dev->backend.eth_api](ctx);

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

    /* USE DISPATCH TABLE - eliminates switch statement */
    ctx->status = eth_reader_dispatch_table[dev->backend.eth_api](dev);

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

    if (dev->backend.eth_api == ETH_API_PCAP) {
#if defined(HAVE_PCAP_NETWORK)
        if (dev->backend.state.pcap != NULL) {
#    if defined(_WIN32)
            ctx->hWait = pcap_getevent((pcap_t*)dev->backend.state.pcap);
#    else
            ctx->select_fd = pcap_get_selectable_fd((pcap_t *)dev->backend.state.pcap);
#    endif

            return ETH_READER_SELECT_WAIT;  /* Retry */
        }
        /* Fall through... */
#endif
    } else {
        /* Not PCAP, retry if socket still valid. */
        /* FIXME: VDE, which doesn't use a socket? */
        if (dev->fd_handle != INVALID_SOCKET) {
            ctx->select_fd = dev->fd_handle;
            return ETH_READER_SELECT_WAIT;  /* Retry */
        }

        /* Fall through... */
    }

    return ETH_READER_SHUTDOWN;  /* Unrecoverable */
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
    eth_reader_state_t state;

    while ((state = (eth_reader_state_t) sim_atomic_get(&ctx->dev->reader_state)) != ETH_READER_SHUTDOWN) {
        eth_reader_state_t next = reader_state_handlers[state](ctx);

        /* Check if shutdown was requested externally (from eth_stop_threads) */
        if ((eth_reader_state_t)sim_atomic_get(&ctx->dev->reader_state) != ETH_READER_SHUTDOWN) {
            sim_atomic_put(&ctx->dev->reader_state, (sim_atomic_type_t)next);
        } else {
            /* External shutdown requested, exit loop */
            break;
        }
    }
    /* Final state update */
    sim_atomic_put(&ctx->dev->reader_state, (sim_atomic_type_t)ETH_READER_SHUTDOWN);
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
    /* Start in the state machine's initial state... */
    sim_atomic_put(&ctx.dev->reader_state, ETH_READER_INIT);

    /* Run state machine until shutdown */
    eth_reader_run_state_machine(&ctx);

    return THREAD_FUNC_RETURN(0);
}

/*============================================================================*/
/*                    STATE HANDLER IMPLEMENTATIONS                          */
/*============================================================================*/

static eth_writer_state_t eth_writer_init_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;
    char writer_name[THREAD_NAME_MAX];

    sim_debug(dev->dbit, dev->dptr, "Writer Thread Starting\n");
    snprintf(writer_name, sizeof(writer_name), "w: %s", dev->name);
    sim_set_thread_name(writer_name);
    sim_os_set_thread_priority(PRIORITY_ABOVE_NORMAL);

    /* Acquire writer lock before entering wait loop */
    sim_mutex_lock(&dev->writer_lock);

    /* Signal that writer thread is ready */
    sim_mutex_lock(&dev->startup_lock);
    dev->threads_ready++;
    sim_cond_signal(&dev->startup_cond);
    sim_mutex_unlock(&dev->startup_lock);

    return ETH_WRITER_WAIT_WORK;
}

static eth_writer_state_t eth_writer_wait_work_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* Wait for work while holding the lock */
    while (dev->write_requests == NULL) {
        /* Check for shutdown before waiting */
        if ((eth_writer_state_t)sim_atomic_get(&dev->writer_state) == ETH_WRITER_SHUTDOWN) {
            return ETH_WRITER_SHUTDOWN;
        }
        sim_cond_wait(&dev->writer_cond, &dev->writer_lock);
    }

    /* Work is available */
    return ETH_WRITER_GET_REQUEST;
}

static eth_writer_state_t eth_writer_get_request_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* Check for more work */
    ctx->request = dev->write_requests;

    if (ctx->request == NULL) {
        /* No more requests, go back to waiting */
        return ETH_WRITER_WAIT_WORK;
    }

    /* Pull request off the list */
    dev->write_requests = ctx->request->next;
    sim_mutex_unlock(&dev->writer_lock);

    /* Check if throttling is enabled */
    if (dev->throttle_delay != ETH_THROT_DISABLED_DELAY) {
        return ETH_WRITER_THROTTLE_CHECK;
    }
    else {
        return ETH_WRITER_DISPATCH_WRITE;
    }
}

static eth_writer_state_t eth_writer_throttle_check_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;
    uint32_t packet_delta_time = sim_os_msec() - dev->throttle_packet_time;

    /* Update throttle history */
    dev->throttle_events <<= 1;
    dev->throttle_events += (packet_delta_time < dev->throttle_time) ? 1 : 0;

    /* Check if we need to throttle */
    if ((dev->throttle_events & dev->throttle_mask) == dev->throttle_mask) {
        return ETH_WRITER_THROTTLE_DELAY;
    }
    else {
        /* Update timestamp and proceed to write */
        dev->throttle_packet_time = sim_os_msec();
        return ETH_WRITER_DISPATCH_WRITE;
    }
}

static eth_writer_state_t eth_writer_throttle_delay_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* Sleep to throttle transmission rate */
    sim_os_ms_sleep(dev->throttle_delay);
    ++dev->throttle_count;
    dev->throttle_packet_time = sim_os_msec();

    return ETH_WRITER_DISPATCH_WRITE;
}

static eth_writer_state_t eth_writer_dispatch_write_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* Perform the actual write operation */
    dev->write_status = _eth_write(dev, &ctx->request->packet, NULL);

    return ETH_WRITER_CLEANUP;
}

static eth_writer_state_t eth_writer_cleanup_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* Re-acquire lock to manipulate buffer lists */
    sim_mutex_lock(&dev->writer_lock);

    /* Return buffer to free list */
    ctx->request->next = dev->write_buffers;
    dev->write_buffers = ctx->request;
    ctx->request = NULL;

    /* Check for more work */
    if (dev->write_requests != NULL) {
        return ETH_WRITER_GET_REQUEST;
    }
    else {
        return ETH_WRITER_WAIT_WORK;
    }
}

static eth_writer_state_t eth_writer_shutdown_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    /* If we exited with a request allocated, avoid leaking by putting it on free list */
    if (ctx->request) {
        ctx->request->next = dev->write_buffers;
        dev->write_buffers = ctx->request;
        ctx->request = NULL;
    }

    sim_mutex_unlock(&dev->writer_lock);
    sim_debug(dev->dbit, dev->dptr, "Writer Thread Exiting\n");

    return ETH_WRITER_SHUTDOWN;  /* Stay in shutdown state */
}

/*============================================================================*/
/*                    STATE HANDLER DISPATCH TABLE                           */
/*============================================================================*/

static const eth_writer_state_handler_t writer_state_handlers[ETH_WRITER_STATE_COUNT] = {
    [ETH_WRITER_INIT]           = eth_writer_init_handler,
    [ETH_WRITER_WAIT_WORK]      = eth_writer_wait_work_handler,
    [ETH_WRITER_GET_REQUEST]    = eth_writer_get_request_handler,
    [ETH_WRITER_THROTTLE_CHECK] = eth_writer_throttle_check_handler,
    [ETH_WRITER_THROTTLE_DELAY] = eth_writer_throttle_delay_handler,
    [ETH_WRITER_DISPATCH_WRITE] = eth_writer_dispatch_write_handler,
    [ETH_WRITER_CLEANUP]        = eth_writer_cleanup_handler,
    [ETH_WRITER_SHUTDOWN]       = eth_writer_shutdown_handler
};

/*============================================================================*/
/*                   WRITE STATE MACHINE EXECUTOR                             */
/*============================================================================*/

static void eth_writer_run_state_machine(eth_writer_context_t *ctx)
{
    eth_writer_state_t state;

    while ((state = (eth_writer_state_t) sim_atomic_get(&ctx->dev->writer_state)) != ETH_WRITER_SHUTDOWN) {
        eth_writer_state_t next = writer_state_handlers[state](ctx);

        /* Check if shutdown was requested externally (from eth_stop_threads) */
        if ((eth_writer_state_t) sim_atomic_get(&ctx->dev->writer_state) != ETH_WRITER_SHUTDOWN) {
            sim_atomic_put(&ctx->dev->writer_state, (sim_atomic_type_t)next);
        } else {
            /* External shutdown, exit loop. */
            break;
        }
    }
    /* Final state update */
    sim_atomic_put(&ctx->dev->writer_state, (sim_atomic_type_t)ETH_WRITER_SHUTDOWN);
}

/*============================================================================*/
/*                    WRITER THREAD ENTRY POINT                               */
/*============================================================================*/

THREAD_FUNC_DEFN(_eth_writer)
{
    eth_writer_context_t ctx;

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.dev = (ETH_DEV *)arg;
    sim_atomic_put(&ctx.dev->writer_state, (sim_atomic_type_t) ETH_WRITER_INIT);

    /* Run state machine until shutdown */
    eth_writer_run_state_machine(&ctx);

    return THREAD_FUNC_RETURN(0);
}

#endif /* USE_READER_THREAD */
