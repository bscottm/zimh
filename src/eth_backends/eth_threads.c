// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet packet reader thread with state machine control flow */

#if !defined(USE_READER_THREAD)
#    error "eth_threads.c MUST BE compile with USE_READER_THREAD defined."
#endif

#include "sim_ether.h"
#include "sim_sock.h"
#include "sim_threads.h"
#include "poll_compat.h"

#include "sim_ether_internal.h"
#include "eth_backends/eth_threads.h"
#include "eth_backends/eth_dispatch.h"

// Default socket read timeout. Note: This can be made longer, which only
// affects how quickly the reader thread exits.
enum {
    ETH_READER_POLL_TMO = 500 /* ms */
};

#define POLL_NORMAL_EVENTS (POLLIN)
#if !defined(_WIN32) && !defined(_WIN64)
#    define POLL_EXTRA_EVENTS (POLLPRI | POLLERR | POLLHUP)
#else
#    define POLL_EXTRA_EVENTS 0 // Windows rejects POLLPRI, POLLERR, POLLHUP.
#endif

/*============================================================================*/
/*                 select()/poll() on a single socket                         */
/*============================================================================*/

static inline int wait_one_socket(SOCKET socket_fd, long timeout_ms)
{
#if SIM_USE_SELECT
    fd_set setl;
    struct timeval timeout;

    FD_ZERO(&setl);
    FD_SET(socket_fd, &setl);
    timeout.tv_sec = 0;
    timeout.tv_usec = timeout_ms * 1000;
    return select(socket_fd + 1, &setl, NULL, NULL, &timeout);
#else
    sim_pollfd_t fds = {.fd = socket_fd, .events = POLL_NORMAL_EVENTS | POLL_EXTRA_EVENTS, .revents = 0};
    return poll(&fds, 1, (sim_polltmo_t)timeout_ms);
#endif
}

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

    sim_os_get_cpu_partition(NULL, &io_set);
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
static int eth_wait_pcap(ETH_DEV *dev);
static int eth_wait_tap(ETH_DEV *dev);
static int eth_wait_vde(ETH_DEV *dev);
static int eth_wait_udp(ETH_DEV *dev);
static int eth_wait_nat(ETH_DEV *dev);
static int eth_wait_test(ETH_DEV *dev);
static int eth_wait_none(ETH_DEV *dev);

/* Dispatch table indexed by eth_api_t */
typedef int (*eth_wait_fn)(ETH_DEV *dev);
static const eth_wait_fn wait_dispatch_table[ETH_API_COUNT] = {
    [ETH_API_NONE] = eth_wait_none, [ETH_API_PCAP] = eth_wait_pcap, [ETH_API_TAP] = eth_wait_tap,
    [ETH_API_VDE] = eth_wait_vde,   [ETH_API_UDP] = eth_wait_udp,   [ETH_API_NAT] = eth_wait_nat,
    [ETH_API_TEST] = eth_wait_test};

/* PCAP wait implementation */
static int eth_wait_pcap(ETH_DEV *dev)
{
#if defined(_WIN32)
    /* Windows: Use event-based waiting */
    return (WAIT_OBJECT_0 == WaitForSingleObject(pcap_getevent(dev->backend.state.pcap), ETH_READER_POLL_TMO) ? 1 : 0);
#else
    return wait_one_socket(pcap_get_selectable_fd(dev->backend.state.pcap), ETH_READER_POLL_TMO);
#endif
}

/* TAP wait implementation */
static int eth_wait_tap(ETH_DEV *dev)
{
#if defined(HAVE_TAP_NETWORK)
    return wait_one_socket(dev->fd_handle, ETH_READER_POLL_TMO);
#else
    return 1;
#endif
}

/* VDE wait implementation */
static int eth_wait_vde(ETH_DEV *dev)
{
#if defined(HAVE_VDE_NETWORK)
    return wait_one_socket(vde_datafd(dev->backend.state.vde), ETH_READER_POLL_TMO);
#else
    return 1;
#endif
}

/* UDP wait implementation */
static int eth_wait_udp(ETH_DEV *dev)
{
    return wait_one_socket(dev->fd_handle, ETH_READER_POLL_TMO);
}

/* NAT (SLiRP) wait implementation */
static int eth_wait_nat(ETH_DEV *dev)
{
#ifdef HAVE_SLIRP_NETWORK
    return sim_slirp_select(dev->backend.state.slirp, ETH_READER_POLL_TMO);
#else
    return 1;
#endif
}

/* Test API wait implementation */
static int eth_wait_test(ETH_DEV *dev)
{
    /* Test API doesn't wait, always return immediately */
    (void)dev;
    return 1;
}

/* None API wait implementation */
static int eth_wait_none(ETH_DEV *dev)
{
    /* No API configured, return immediately */
    (void)dev;
    return 1;
}

static bool eth_reader_error_handler(ETH_DEV *dev)
{
    ++dev->receive_packet_errors;
    _eth_error(dev, "_eth_reader");

    /* Attempt to recover if device still attached */

    if (dev->backend.eth_api == ETH_API_PCAP) {
#if defined(HAVE_PCAP_NETWORK)
        if (dev->backend.state.pcap != NULL) {
            return true; /* Retry */
        }
        /* Fall through... */
#endif
    } else {
        /* Not PCAP, retry if socket still valid. */
        /* FIXME: VDE, which doesn't use a socket? */
        if (dev->fd_handle != INVALID_SOCKET) {
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
        int status = wait_dispatch_table[dev->backend.eth_api](dev);

        /* Packet available? */
        if (status > 0) {
            status = eth_reader_dispatch_table[dev->backend.eth_api](dev);

            if (status > 0) {
                /* If async I/O is enabled and queue has data, schedule a DEVICE/UNIT poll */
                if (dev->asynch_io && !sim_tailq_empty(&dev->read_queue)) {
                    sim_debug(dev->dbit, dev->dptr, "Queueing automatic poll\n");
                    sim_activate_abs(dev->dptr->units, dev->asynch_io_latency);
                }
            } else if (status < 0 && !eth_reader_error_handler(dev)) {
                goto error_out;
            }
        } else {
            if (status < 0 && errno != EINTR && !eth_reader_error_handler(dev)) {
                /* Handle select errors */
                goto error_out;
            }
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

    sim_os_get_cpu_partition(NULL, &io_set);
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
    eth_backend_t *backend = &dev->backend;
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
