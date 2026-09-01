// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet reader thread with state machine control flow */

#if !ETH_THREADING_AVAILABLE
#  error "eth_threads.c requires pthread support (define HAVE_PTHREAD)"
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

/*============================================================================*/
/*                    STATE HANDLER IMPLEMENTATIONS                           */
/*============================================================================*/

static eth_reader_status_t eth_reader_init(ETH_DEV *dev)
{
    char reader_name[THREAD_NAME_MAX];

    sim_debug(dev->dbit, dev->dptr, "Reader Thread Starting\n");
    if (dev->dptr->name != NULL)
        snprintf(reader_name, sizeof(reader_name), "%s reader", dev->dptr->name);
    else
        snprintf(reader_name, sizeof(reader_name), "r: %s", dev->name);
    sim_set_thread_name(reader_name);

    /* Set the reader thread's affinity to the I/O affinity set: */
    sim_cpu_set_t io_set;

    sim_os_get_cpu_partition(NULL, &io_set, NULL);
    if (!sim_cpu_set_empty(&io_set))
        sim_os_set_thread_affinity(&io_set);

    /* Signal that reader thread is ready */
    sim_mutex_lock(&dev->startup_lock);
    dev->threads_ready++;
    sim_cond_signal(&dev->startup_cond);
    sim_mutex_unlock(&dev->startup_lock);

    return ETH_READER_RUNNING;
}

/*============================================================================*/
/*                    SELECT/WAIT DISPATCH TABLE                              */
/*============================================================================*/

/* Forward declarations of API-specific wait handlers */

/* NAT (SLiRP) wait implementation */
int eth_wait_nat(eth_backend_t *backend, ETH_DEV *dev, int timeout_ms)
{
    (void)dev;
#ifdef HAVE_SLIRP_NETWORK
    return sim_slirp_select(backend->state.slirp, timeout_ms);
#else
    (void)timeout_ms;
    return 1;
#endif
}

/* None API wait implementation */
int eth_wait_none(eth_backend_t *backend, ETH_DEV *dev)
{
    /* No API configured, return immediately */
    (void)backend;
    (void)dev;
    return 1;
}

static bool eth_reader_error_handler(ETH_DEV *dev)
{
    ++dev->receive_packet_errors;
    _eth_error(dev, "_eth_reader");

    /* Attempt to recover if device still attached */

    if (dev->backend->eth_api == ETH_API_PCAP) {
#if defined(HAVE_PCAP_NETWORK)
        if (dev->backend->state.pcap != NULL) {
            return true; /* Retry */
        }
        /* Fall through... */
#endif
    } else {
        /* Not PCAP, retry if socket still valid. */
        /* FIXME: VDE, which doesn't use a socket? */
        if (dev->backend->state.eth_socket != INVALID_SOCKET) {
            return true;
        }

        /* Fall through... */
    }

    return false; /* Unrecoverable */
}

/*============================================================================*/
/*                    READER THREAD ENTRY POINT                               */
/*============================================================================*/

THREAD_FUNC_DEFN(_eth_reader)
{
    ETH_DEV *dev = (ETH_DEV *)arg;

    /* Starting up... */
    sim_atomic_put(&dev->reader_status, ETH_READER_INIT);

    int start_status = eth_reader_init(dev);

    if (start_status != ETH_READER_RUNNING) {
        goto error_out;
    }

    sim_atomic_put(&dev->reader_status, start_status);
    while ((eth_reader_status_t)sim_atomic_get(&dev->reader_status) == ETH_READER_RUNNING) {
        /* Dispatch to API-specific wait handler */
        eth_backend_t *backend = dev->backend;
        int status = backend->packet_wait(backend, dev, ETH_READER_POLL_TMO);

        /* Packet available? */
        if (status > 0) {
            /* Have backend deliver it. */
            status = backend->packet_read(backend, dev);
        }

        /* If async I/O is enabled and queue has data, schedule a DEVICE/UNIT poll.
         *
         * Note: libslirp is notorious for putting packets on the read queue even if no packets are
         * actually read from an active socket. Hence the "status >= 0" check, which will succeed and the
         * check for a non-empty read queue. */
        if (status >= 0 && dev->asynch_io && !sim_tailq_empty(&dev->read_queue)) {
            sim_debug(dev->dbit, dev->dptr, "Queueing automatic poll\n");
            sim_activate_abs(dev->dptr->units, dev->asynch_io_latency);
        } else if (status < 0 && errno != EINTR && !eth_reader_error_handler(dev)) {
            /* Handle select errors */
            goto error_out;
        }
    }

    sim_atomic_put(&dev->reader_status, (sim_atomic_type_t)ETH_READER_SHUTDOWN);
    return THREAD_FUNC_RETURN(0);

error_out:
    sim_atomic_put(&dev->reader_status, (sim_atomic_type_t)ETH_READER_ERROR);
    return THREAD_FUNC_RETURN(1);
}

/*============================================================================*/
/*                     WRITER THREAD SUPPORT FUNCTIONS                        */
/*============================================================================*/

static int eth_writer_init(ETH_DEV *dev)
{
    char writer_name[THREAD_NAME_MAX];

    sim_debug(dev->dbit, dev->dptr, "Writer Thread Starting\n");
    if (dev->dptr->name != NULL)
        snprintf(writer_name, sizeof(writer_name), "%s writer", dev->dptr->name);
    else
        snprintf(writer_name, sizeof(writer_name), "w: %s", dev->name);
    sim_set_thread_name(writer_name);

    /* Set the writer thread's affinity to the I/O affinity set: */
    sim_cpu_set_t io_set;

    sim_os_get_cpu_partition(NULL, &io_set, NULL);
    if (!sim_cpu_set_empty(&io_set))
        sim_os_set_thread_affinity(&io_set);

    /* Signal that writer thread is ready */
    sim_mutex_lock(&dev->startup_lock);
    dev->threads_ready++;
    sim_cond_signal(&dev->startup_cond);
    sim_mutex_unlock(&dev->startup_lock);

    return ETH_WRITER_RUNNING;
}

/*============================================================================*/
/*                    WRITER THREAD ENTRY POINT                               */
/*============================================================================*/

THREAD_FUNC_DEFN(_eth_writer)
{
    ETH_DEV *dev = (ETH_DEV *)arg;
    eth_backend_t *backend = dev->backend;
    ETH_WRITE_REQUEST *local_freelist = NULL; /* Local accumulator for freed buffers */

    sim_atomic_put(&dev->writer_status, (sim_atomic_type_t)ETH_WRITER_INIT);

    int start_status = eth_writer_init(dev);

    if (start_status != ETH_WRITER_RUNNING) {
        goto error_out;
    }

    sim_atomic_put(&dev->writer_status, start_status);

    while ((eth_writer_state_t)sim_atomic_get(&dev->writer_status) == ETH_WRITER_RUNNING) {
        while (sim_tailq_empty(&dev->write_requests)) {
            /* Check for shutdown before waiting */
            if ((eth_writer_state_t)sim_atomic_get(&dev->writer_status) != ETH_WRITER_RUNNING) {
                goto writer_done;
            }

            sim_mutex_lock(&dev->writer_lock);
            sim_cond_wait(&dev->writer_cond, &dev->writer_lock);
            sim_mutex_unlock(&dev->writer_lock);
        }

        /* Before write housekeeping... */
        if (backend->before_packet_write != NULL && !backend->before_packet_write(backend, dev)) {
            goto error_out;
        }

        /* Dequeue and process outbound packets: */
        ETH_WRITE_REQUEST *outbound = (ETH_WRITE_REQUEST *)sim_tailq_dequeue(&dev->write_requests);

        while (outbound != NULL && (eth_writer_state_t)sim_atomic_get(&dev->writer_status) == ETH_WRITER_RUNNING) {
            /* Check if throttling is enabled */
            if (dev->throttle_delay != ETH_THROT_DISABLED_DELAY) {
                uint32_t packet_delta_time = sim_os_msec() - dev->throttle_packet_time;

                /* Update throttle history */
                dev->throttle_events <<= 1;
                dev->throttle_events += (packet_delta_time < dev->throttle_time) ? 1 : 0;

                /* Check if we need to throttle */
                if ((dev->throttle_events & dev->throttle_mask) == dev->throttle_mask) {
                    /* Sleep to throttle transmission rate */
                    sim_os_ms_sleep(dev->throttle_delay);
                    ++dev->throttle_count;
                    dev->throttle_packet_time = sim_os_msec();
                }
            }

            /* Send the outbound packet */
            dev->write_status = _eth_write(dev, &outbound->packet, NULL);

            /* Add to local freelist (no lock needed) */
            outbound->next = local_freelist;
            local_freelist = outbound;

            /* Check for more work without blocking */
            outbound = (ETH_WRITE_REQUEST *)sim_tailq_dequeue(&dev->write_requests);
        }

        /* After write housekeeping... */
        if (backend->after_packet_write != NULL && !backend->after_packet_write(backend, dev)) {
            goto error_out;
        }

        /* Batch return buffers to global freelist - ONE lock for entire batch */
        if (local_freelist != NULL) {
            /* Find end of local freelist */
            ETH_WRITE_REQUEST *tail = local_freelist;
            while (tail->next != NULL) {
                tail = tail->next;
            }

            /* Prepend entire local freelist to global freelist */
            sim_mutex_lock(&dev->writer_lock);
            tail->next = dev->write_buffers;
            dev->write_buffers = local_freelist;
            sim_mutex_unlock(&dev->writer_lock);

            local_freelist = NULL;
        }
    }

writer_done:
    sim_mutex_lock(&dev->writer_lock);

    /* Return any local freelist buffers */
    if (local_freelist != NULL) {
        ETH_WRITE_REQUEST *tail = local_freelist;
        while (tail->next != NULL) {
            tail = tail->next;
        }

        tail->next = dev->write_buffers;
        dev->write_buffers = local_freelist;
    }

    /* If we exited with requests in queue, avoid leaking by putting them on free list */
    while (!sim_tailq_empty(&dev->write_requests)) {
        ETH_WRITE_REQUEST *outbound = (ETH_WRITE_REQUEST *)sim_tailq_dequeue(&dev->write_requests);
        outbound->next = dev->write_buffers;
        dev->write_buffers = outbound;
    }

    sim_mutex_unlock(&dev->writer_lock);

    sim_atomic_put(&dev->writer_status, (sim_atomic_type_t)ETH_WRITER_SHUTDOWN);
    sim_debug(dev->dbit, dev->dptr, "Writer Thread Exiting\n");

error_out:
    return THREAD_FUNC_RETURN(0);
}

/*============================================================================*/
/*           THREAD MANAGEMENT FUNCTIONS                                      */
/*============================================================================*/

/* Initialize threading structures without starting threads */
t_stat eth_init_threading_structures(ETH_DEV *dev)
{
    t_stat r;

    if (!dev || dev->threading_initialized)
        return SCPE_OK;

    /* Initialize FIFO queues */
    r = eth_tailq_init(&dev->read_queue, 200);
    if (r != SCPE_OK)
        return r;

    r = eth_tailq_init(&dev->write_requests, 200);
    if (r != SCPE_OK) {
        eth_tailq_destroy(&dev->read_queue);
        return r;
    }

    /* Initialize mutexes and condition variables */
    sim_mutex_init(&dev->lock);
    sim_mutex_init(&dev->writer_lock);
    sim_mutex_init(&dev->self_lock);
    sim_mutex_init(&dev->startup_lock);
    sim_cond_init(&dev->writer_cond);
    sim_cond_init(&dev->startup_cond);

    dev->threading_initialized = true;
    dev->threads_running = false;

    return SCPE_OK;
}

/* Stop threads gracefully */
void eth_stop_threads(ETH_DEV *dev)
{
    if (!dev || !dev->threads_running)
        return;

    /* Signal reader thread to shutdown */
    sim_atomic_put(&dev->reader_status, (sim_atomic_type_t)ETH_READER_SHUTDOWN);

    if (dev->backend && dev->backend->reader_shutdown)
        dev->backend->reader_shutdown(dev->backend, dev);

    sim_thread_join(dev->reader_thread, NULL);

    /* Signal writer thread to shutdown */
    sim_mutex_lock(&dev->writer_lock);
    sim_atomic_put(&dev->writer_status, (sim_atomic_type_t)ETH_WRITER_SHUTDOWN);
    sim_cond_signal(&dev->writer_cond);
    sim_mutex_unlock(&dev->writer_lock);

    if (dev->backend && dev->backend->writer_shutdown)
        dev->backend->writer_shutdown(dev->backend, dev);

    sim_thread_join(dev->writer_thread, NULL);

    dev->threads_running = false;
}

/* Clean up threading structures */
void eth_destroy_threading_structures(ETH_DEV *dev)
{
    ETH_WRITE_REQUEST *buffer;

    if (!dev || !dev->threading_initialized)
        return;

    sim_mutex_destroy(&dev->lock);
    sim_mutex_destroy(&dev->self_lock);
    sim_mutex_destroy(&dev->writer_lock);
    sim_mutex_destroy(&dev->startup_lock);
    sim_cond_destroy(&dev->writer_cond);
    sim_cond_destroy(&dev->startup_cond);

    while (NULL != (buffer = dev->write_buffers)) {
        dev->write_buffers = buffer->next;
        free(buffer);
    }

    eth_tailq_destroy(&dev->write_requests);
    eth_tailq_destroy(&dev->read_queue);

    dev->threading_initialized = false;
}

/* Start reader/writer threads */
t_stat eth_start_threads(ETH_DEV *dev)
{
    int create_status;
    const char *thread_name = "reader";

    if (!dev)
        return SCPE_ARG;

    if (dev->threads_running)
        return SCPE_OK;

    if (!dev->threading_initialized)
        return SCPE_IERR;

    dev->threads_ready = 0;

    create_status = sim_thread_create(&dev->reader_thread, _eth_reader, (void *)dev);
    if (create_status == 0) {
        thread_name = "writer";
        create_status = sim_thread_create(&dev->writer_thread, _eth_writer, (void *)dev);
        if (create_status == 0) {
            sim_mutex_lock(&dev->startup_lock);
            while (dev->threads_ready < 2) {
                sim_cond_wait(&dev->startup_cond, &dev->startup_lock);
            }
            sim_mutex_unlock(&dev->startup_lock);

            dev->threads_running = true;
            return SCPE_OK;
        }
    }

    return sim_messagef(SCPE_OPENERR, "Eth: can't start %s thread: %s\n",
                       thread_name, strerror(create_status));
}
