// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/*=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * TAP device open function. This is the only place where the TAP device is opened, and it is expected to be
 * called from the _eth_open_port() function in sim_ether.c. The TAP device is opened and configured, and the
 * resulting file descriptor is stored in the eth_backend_t structure for later use by the reader and writer
 * threads.
 *=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#include "sim_defs.h"
#include "sim_sock.h"
#include "simnetwork/unix_tap/unix_tap.h"

// Open a TAP device and configure it for use with the simulator. The device name is specified in the devname
// parameter, and the resulting device name is stored in the savname parameter. The savname parameter must be
// large enough to hold the resulting device name, which is typically the same as the devname parameter.
//
// Returns:
// SCPE_OK: The TAP device was opened and configured successfully.
// SCPE_OPENERR: An error occurred while opening or configuring the TAP device.
// SCPE_MEM: An error occurred while allocating memory for the eth_backend_t structure.
t_stat eth_tap_open(const char *devname, ETH_DEV *dev, char *savname, size_t savname_size)
{
    if (!strcmp(savname, "tap:tapN"))
        return sim_messagef(SCPE_OPENERR, "Eth: Must specify actual tap device name (i.e. tap:tap0)\n");

    /* The resulting TUN/TAP socket.*/
    int tun = -1;

#if defined(__linux) || defined(__linux__)
    int on = 1;

    if ((tun = open("/dev/net/tun", O_RDWR)) >= 0) {
        struct ifreq ifr; /* Interface Requests */

        memset(&ifr, 0, sizeof(ifr));
        /* Set up interface flags */
        strlcpy(ifr.ifr_name, devname, sizeof(ifr.ifr_name));
        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

        /* Send interface requests to TUN/TAP driver. */
        if (ioctl(tun, TUNSETIFF, &ifr) >= 0) {
            if (ioctl(tun, FIONBIO, &on)) {
                close(tun);
                return sim_messagef(SCPE_OPENERR, "Eth: ioctl(FIONBIO) on %s failed: %s\n", devname, strerror(errno));
            } else {
                strlcpy(savname, ifr.ifr_name, savname_size);
            }
        } else {
            close(tun);
            return sim_messagef(SCPE_OPENERR, "Eth: ioctl(TUNSETIFF) on %s failed: %s\n", devname, strerror(errno));
        }
    } else {
        return sim_messagef(SCPE_OPENERR, "Eth: Error opening deevice %s: %s\n", devname, strerror(errno));
    }
#elif defined(HAVE_BSDTUNTAP)
    int on = 1;
    char dev_name[64] = "";

    snprintf(dev_name, sizeof(dev_name) - 1, "/dev/%s", devname);
    dev_name[sizeof(dev_name) - 1] = '\0';

    if ((tun = open(dev_name, O_RDWR)) >= 0) {
        if (ioctl(tun, FIONBIO, &on)) {
            close(tun);
            return sim_messagef(SCPE_OPENERR, "Eth: ioctl(FIONBIO) on %s failed: %s\n", devname, strerror(errno));
        } else {
            memmove(savname, devname, strlen(devname) + 1);
        }

            #    if defined(__APPLE__)
        if (tun >= 0) { /* Good so far? */
            struct ifreq ifr;
            int s;

            /* Now make sure the interface is up */
            memset(&ifr, 0, sizeof(ifr));
            ifr.ifr_addr.sa_family = AF_INET;
            strlcpy(ifr.ifr_name, savname, sizeof(ifr.ifr_name));
            if ((s = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
                if (ioctl(s, SIOCGIFFLAGS, (caddr_t)&ifr) >= 0) {
                    ifr.ifr_flags |= IFF_UP;
                    if (ioctl(s, SIOCSIFFLAGS, (caddr_t)&ifr)) {
                        close(s);
                        return sim_messagef(SCPE_OPENERR, "Eth: ioctl(SIOCSIFFLAGS) on %s failed: %s\n", devname,
                                            strerror(errno));
                    }
                }
                close(s);
            }
        }
#    endif
    } else
        return sim_messagef(SCPE_OPENERR, "Eth: Error opening device %s: %s\n", devname, strerror(errno));
#endif /* !defined(__linux) && !defined(HAVE_BSDTUNTAP) */

    // Catch-all error case: if tun is still -1, we failed to open the device.
    if (tun < 0)
        return sim_messagef(SCPE_OPENERR, "Eth: Error opening device %s: %s\n", devname, strerror(errno));

    eth_backend_t *backend;

    if ((backend = (eth_backend_t *)calloc(1, sizeof(*backend))) == NULL)
        return sim_messagef(SCPE_MEM, "Eth: Error allocating memory for eth_backend_t\n");

    backend->eth_api = ETH_API_TAP;
    backend->packet_wait = eth_wait_tap;
    backend->packet_read = eth_reader_tap;
    backend->before_packet_write = NULL;
    backend->write_packet = eth_writer_tap;
    backend->after_packet_write = NULL;
    backend->reader_shutdown = NULL;
    backend->writer_shutdown = NULL;
    backend->state.eth_socket = tun;

    dev->backend = backend;

    return SCPE_OK;
}