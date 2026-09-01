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

/* Match the existing ETH_LIST structure from sim_ether.h */
#define ETH_DEV_NAME_MAX 256
#define ETH_DEV_DESC_MAX 256

typedef enum {
    ETH_API_NONE = 0,
    ETH_API_PCAP,
    ETH_API_TAP,
    ETH_API_VDE,
    ETH_API_UDP,
    ETH_API_NAT,
    ETH_API_TEST,
    ETH_API_COUNT
} eth_api_t;

typedef struct eth_list_s {
    char name[ETH_DEV_NAME_MAX];
    char desc[ETH_DEV_DESC_MAX];
    eth_api_t eth_api;
} ETH_LIST;

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

static int eth_devices_native_windows(int max, ETH_LIST *list)
{
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    IP_ADAPTER_ADDRESSES *adapter;
    ULONG outBufLen = 0;
    DWORD result;
    HANDLE hHeap = NULL;
    int used = 0;

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
    result = GetAdaptersAddresses(
        AF_UNSPEC,                          /* Both IPv4 and IPv6 */
        GAA_FLAG_SKIP_ANYCAST |             /* Skip anycast addresses */
        GAA_FLAG_SKIP_MULTICAST |           /* Skip multicast addresses */
        GAA_FLAG_SKIP_DNS_SERVER |          /* Skip DNS server addresses */
        GAA_FLAG_INCLUDE_PREFIX,            /* Include prefix information */
        NULL,                               /* Reserved */
        NULL,                               /* Get required size */
        &outBufLen
    );

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
        AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX,
        NULL,
        adapters,
        &outBufLen
    );

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
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
            adapter->IfType != IF_TYPE_IEEE80211)
            continue;

        /* Skip interfaces that are down or not operational */
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        /* Skip interfaces without a physical address (MAC) */
        if (adapter->PhysicalAddressLength != 6)
            continue;

        /* Build device name compatible with PCAP format */
        /* Windows PCAP uses: \Device\NPF_{GUID} */
        snprintf(list[used].name, sizeof(list[used].name),
                 "\\Device\\NPF_{%s}", adapter->AdapterName);

        /* Use friendly name as description, fall back to description */
        if (adapter->FriendlyName != NULL) {
            /* Convert wide string to multibyte */
            WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                               list[used].desc, sizeof(list[used].desc),
                               NULL, NULL);
        } else if (adapter->Description != NULL) {
            WideCharToMultiByte(CP_UTF8, 0, adapter->Description, -1,
                               list[used].desc, sizeof(list[used].desc),
                               NULL, NULL);
        } else {
            snprintf(list[used].desc, sizeof(list[used].desc),
                    "Network adapter %s", adapter->AdapterName);
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

static int eth_devices_native_unix(int max, ETH_LIST *list)
{
    struct ifaddrs *ifaddr, *ifa;
    int used = 0;

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
        for (int i = 0; i < used; i++) {
            if (strcmp(list[i].name, ifa->ifa_name) == 0) {
                already_listed = true;
                break;
            }
        }
        if (already_listed)
            continue;

#ifdef __linux__
        /* Linux: Look for AF_PACKET address family to confirm Ethernet */
        if (ifa->ifa_addr->sa_family == AF_PACKET) {
            struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;

            /* Only Ethernet/802.11 (type == ARPHRD_ETHER) */
            if (sll->sll_hatype != ARPHRD_ETHER)
                continue;

            /* Verify we have a valid MAC address */
            if (sll->sll_halen != 6)
                continue;

            snprintf(list[used].name, sizeof(list[used].name), "%s", ifa->ifa_name);
            snprintf(list[used].desc, sizeof(list[used].desc),
                    "Ethernet adapter %s", ifa->ifa_name);
            list[used].eth_api = ETH_API_PCAP; /* Native, but PCAP-compatible */
            used++;
        }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        /* BSD: Look for AF_LINK address family */
        if (ifa->ifa_addr->sa_family == AF_LINK) {
            struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;

            /* Only IFT_ETHER (Ethernet) and IFT_IEEE80211 (wireless) */
            if (sdl->sdl_type != IFT_ETHER && sdl->sdl_type != IFT_IEEE80211)
                continue;

            /* Verify we have a valid MAC address */
            if (sdl->sdl_alen != 6)
                continue;

            snprintf(list[used].name, sizeof(list[used].name), "%s", ifa->ifa_name);
            snprintf(list[used].desc, sizeof(list[used].desc),
                    "Ethernet adapter %s", ifa->ifa_name);
            list[used].eth_api = ETH_API_PCAP; /* Native, but PCAP-compatible */
            used++;
        }
#endif
    }

    freeifaddrs(ifaddr);
    return used;
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
int eth_devices(int max, ETH_LIST *list)
{
    int used;

    if (list == NULL || max <= 0)
        return 0;

    /* Clear the list */
    memset(list, 0, max * sizeof(ETH_LIST));

#if defined(_WIN32) || defined(_WIN64)
    used = eth_devices_native_windows(max, list);
#else
    used = eth_devices_native_unix(max, list);
#endif

    return used;
}

const char *eth_getname(int number, char *name, size_t name_size, char *desc, size_t desc_size)
{
    ETH_LIST list[ETH_MAX_DEVICE];
    int count = eth_devices(ETH_MAX_DEVICE, list, false);

    if ((number < 0) || (count <= number))
        return NULL;
    if (list[number].eth_api != ETH_API_PCAP) {
        sim_printf("Eth: Pcap capable device not found.  You may need to run as root\n");
        return NULL;
    }

    strlcpy(name, list[number].name, name_size);
    strlcpy(desc, list[number].desc, desc_size);
    return name;
}

const char *eth_getname_bydesc(const char *desc, char *name, size_t name_size, char *ndesc, size_t ndesc_size)
{
    ETH_LIST list[ETH_MAX_DEVICE];
    int count = eth_devices(ETH_MAX_DEVICE, list, false);
    int i;
    size_t j = strlen(desc);

    for (i = 0; i < count; i++) {
        int found = 1;
        size_t k = strlen(list[i].desc);

        if (j != k)
            continue;
        for (k = 0; k < j; k++)
            if (tolower(list[i].desc[k]) != tolower(desc[k]))
                found = 0;
        if (found == 0)
            continue;

        /* found a case-insensitive description match */
        strlcpy(name, list[i].name, name_size);
        strlcpy(ndesc, list[i].desc, ndesc_size);
        return name;
    }
    /* not found */
    return NULL;
}

const char *eth_getname_byname(const char *name, char *temp, size_t temp_size, char *desc, size_t desc_size)
{
    ETH_LIST list[ETH_MAX_DEVICE];
    int count = eth_devices(ETH_MAX_DEVICE, list, false);
    size_t n;
    int i, found;

    found = 0;
    n = strlen(name);
    for (i = 0; i < count && !found; i++) {
        if ((n == strlen(list[i].name)) && (strncasecmp(name, list[i].name, n) == 0)) {
            found = 1;
            strlcpy(temp, list[i].name, temp_size); /* only case might be different */
            strlcpy(desc, list[i].desc, desc_size);
        }
    }
    return (found ? temp : NULL);
}

const char *eth_getdesc_byname(char *name, char *temp, size_t temp_size)
{
    ETH_LIST list[ETH_MAX_DEVICE];
    int count = eth_devices(ETH_MAX_DEVICE, list, false);
    size_t n;
    int i, found;

    found = 0;
    n = strlen(name);
    for (i = 0; i < count && !found; i++) {
        if ((n == strlen(list[i].name)) && (strncasecmp(name, list[i].name, n) == 0)) {
            found = 1;
            strlcpy(temp, list[i].desc, temp_size);
        }
    }
    return (found ? temp : NULL);
}

static ETH_DEV **eth_open_devices = NULL;
static int eth_open_device_count = 0;

void _eth_add_to_open_list(ETH_DEV *dev)
{
    ETH_DEV **tmp = (ETH_DEV **)realloc(eth_open_devices, (eth_open_device_count + 1) * sizeof(*eth_open_devices));
    if (tmp != NULL) {
        eth_open_devices = tmp;
        eth_open_devices[eth_open_device_count++] = dev;
    }
}

void _eth_remove_from_open_list(ETH_DEV *dev)
{
    int i, j;

    for (i = 0; i < eth_open_device_count; ++i)
        if (eth_open_devices[i] == dev) {
            for (j = i + 1; j < eth_open_device_count; ++j)
                eth_open_devices[j - 1] = eth_open_devices[j];
            --eth_open_device_count;
            break;
        }
}

#if 0
/*============================================================================*/
/*                    Example Usage / Test Program                            */
/*============================================================================*/

int main(void)
{
    ETH_LIST devices[32];
    int count;

    printf("Native Network Device Enumeration\n");
    printf("===================================\n\n");

#if defined(_WIN32) || defined(_WIN64)
    printf("Using Windows IP Helper API with local heap allocation\n\n");
#elif defined(__linux__)
    printf("Using Linux getifaddrs() with AF_PACKET\n\n");
#elif defined(__APPLE__)
    printf("Using macOS getifaddrs() with AF_LINK\n\n");
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    printf("Using BSD getifaddrs() with AF_LINK\n\n");
#endif

    count = eth_devices_native(32, devices);

    if (count == 0) {
        printf("No network devices found.\n");
        printf("(This may require administrator/root privileges)\n");
        return 1;
    }

    printf("Found %d network device%s:\n\n", count, count == 1 ? "" : "s");

    for (int i = 0; i < count; i++) {
        printf("  eth%-2d  %-50s\n", i, devices[i].name);
        printf("         %s\n", devices[i].desc);
        printf("\n");
    }

    return 0;
}
#endif
