// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_ether.h"
#include "simnetwork/eth_funcs.h"
#include "simnetwork/eth_backends.h"

// ETH_DEV initializer
static void eth_zero(ETH_DEV *dev);

static t_stat eth_open_port(char *savname, size_t savname_size, ETH_DEV *eth_dev, DEVICE *dptr, uint32_t dbit)
{
    (void)savname_size;

    int bufsz = (BUFSIZ < ETH_MAX_PACKET) ? ETH_MAX_PACKET : BUFSIZ;

    if (bufsz < ETH_MAX_JUMBO_FRAME)
        bufsz = ETH_MAX_JUMBO_FRAME; /* Enable handling of jumbo frames */

    /* attempt to connect device */
    if (0 == strncmp("test:", savname, 5)) {
        const char *devname = savname + 5;
        t_stat status;

        while (isspace(*devname))
            ++devname;
        if (*devname == '\0')
            return sim_messagef(SCPE_OPENERR, "Eth: Must specify test backend name\n");
        status = eth_test_open(devname, eth_dev, savname, savname_size);
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
        t_stat status = eth_udp_open(savname, eth_dev, savname, savname_size);

        if (status != SCPE_OK)
            return status;
    } else {
        /* Default: attempt to open the parameter as if it were an explicit device name for pcap. */
#    if defined(HAVE_PCAP_NETWORK)
        t_stat status = eth_pcap_open(savname, eth_dev, savname, savname_size);
        if (status != SCPE_OK)
            return status;
#    else
        return sim_messagef(SCPE_OPENERR, "Eth (pcap): Unknown or unsupported network device %s\n", savname);
#    endif /* defined(HAVE_PCAP_NETWORK) */
    }

    return SCPE_OK;
}

/* Return true when a name explicitly identifies an integrated pseudo backend. */
static bool eth_is_explicit_pseudo_device(const char *name)
{
    static const char *prefixes[] = {"test:", "tap:", "vde:", "nat:", "udp:"};

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (strncasecmp(name, prefixes[i], strlen(prefixes[i])) == 0)
            return true;

    return false;
}

t_stat eth_open(ETH_DEV *dev, const char *name, DEVICE *dptr, uint32_t dbit)
{
    t_stat r;
    int bufsz = (BUFSIZ < ETH_MAX_PACKET) ? ETH_MAX_PACKET : BUFSIZ;
    char errbuf[PCAP_ERRBUF_SIZE];
    char temp[1024], desc[1024] = "";
    const char *savname = name;
    char namebuf[4 * CBUFSIZE];
    int num;

    if (bufsz < ETH_MAX_JUMBO_FRAME)
        bufsz = ETH_MAX_JUMBO_FRAME; /* Enable handling of jumbo frames */

    /* initialize device */
    eth_zero(dev);

    /* translate name of type "eth<num>" to real device name */
    if ((strlen(name) == 4 || strlen(name) == 5) && (tolower(name[0]) == 'e') && (tolower(name[1]) == 't') &&
        (tolower(name[2]) == 'h') && isdigit(name[3]) && (strlen(name) == 4 || isdigit(name[4]))) {
        num = atoi(&name[3]);
        savname = eth_getname(num, temp, sizeof(temp), desc, sizeof(desc));
        if (savname == NULL) /* didn't translate */
            return SCPE_OPENERR;
    } else if (eth_is_explicit_pseudo_device(name)) {
        savname = name;
        desc[0] = '\0';
    } else {
        /* are they trying to use device description? */
        savname = eth_getname_bydesc(name, temp, sizeof(temp), desc, sizeof(desc));
        if (savname == NULL) { /* didn't translate */
            /* probably is not ethX and has no description */
            savname = eth_getname_byname(name, temp, sizeof(temp), desc, sizeof(desc));
            if (savname == NULL) { /* didn't translate */
                savname = name;
                desc[0] = '\0';    /* no description */
            }
        }
    }

    namebuf[sizeof(namebuf) - 1] = '\0';
    strlcpy(namebuf, savname, sizeof(namebuf));
    if (strchr(namebuf, ':')) {
        for (num = 0; (namebuf[num] != ':') && (namebuf[num] != '\0'); num++)
            if (isupper(namebuf[num]))
                namebuf[num] = tolower(namebuf[num]);
    }
    savname = namebuf;
    r = eth_open_port(namebuf, sizeof(namebuf), dev, dptr, dbit);

    if (errbuf[0])
        return sim_messagef(SCPE_OPENERR, "Eth: open error - %s\n", errbuf);
    if (r != SCPE_OK)
        return r;

    if (!strcmp(desc, "No description available"))
        strlcpy(desc, "", sizeof(desc));
    sim_messagef(SCPE_OK, "Eth: opened OS device %s%s%s\n", savname, desc[0] ? " - " : "", desc);

    /* get the NIC's hardware MAC address */
    eth_get_nic_hw_addr(dev, savname, 1);

    /* save name of device */
    dev->name = strdup(savname);

    /* save debugging information */
    dev->dptr = dptr;
    dev->dbit = dbit;
    if (dev->backend->eth_api == ETH_API_TEST)
        dev->reflections = 0;

#    if defined(USE_READER_THREAD)
    if (dev->backend->eth_api != ETH_API_TEST) {
        r = eth_tailq_init(&dev->read_queue, 200); /* initialize FIFO queue */
        if (r != SCPE_OK) {
            sim_printf("eth_open: eth_tailq_init FAILED with status %d\n", r);
            _eth_close_port(&dev->backend, dev->backend->state.eth_socket);
            free(dev->name);
            eth_zero(dev);
            return r;
        }
        sim_mutex_init(&dev->lock);
        sim_mutex_init(&dev->writer_lock);
        sim_mutex_init(&dev->self_lock);
        sim_mutex_init(&dev->startup_lock);
        sim_cond_init(&dev->writer_cond);
        sim_cond_init(&dev->startup_cond);
        r = eth_tailq_init(&dev->write_requests, 200);
        if (r != SCPE_OK) {
            _eth_close_port(&dev->backend, dev->backend->state.eth_socket);
            free(dev->name);
            eth_zero(dev);
            return r;
        }

        /* Start reader and writer threads */
        r = eth_start_threads(dev);
        if (r != SCPE_OK) {
            SOCKET opened_fd = dev->backend->state.eth_socket;

            /* Don't set dev->backend->state.eth_socket to 0 until the threads are stopped. The reader
               thread might start to read from stdin. */
            eth_stop_threads(dev);
            eth_destroy_thread_state(dev);
            _eth_close_port(&dev->backend, opened_fd);

            dev->backend->state.eth_socket = 0;
            free(dev->name);
            eth_zero(dev);
            return r;
        }
    }
#    endif     /* defined (USE_READER_THREAD */
    _eth_add_to_open_list(dev);
    /*
     * install a total filter on a newly opened interface and let the device
     * simulator install an appropriate filter that reflects the device's
     * configuration.
     */
    return eth_filter_hash(dev, 0, NULL, false, false, NULL);
}

void eth_zero(ETH_DEV *dev)
{
    /* set all members to NULL OR 0 */
    memset(dev, 0, sizeof(ETH_DEV));
    dev->reflections = -1; /* not established yet */
}
