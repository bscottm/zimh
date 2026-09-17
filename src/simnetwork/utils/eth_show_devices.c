// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_ether.h"
#include "simnetwork/eth_network.h"
#include "simnetwork/eth_backends.h"

/* Don't pull in all of the scp junk just for a simple utility test. */
#undef fprintf
#undef tolower

/*============================================================================*/
/*                    Example Usage / Test Program                            */
/*============================================================================*/

int main(void)
{
    ETH_LIST devices[32];
    int count;

    printf("Native Network Device Enumeration\n");
    printf("===================================\n\n");

#    if defined(_WIN32) || defined(_WIN64)
    printf("Using Windows IP Helper API with local heap allocation\n\n");
#    elif defined(__linux__)
    printf("Using Linux getifaddrs() with AF_PACKET\n\n");
#    elif defined(__APPLE__)
    printf("Using macOS getifaddrs() with AF_LINK\n\n");
#    elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    printf("Using BSD getifaddrs() with AF_LINK\n\n");
#    endif

    count = eth_devices(32, devices, false);

    if (count == 0) {
        printf("No network devices found.\n");
        printf("(This may require administrator/root privileges)\n");
        return 1;
    }

    printf("Found %d network device%s:\n\n", count, count == 1 ? "" : "s");

    for (int i = 0; i < count; i++) {
        printf("  eth%-2d  %-50s\n", i, devices[i].desc);
        printf("         Device: %s\n", devices[i].name);

        const uint8_t *m = (const uint8_t *)devices[i].eth_mac;
        printf("         MAC:    %02X:%02X:%02X:%02X:%02X:%02X\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    return 0;
}
