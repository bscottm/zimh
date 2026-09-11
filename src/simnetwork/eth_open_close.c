// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_aio.h"
#include "sim_ether.h"
#include "simnetwork/eth_network.h"
#include "simnetwork/eth_backends.h"

// ETH_DEV initializer
static t_stat eth_construct_device(ETH_DEV *dev);
// Open an emulated Ethernet "port"
static t_stat eth_open_port(const char *savname, size_t savname_size, ETH_DEV *eth_dev, DEVICE *dptr, uint32_t dbit);
// Close the emulated Ethernet "port".
static t_stat eth_close_port(eth_backend_t *backend, SOCKET socket_fd);
/* Return true when a name explicitly identifies an integrated pseudo backend. */
static bool eth_is_explicit_pseudo_device(const char *name);

/* Extended entry point for simulators to open an emulated Ethernet connection with control over thread startup.
 *
 * Parameters:
 *   dev          - Ethernet device structure to initialize
 *   name         - Device name to open (e.g., "eth0", "test:label", "tap:tap0")
 *   dptr         - DEVICE pointer for debugging
 *   dbit         - Debug bit mask
 *   start_threads - If true, start reader/writer threads (if AIO available)
 *                   If false, initialize structures but don't start threads
 *
 * Notes:
 *   - Threads can only start if AIO is globally enabled (aio_enabled_and_active())
 *   - If start_threads=true but AIO unavailable, falls back to sync mode
 *   - Device can later call eth_set_async()/eth_clr_async() to toggle mode
 */
t_stat eth_open_ex(ETH_DEV *dev, const char *name, DEVICE *dptr, uint32_t dbit, bool start_threads)
{
    t_stat r;
    int num;
    const char *openname = name;
    const ETH_LIST *the_device = NULL;
    char namebuf[4 * CBUFSIZE];
    ETH_LIST devices[ETH_MAX_DEVICE];
    size_t n_devices;

    /* initialize the ETH_DEV device structure. */
    if ((r = eth_construct_device(dev)) != SCPE_OK)
        return r;

    /* Acquire the device list */
    if ((n_devices = eth_devices(ETH_MAX_DEVICE, devices, false)) == 0) {
        return sim_messagef(SCPE_OPENERR, "Unable to acquire system Ethernet device list");
    }

    /* translate name of type "eth<num>" to real device name */
    if (eth_is_explicit_pseudo_device(name)) {
        openname = name;
    } else if (strlen(name) >= 4 && strncasecmp(name, "eth", 3) == 0) {
        /* "ethNN"? */
        bool all_digits = true;
        const char *p = name + 3;

        while (all_digits && *p != '\0') {
            all_digits = all_digits && isdigit(*p);
            ++p;
        }

        if (all_digits && *p == '\0') {
            num = atoi(&name[3]);

            if (num < 0 || n_devices <= num || devices[num].eth_api != ETH_API_PCAP) {
                return sim_messagef(SCPE_OPENERR, "%s is not a pcap-capable device or you need to run as root to use it.\n", name);
            }

            the_device = devices + num;
            openname = the_device->name;
        }
    } else {
        if ((the_device = eth_getdevice_bydesc(devices, n_devices, name)) == NULL ||
            (the_device = eth_getdevice_byname(devices, n_devices, name)) == NULL) {
            return sim_messagef(SCPE_OPENERR, "%s does not match an Ethernet device name or description.\n", name);
        }

        openname = the_device->name;
    }

    if (strchr(openname, ':')) {
        // Ensure the prefix is lower case.
        strlcpy(namebuf, openname, sizeof(namebuf));
        namebuf[sizeof(namebuf) - 1] = '\0';

        size_t i;

        for (i = 0; namebuf[i] != ':' && namebuf[i] != '\0'; i++)
            namebuf[i] = tolower(namebuf[i]);

        openname = namebuf;
    }

    if ((r = eth_open_port(openname, strlen(openname), dev, dptr, dbit)) != SCPE_OK)
        return r;

    const char *desc = (the_device != NULL && strcmp(the_device->desc, "No description available") != 0) ? the_device->desc : NULL;
    sim_messagef(SCPE_OK, "Eth: opened OS device %s%s%s\n", openname, desc != NULL ? " - " : "", desc != NULL ? desc : "");

    /* get the NIC's hardware MAC address */
    if (the_device != NULL)
        eth_get_nic_hw_addr(dev, the_device, 1);

    /* save name of device */
    dev->name = strdup(openname);

    /* save debugging information */
    dev->dptr = dptr;
    dev->dbit = dbit;

    /* Test backend doesn't generate self-reflections */
    if (dev->backend->eth_api == ETH_API_TEST) {
        dev->reflections = 0;
    } else {
        /* Measure reflections for real backends BEFORE starting async threads.
         * This ensures eth_reflect() runs synchronously using _eth_write/eth_read directly. */
        if ((r = eth_reflect(dev)) != SCPE_OK)
            goto cleanup_after_open;
    }

    /* Always initialize threading structures if platform supports it */
    if ((r = eth_init_threading_structures(dev)) != SCPE_OK)
        goto cleanup_after_open;

    /*
     * Install a total filter on a newly opened interface and let the device
     * simulator install an appropriate filter that reflects the device's
     * configuration. This must happen BEFORE starting async threads to ensure
     * proper BPF filter setup.
     */
    if ((r = eth_filter_hash(dev, 0, NULL, false, false, NULL)) != SCPE_OK)
        goto cleanup_after_open;

    /* Start threads based on start_threads parameter AND global AIO availability */
    if (start_threads && aio_enabled_and_active()) {
        r = eth_start_threads(dev);
        if (r != SCPE_OK) {
            sim_printf("Eth: Warning - failed to start async threads, "
                      "falling back to synchronous mode\n");
            dev->asynch_io = false;
        } else {
            dev->asynch_io = true;
            dev->asynch_io_latency = 1000; /* Default 1ms */
        }
    } else {
        /* AIO isn't available or threads not requested. */
        dev->asynch_io = false;
    }

    eth_add_to_open_list(dev);
    return SCPE_OK;

cleanup_after_open:
    eth_close(dev);
    return r;
}

/* Standard entry point - starts threads based on global AIO preference.
 * This is the most common case: honor the system-wide async I/O setting.
 */
t_stat eth_open(ETH_DEV *dev, const char *name, DEVICE *dptr, uint32_t dbit)
{
    return eth_open_ex(dev, name, dptr, dbit, aio_enabled_and_active());
}

/* Initialize an ETH_DEV device. */
t_stat eth_construct_device(ETH_DEV *dev)
{
    /* set all members to NULL OR 0 */
    memset(dev, 0, sizeof(ETH_DEV));
    dev->reflections = -1; /* not established yet */

    t_stat r;

    if ((r = eth_init_threading_structures(dev)) != SCPE_OK) {
        return sim_messagef(r, "Eth: Failed to initialize threading structures\n");
    }

    return SCPE_OK;
}

//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
// Internal functions:
//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=

t_stat eth_open_port(const char *savname, size_t savname_size, ETH_DEV *eth_dev, DEVICE *dptr, uint32_t dbit)
{
    /* attempt to connect device */
    if (0 == strncmp("test:", savname, 5)) {
        const char *test_label = savname + 5;
        t_stat status;

        while (isspace(*test_label))
            ++test_label;
        if (*test_label == '\0')
            return sim_messagef(SCPE_OPENERR, "Eth: 'test:' device must be given a test label.\n");
        status = eth_test_open(test_label, eth_dev);
        if (status != SCPE_OK)
            return status;
    } else if (0 == strncmp("tap:", savname, 4)) {
#    if defined(HAVE_TAP_NETWORK)
        const char *devname = savname + 4;

        while (isspace(*devname))
            ++devname;

        t_stat status = eth_tap_open(devname, eth_dev, savname, savname_size);

        if (status != SCPE_OK)
            return status;
#    else
        return sim_messagef(SCPE_OPENERR, "Eth: No support for Unix TAP network devices\n");
#    endif
    } else if (0 == strncmp("vde:", savname, 4)) {
#    if defined(HAVE_VDE_NETWORK)
        const char *devname = savname + 4;

        while (isspace(*devname))
            ++devname;

        t_stat status = eth_vde_open(devname, eth_dev, savname, savname_size);

        if (status != SCPE_OK)
            return status;
#    else
        return sim_messagef(SCPE_OPENERR, "Eth: No support for VDE network devices\n");
#    endif /* defined(HAVE_VDE_NETWORK) */
    } else if (0 == strncmp("nat:", savname, 4)) {
#    if defined(HAVE_SLIRP_NETWORK)
        const char *devname = savname + 4;

        while (isspace(*devname))
            ++devname;

        t_stat status;
        if ((status = sim_slirp_open(devname, eth_dev, dptr, dbit)) != SCPE_OK)
            return status;
#    else
        return sim_messagef(SCPE_OPENERR, "Eth: No support for libslirp/SLiRP NAT network devices\n");
#    endif /* defined(HAVE_SLIRP_NETWORK) */
    } else if (0 == strncmp("udp:", savname, 4)) {
        t_stat status = eth_udp_open(savname, eth_dev);

        if (status != SCPE_OK)
            return status;
    } else {
        /* Default: attempt to open the parameter as if it were an explicit device name for pcap. */
#    if defined(HAVE_PCAP_NETWORK)
        t_stat status = eth_pcap_open(savname, eth_dev);
        if (status != SCPE_OK)
            return status;
#    else
        return sim_messagef(SCPE_OPENERR, "Eth (pcap): Unknown or unsupported network device %s\n", savname);
#    endif /* defined(HAVE_PCAP_NETWORK) */
    }

    return SCPE_OK;
}

/* "Pseudo" devices: These are specific Ethernet emulation prefixes, e.g. "tap:" */
bool eth_is_explicit_pseudo_device(const char *name)
{
    static const char *prefixes[] = {"test:", "tap:", "vde:", "nat:", "udp:"};
    const size_t n_prefixes = sizeof(prefixes) / sizeof(prefixes[0]);
    size_t i;

    for (i = 0; i < n_prefixes; ++i) {
        if (strncasecmp(name, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    }

    return false;
}

t_stat eth_close(ETH_DEV *dev)
{
    /* make sure device exists */
    if (dev == NULL)
        return SCPE_UNATT;
    if (dev->backend->eth_api == ETH_API_NONE)
        return SCPE_OK;

    /* close the device */
    dev->have_host_nic_phy_addr = false;

    /* Stop threads if running */
    if (dev->threads_running)
        eth_stop_threads(dev);

    /* Clean up threading structures */
    if (dev->threading_initialized)
        eth_destroy_threading_structures(dev);

    dev->backend->eth_funcs->close(dev->backend);
    sim_messagef(SCPE_OK, "Eth: closed %s\n", dev->name);

    /* Clean up device resources */
    free(dev->name);
    free(dev->bpf_filter);
    memset(dev, 0, sizeof(ETH_DEV));
    eth_remove_from_open_list(dev);

    return SCPE_OK;
}

void eth_error(ETH_DEV *dev, const char *where)
{
    char msg[64];
    const char *netname = "";
    time_t now;
    struct tm tm_now;
    char time_buf[32];

    if ((sim_time(&now) != (time_t)-1) && (localtime_r(&now, &tm_now) != NULL) &&
        (strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S\n", &tm_now) != 0))
        sim_printf("%s", time_buf);
    else
        sim_printf("unknown time\n");
    switch (dev->backend->eth_api) {
    case ETH_API_PCAP:
        netname = "pcap";
        break;
    case ETH_API_TAP:
        netname = "tap";
        break;
    case ETH_API_VDE:
        netname = "vde";
        break;
    case ETH_API_UDP:
        netname = "udp";
        break;
    case ETH_API_NAT:
        netname = "nat";
        break;
    case ETH_API_TEST:
        netname = "test";
        break;
    case ETH_API_NONE:
    case ETH_API_COUNT:
    default:
        netname = "unknown";
        break;
    }
    snprintf(msg, sizeof(msg), "%s(%s): ", where, netname);
    switch (dev->backend->eth_api) {
    case ETH_API_PCAP:
#    if defined(HAVE_PCAP_NETWORK)
        sim_printf("%s%s\n", msg, pcap_geterr(dev->backend->state.pcap));
#    endif
        break;
    default:
        sim_err_sock(INVALID_SOCKET, msg);
        break;
    }

    if (aio_enabled_and_active()) {
        sim_mutex_lock(&dev->lock);
        ++dev->error_waiting_threads;
        /* FIXME: Reevaluate this and the direct case. */
        if (!dev->error_needs_reset)
            dev->error_needs_reset =
                (((dev->transmit_packet_errors + dev->receive_packet_errors) % ETH_ERROR_REOPEN_THRESHOLD) == 0);
        sim_mutex_unlock(&dev->lock);
    } else {
        dev->error_needs_reset =
            (((dev->transmit_packet_errors + dev->receive_packet_errors) % ETH_ERROR_REOPEN_THRESHOLD) == 0);
    }

    /* Limit errors to 1 per second (per invoking thread (reader and writer)) */
    sim_os_sleep(1);

    /*
      When all of the threads which can reference this ETH_DEV object are
      simultaneously waiting in this routine, we have the potential to close
      and reopen the network connection.

      We do this after ETH_ERROR_REOPEN_THRESHOLD total errors have occurred.
      In practice could be as frequently as once every ETH_ERROR_REOPEN_THRESHOLD/2
      seconds, but normally would be about once every 1.5*ETH_ERROR_REOPEN_THRESHOLD
      seconds (ONLY when the error condition exists).
    */

    if (aio_enabled_and_active()) {
        sim_mutex_lock(&dev->lock);
    }

    if ((!aio_enabled_and_active() || dev->error_waiting_threads == 2) && (dev->error_needs_reset)) {
        t_stat r;

        dev->backend->eth_funcs->close(dev->backend);
        sim_os_sleep(ETH_ERROR_REOPEN_PAUSE);

        r = eth_open_port(dev->name, strlen(dev->name) + 1, dev, dev->dptr, dev->dbit);
        dev->error_needs_reset = false;
        if (r == SCPE_OK)
            sim_printf("%s ReOpened: %s \n", msg, dev->name);
        ++dev->error_reopen_count;
    }

    if (aio_enabled_and_active()) {
        --dev->error_waiting_threads;
        sim_mutex_unlock(&dev->lock);
    }
}
