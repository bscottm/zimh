// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet writer thread with state machine control flow */

#if defined(USE_READER_THREAD)

#include "sim_ether.h"
#include "sim_threads.h"

/* Include pcap.h BEFORE sim_ether_internal.h when available */
#ifdef HAVE_PCAP_NETWORK
#include <pcap.h>
#endif

#include "sim_ether_internal.h"
#include "eth_writers.h"
#include "eth_dispatch.h"

/*============================================================================*/
/*                    STATE HANDLER IMPLEMENTATIONS                          */
/*============================================================================*/

static eth_writer_state_t eth_writer_init_handler(eth_writer_context_t *ctx)
{
    ETH_DEV *dev = ctx->dev;

    sim_debug(dev->dbit, dev->dptr, "Writer Thread Starting\n");
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
    while (dev->handle && (dev->write_requests == NULL)) {
        sim_cond_wait(&dev->writer_cond, &dev->writer_lock);
    }

    /* Check if we're shutting down */
    if (!dev->handle) {
        return ETH_WRITER_SHUTDOWN;
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

    /* Shutting down check */
    if (!dev->handle) {
        return ETH_WRITER_CLEANUP;
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
    if (dev->write_requests != NULL && dev->handle) {
        return ETH_WRITER_GET_REQUEST;
    }
    else if (!dev->handle) {
        return ETH_WRITER_SHUTDOWN;
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
/*                    STATE MACHINE EXECUTOR                                 */
/*============================================================================*/

static void eth_writer_run_state_machine(eth_writer_context_t *ctx)
{
    while (ctx->current_state != ETH_WRITER_SHUTDOWN) {
        eth_writer_state_t next = writer_state_handlers[ctx->current_state](ctx);
        ctx->dev->writer_state = next;  /* Update ETH_DEV state */
        ctx->current_state = next;
    }
    /* Final state update */
    ctx->dev->writer_state = ETH_WRITER_SHUTDOWN;
}

/*============================================================================*/
/*                    WRITER THREAD ENTRY POINT                              */
/*============================================================================*/

THREAD_FUNC_DEFN(_eth_writer)
{
    eth_writer_context_t ctx;

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.dev = (ETH_DEV *)arg;
    ctx.current_state = ETH_WRITER_INIT;

    /* Run state machine until shutdown */
    eth_writer_run_state_machine(&ctx);

    return THREAD_FUNC_RETURN(0);
}

#endif /* USE_READER_THREAD */
