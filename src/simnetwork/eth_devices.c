// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/*
 * Native network device enumeration example
 *
 * This demonstrates how to enumerate network interfaces without requiring
 * PCAP/npcap libraries to be loaded. Each platform uses native OS APIs:
 *
 * Windows: IP Helper API (GetAdaptersAddresses) with local heap allocation
 * Linux/Unix: getifaddrs() with standard heap allocation
 *
 * Benefits:
 * - No dependency on npcap.dll just to list devices
 * - Faster enumeration (no PCAP initialization overhead)
 * - Better integration with OS network stack
 * - Can still use PCAP for actual packet capture when selected
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#if defined(_WIN32) || defined(_WIN64)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <iphlpapi.h>
#    pragma comment(lib, "iphlpapi.lib")
#    pragma comment(lib, "ws2_32.lib")
#else
#    include <sys/types.h>
#    include <ifaddrs.h>
#    include <net/if.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/ioctl.h>
#    include <unistd.h>
#    ifdef __linux__
#        include <linux/if_packet.h>
#        include <net/ethernet.h>
#    elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#        include <net/if_dl.h>
#    endif
#endif

#include "sim_defs.h"
#include "sim_ether.h"
#include "simnetwork/eth_backends.h"

#include "sim_defs.h"
#include "sim_ether.h"
#include "simnetwork/eth_backends.h"

/* Don't pull in all of the scp junk just for a simple utility test. */
#if defined(SHOW_ETH_DEVICES_TARGET)
#    undef fprintf
#    define sim_printf printf
#    undef tolower
#endif

static ETH_DEV **open_eth_devices = NULL;
static size_t n_eth_devices = 0;

#if !defined(_WIN32) && !defined(_WIN64)
static const ETH_DEV_COMMAND eth_turnon_commands[] = {
    {"ip link set dev ", " up 2>/dev/null"}, {"ifconfig ", " up 2>/dev/null"}, {NULL, NULL}};

#    if 0
#    define ETH_MAC_FIXED_PATTERN                                                                                      \
        "[0-9a-fA-F][0-9a-fA-F]:"                                                                                      \
        "[0-9a-fA-F][0-9a-fA-F]:"                                                                                      \
        "[0-9a-fA-F][0-9a-fA-F]:"                                                                                      \
        "[0-9a-fA-F][0-9a-fA-F]:"                                                                                      \
        "[0-9a-fA-F][0-9a-fA-F]:"                                                                                      \
        "[0-9a-fA-F][0-9a-fA-F]"

#    define ETH_MAC_EXTENDED_PATTERN                                                                                   \
        "[0-9a-fA-F]?[0-9a-fA-F]:"                                                                                     \
        "[0-9a-fA-F]?[0-9a-fA-F]:"                                                                                     \
        "[0-9a-fA-F]?[0-9a-fA-F]:"                                                                                     \
        "[0-9a-fA-F]?[0-9a-fA-F]:"                                                                                     \
        "[0-9a-fA-F]?[0-9a-fA-F]:"                                                                                     \
        "[0-9a-fA-F]?[0-9a-fA-F]"

// No longer needed because ZIMH captures the MAC address when constructing the ETH_LIST array.
static const ETH_DEV_COMMAND eth_mac_lookup_commands[] = {
    {"ip link show ", " 2>/dev/null | grep " ETH_MAC_FIXED_PATTERN},
    {"ip link show ", " 2>/dev/null | grep -E " ETH_MAC_EXTENDED_PATTERN},
    {"ifconfig ", " 2>/dev/null | grep " ETH_MAC_FIXED_PATTERN},
    {"ifconfig ", " 2>/dev/null | grep -E " ETH_MAC_EXTENDED_PATTERN},
    {NULL, NULL}};
#endif

// TUN or TAP predicate.
static int is_tuntap(const char *iface);
#endif

/* DEC's organizational unit identifier. Not sure why it was called "framers" in the original SIMH code. */
static const uchar_t digital_equipment_oui[3] = {0xaa, 0x00, 0x03};

/* These need to be externally visible. >sigh!< */
const ETH_MAC eth_mac_any = {0, 0, 0, 0, 0, 0};
const ETH_MAC eth_mac_bcast = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/*============================================================================*/
/*                    Windows Implementation                                  */
/*============================================================================*/

#if defined(_WIN32) || defined(_WIN64)

/*
 * Windows native enumeration using IP Helper API with local heap.
 *
 * Using a local heap provides better isolation and allows us to control
 * allocation behavior independently from the process heap. This is particularly
 * useful for large or variable-size allocations like adapter lists.
 */

static size_t eth_devices_native_windows(int max, ETH_LIST *list, bool include_framers)
{
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    IP_ADAPTER_ADDRESSES *adapter;
    ULONG outBufLen = 0;
    DWORD result;
    HANDLE hHeap = NULL;
    size_t used = 0;

    /* Create a local heap for adapter allocation. This provides:
     * - Better memory isolation
     * - Automatic cleanup via HeapDestroy
     * - No fragmentation of the process heap
     * - Ability to set heap flags independently
     */
    hHeap = HeapCreate(0, 0, 0); /* 0 = growable heap */
    if (hHeap == NULL) {
        fprintf(stderr, "Eth: Failed to create local heap\n");
        return 0;
    }

    /* First call: determine required buffer size */
    result = GetAdaptersAddresses(AF_UNSPEC,                     /* Both IPv4 and IPv6 */
                                  GAA_FLAG_SKIP_ANYCAST |        /* Skip anycast addresses */
                                      GAA_FLAG_SKIP_MULTICAST |  /* Skip multicast addresses */
                                      GAA_FLAG_SKIP_DNS_SERVER | /* Skip DNS server addresses */
                                      GAA_FLAG_INCLUDE_PREFIX,   /* Include prefix information */
                                  NULL,                          /* Reserved */
                                  NULL,                          /* Get required size */
                                  &outBufLen);

    if (result != ERROR_BUFFER_OVERFLOW) {
        /* Unexpected: should return BUFFER_OVERFLOW on first call */
        HeapDestroy(hHeap);
        return 0;
    }

    /* Allocate buffer from local heap */
    adapters = (IP_ADAPTER_ADDRESSES *)HeapAlloc(hHeap, 0, outBufLen);
    if (adapters == NULL) {
        fprintf(stderr, "Eth: Failed to allocate adapter buffer\n");
        HeapDestroy(hHeap);
        return 0;
    }

    /* Second call: get actual adapter information */
    result = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX,
        NULL, adapters, &outBufLen);

    if (result != NO_ERROR) {
        fprintf(stderr, "Eth: GetAdaptersAddresses failed with error %lu\n", result);
        HeapDestroy(hHeap);
        return 0;
    }

    /* Iterate through adapters */
    for (adapter = adapters; adapter != NULL && used < max; adapter = adapter->Next) {
        /* Skip loopback interfaces */
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        /* Skip non-Ethernet interfaces (only Ethernet, IEEE 802.11 wireless) */
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && adapter->IfType != IF_TYPE_IEEE80211)
            continue;

        /* Skip interfaces that are down or not operational */
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        /* Skip interfaces without an Ethernet MAC address */
        if (adapter->PhysicalAddressLength != sizeof(ETH_MAC))
            continue;

        /* Skip NATIVE interfaces that have DEC's MAC prefix. Why?  Good question, but it's part of the
         * original SIMH/open-simh code. Likelihood of encountering a real DEC interface is pretty low,
         * asymptotically rounding exactly to zero. */
        if ((memcmp(adapter->PhysicalAddress, digital_equipment_oui, 3) == 0) != include_framers)
            continue;

        eth_copy_mac(list[used].eth_mac, adapter->PhysicalAddress);

        /* Build device name compatible with PCAP format */
        /* Windows PCAP uses: \Device\NPF_{GUID} */
        snprintf(list[used].name, sizeof(list[used].name), "\\Device\\NPF_%s", adapter->AdapterName);

        /* Use friendly name as description, fall back to description */
        if (adapter->FriendlyName != NULL) {
            /* Convert wide string to multibyte */
            WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, list[used].desc, sizeof(list[used].desc), NULL,
                                NULL);
        } else if (adapter->Description != NULL) {
            WideCharToMultiByte(CP_UTF8, 0, adapter->Description, -1, list[used].desc, sizeof(list[used].desc), NULL,
                                NULL);
        } else {
            snprintf(list[used].desc, sizeof(list[used].desc), "Network adapter %s", adapter->AdapterName);
        }

        list[used].eth_api = ETH_API_PCAP; /* Native, but PCAP-compatible */
        used++;
    }

    /* Destroy the local heap - frees all allocations made from it */
    HeapDestroy(hHeap);

    return used;
}

/*============================================================================*/
/*                    Linux/Unix Implementation                               */
/*============================================================================*/

#else /* Linux/Unix */

/*
 * Linux/Unix native enumeration using getifaddrs().
 *
 * This is the standard POSIX way to enumerate network interfaces.
 * We extract MAC addresses from the AF_PACKET (Linux) or AF_LINK (BSD)
 * address families.
 */

static size_t eth_devices_native_unix(int max, ETH_LIST *list, bool include_framers)
{
    struct ifaddrs *ifaddr, *ifa;
    size_t used = 0;

    if (getifaddrs(&ifaddr) == -1) {
        perror("Eth: getifaddrs");
        return 0;
    }

    /* First pass: collect unique interface names with link-layer addresses */
    for (ifa = ifaddr; ifa != NULL && used < max; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        /* Skip loopback interfaces */
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;

        /* Skip interfaces that are down */
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        /* Check for duplicate interface names already in list */
        bool already_listed = false;
        for (int i = 0; i < used && !already_listed; i++) {
            if (strcmp(list[i].name, ifa->ifa_name) == 0) {
                already_listed = true;
            }
        }
        if (already_listed)
            continue;

#    ifdef __linux__
        /* Linux: Look for AF_PACKET address family to confirm Ethernet */
        if (ifa->ifa_addr->sa_family == AF_PACKET) {
            struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;

            /* Only Ethernet/802.11 (type == ARPHRD_ETHER) */
            if (sll->sll_hatype != ARPHRD_ETHER)
                continue;

            /* Verify we have a valid MAC address */
            if (sll->sll_halen != 6)
                continue;

            if ((memcmp(sll->sll_addr, digital_equipment_oui, 3) != 0) != include_framers)
                continue;

            snprintf(list[used].name, sizeof(list[used].name), "%s", ifa->ifa_name);
            snprintf(list[used].desc, sizeof(list[used].desc), "Ethernet adapter %s", ifa->ifa_name);
            eth_copy_mac(list[used].eth_mac, (ETH_MAC)sll->sll_addr);
            list[used].eth_api = ETH_API_PCAP; /* Native, but PCAP-compatible */
            used++;
        }
#    elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        /* BSD: Look for AF_LINK address family */
        if (ifa->ifa_addr->sa_family == AF_LINK) {
            struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;

            /* Only IFT_ETHER (Ethernet) and IFT_IEEE80211 (wireless) */
            if (sdl->sdl_type != IFT_ETHER && sdl->sdl_type != IFT_IEEE80211)
                continue;

            /* Verify we have a valid MAC address */
            if (sdl->sdl_alen != 6)
                continue;

            /* MAC address follows the name in the sockaddr_dl */
            uint8_t *mac_addr = sll->sdl_data + sll->sdl_nlen;

            if ((memcmp(mac_addr, digital_equipment_oui, 3) != 0) != include_framers)
                continue;

            snprintf(list[used].name, sizeof(list[used].name), "%s", ifa->ifa_name);
            snprintf(list[used].desc, sizeof(list[used].desc), "Ethernet adapter %s", ifa->ifa_name);
            eth_copy_mac(list[used].eth_mac, (ETH_MAC)mac_addr);

            /* Distinguish between pcap-capable vs. TAP interfaces.
             * NOTE: Assuming "not TAP, so it's PCAP" might need to be revisited in the future.
             */
            list[used].eth_api = is_tuntap(ifa->ifa_name) == 1 ? ETH_API_TAP : ETH_API_PCAP;
            used++;
        }
#    endif
    }

    freeifaddrs(ifaddr);
    return used;
}

/* Returns 1 if iface is TUN, 2 if TAP, 0 if neither, -1 on error */
int is_tuntap(const char *iface)
{
#    ifdef __linux__
    /* Linux: TUN/TAP interfaces expose /sys/class/net/<iface>/tun_flags */
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/tun_flags", iface);

    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return 0; /* not a tun/tap interface */

    unsigned int flags = 0;
    if (read(fd, &flags, sizeof(flags)) == -1) {
        close(fd);
        return -1;
    }
    close(fd);

    /* IFF_TUN = 0x0001, IFF_TAP = 0x0002 */
    if (flags & 0x0002)
        return 2; /* TAP */
    if (flags & 0x0001)
        return 1; /* TUN */
    return 0;
#    else
    /* macOS / BSD: check for corresponding /dev/tunN or /dev/tapN node.
     * The interface name encodes the type: tunN → TUN, tapN → TAP.
     * We verify the device node actually exists. */
    char devpath[64];

    if (strncmp(iface, "tap", 3) == 0) {
        snprintf(devpath, sizeof(devpath), "/dev/%s", iface);
        if (access(devpath, F_OK) == 0)
            return 2;
    } else if (strncmp(iface, "tun", 3) == 0) {
        snprintf(devpath, sizeof(devpath), "/dev/%s", iface);
        if (access(devpath, F_OK) == 0)
            return 1;
    }

    /* macOS built-in utun interfaces (IKE/IPsec) are TUN-like */
    if (strncmp(iface, "utun", 4) == 0)
        return 1;

    return 0;
#    endif
}
#endif /* _WIN32 */

/*============================================================================*/
/*                    Public API                                              */
/*============================================================================*/

/*
 * Enumerate network devices using native OS APIs.
 *
 * This is a drop-in replacement for eth_devices() when you want to list
 * available devices without loading PCAP libraries.
 *
 * Returns: number of devices found
 */
size_t eth_devices(size_t max, ETH_LIST *list, bool include_framers)
{
    size_t used;

    if (list == NULL || max <= 0)
        return 0;

    /* Clear the list */
    memset(list, 0, max * sizeof(ETH_LIST));

#if defined(_WIN32) || defined(_WIN64)
    used = eth_devices_native_windows(max, list, false);
#else
    used = eth_devices_native_unix(max, list, false);
#endif

    /* Add the non-libpcap emulations, when present: */

#    if 0 && defined(HAVE_TAP_NETWORK)
    if (used < max) {
#        if defined(__OpenBSD__)
        strlcpy(list[used].name, "tap:tunN", sizeof(list[used].name));
#        else
        strlcpy(list[used].name, "tap:tapN", sizeof(list[used].name));
#        endif
        strlcpy(list[used].desc, "Integrated Tun/Tap support", sizeof(list[used].desc));
        list[used].eth_api = ETH_API_TAP;
        ++used;
    }
#    endif
#    if 0 && defined(HAVE_VDE_NETWORK)
    if (used < max) {
        strlcpy(list[used].name, "vde:device{:switch-port-number}", sizeof(list[used].name));
        strlcpy(list[used].desc, "Integrated VDE support", sizeof(list[used].desc));
        list[used].eth_api = ETH_API_VDE;
        ++used;
    }
#    endif
#    if 0 && defined(HAVE_SLIRP_NETWORK)
    if (used < max) {
        strlcpy(list[used].name, "nat:{optional-nat-parameters}", sizeof(list[used].name));
        strlcpy(list[used].desc, "Integrated NAT (SLiRP) support", sizeof(list[used].desc));
        list[used].eth_api = ETH_API_NAT;
        ++used;
    }
#    endif

#if 0
    if (used < max) {
        strlcpy(list[used].name, "udp:sourceport:remotehost:remoteport", sizeof(list[used].name));
        strlcpy(list[used].desc, "Integrated UDP bridge support", sizeof(list[used].desc));
        list[used].eth_api = ETH_API_UDP;
        ++used;
    }
#endif

#if 0
    if (used < max) {
        strlcpy(list[used].name, "test:name", sizeof(list[used].name));
        strlcpy(list[used].desc, "Integrated test Ethernet backend", sizeof(list[used].desc));
        list[used].eth_api = ETH_API_TEST;
        ++used;
    }
#endif

    return used;
}

const ETH_LIST *eth_getdevice_bydesc(const ETH_LIST *list, size_t n_list, const char *desc)
{
    size_t i;
    const size_t j = strlen(desc);

    for (i = 0; i < n_list; i++) {
        if (j == strlen(list[i].desc) && strncasecmp(list[i].desc, desc, j) == 0)
            return list + i;
    }

    /* not found */
    return NULL;
}

const ETH_LIST *eth_getdevice_byname(const ETH_LIST *list, size_t n_list, const char *name)
{
    size_t i;
    const size_t n = strlen(name);

    for (i = 0; i < n_list; i++) {
        if ((n == strlen(list[i].name)) && (strncasecmp(name, list[i].name, n) == 0)) {
            return list + i;
        }
    }

    /* Not found. */
    return NULL;
}

void eth_add_to_open_list(ETH_DEV *dev)
{
    ETH_DEV **tmp = (ETH_DEV **)realloc(open_eth_devices, (n_eth_devices + 1) * sizeof(*open_eth_devices));
    if (tmp != NULL) {
        open_eth_devices = tmp;
        open_eth_devices[n_eth_devices++] = dev;
    }
}

void eth_remove_from_open_list(ETH_DEV *dev)
{
    int i, j;

    for (i = 0; i < n_eth_devices; ++i)
        if (open_eth_devices[i] == dev) {
            for (j = i + 1; j < n_eth_devices; ++j)
                open_eth_devices[j - 1] = open_eth_devices[j];
            --n_eth_devices;
            break;
        }
}

size_t eth_open_device_count()
{
    return n_eth_devices;
}

ETH_DEV ** const eth_open_devices()
{
    return open_eth_devices;
}

void eth_get_nic_hw_addr(ETH_DEV *dev, const ETH_LIST *eth_info, int set_on)
{
    eth_copy_mac(dev->host_nic_phy_hw_addr, eth_mac_any);
    dev->have_host_nic_phy_addr = false;

    if (dev->backend->eth_api != ETH_API_PCAP && dev->backend->eth_api != ETH_API_TAP)
        return;

    // Easy: Just copy the MAC from the previously gathered ETH_LIST data.
    eth_copy_mac(dev->host_nic_phy_hw_addr, eth_info->eth_mac);
    dev->have_host_nic_phy_addr = true;

#if !defined(_WIN32) && !defined(_WIN64)
    char command[1024];
    FILE *f;
    int i;
    char tool[CBUFSIZE];

    // Ensure the interface is up.
    memset(command, 0, sizeof(command));
    if (set_on) {
        /* try to force an otherwise unused interface to be turned on */
        for (i = 0; eth_turnon_commands[i].prefix; ++i) {
            eth_format_dev_command(command, sizeof(command), &eth_turnon_commands[i], devname);
            get_glyph_nc(command, tool, 0);
            if (sim_get_tool_path(tool)[0]) {
                if (NULL != (f = popen(command, "r")))
                    pclose(f);
            }
        }
    }
}
#endif
}
