// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_ETH_BACKENDS_H)
#define SIM_ETH_BACKENDS_H

#include "eth_types.h"

/* On BSD/macOS, include net/bpf.h BEFORE any pcap headers to establish BPF definitions.
 * This prevents redefinition errors on macOS 15.5+ where both net/bpf.h and pcap/bpf.h
 * define bpf_program/bpf_insn. pcap.h is included transitively via sim_ether_internal.h. */
#if (defined (xBSD) || defined (__APPLE__)) && (defined (HAVE_TAP_NETWORK) || defined (HAVE_PCAP_NETWORK))
#include <sys/ioctl.h>
#include <net/bpf.h>
#endif

#if defined(HAVE_PCAP_NETWORK)
#    include <pcap.h>
#    include <string.h>
#else
struct pcap_pkthdr {
    uint32_t caplen; /* length of portion present */
    uint32_t len;    /* length this packet (off wire) */
};
#    define PCAP_ERRBUF_SIZE 256
typedef void *pcap_t; /* Pseudo Type to avoid compiler errors */
#    define DLT_EN10MB 1 /* Dummy Value to avoid compiler errors */
#endif                   /* HAVE_PCAP_NETWORK */

#ifdef HAVE_TAP_NETWORK
#    if defined(__linux) || defined(__linux__)
#        include <sys/ioctl.h>
#        include <net/if.h>
#        include <linux/if_tun.h>
#    elif defined(HAVE_BSDTUNTAP)
#        include <sys/types.h>
#        include <net/if_types.h>
#        include <net/if.h>
#    else /* We don't know how to do this on the current platform */
#        undef HAVE_TAP_NETWORK
#    endif
#endif /* HAVE_TAP_NETWORK */

#ifdef HAVE_VDE_NETWORK
#    include <libvdeplug.h>
#endif /* HAVE_VDE_NETWORK */

#ifdef HAVE_SLIRP_NETWORK
#    include "sim_slirp.h"
#endif

/* Ethernet testing backend. */
typedef struct eth_test_backend {
    char *name;
    ETH_QUE rx_to_guest;
    ETH_QUE tx_from_guest;
    int write_status;
    struct eth_test_backend *next;
} ETH_TEST_BACKEND;

/* Ethernet API type - designates which network backend is in use */
typedef enum eth_api_e {
    ETH_API_NONE = 0, /* No API in use yet */
    ETH_API_PCAP = 1, /* Pcap API in use */
    ETH_API_TAP = 2,  /* tun/tap API in use */
    ETH_API_VDE = 3,  /* VDE API in use */
    ETH_API_UDP = 4,  /* UDP API in use */
    ETH_API_NAT = 5,  /* NAT (SLiRP) API in use */
    ETH_API_TEST = 6, /* test API in use */
    ETH_API_COUNT     /* Number of API types (for array sizing) */
} eth_api_t;

/* Discriminated union for API-specific state. */
typedef struct {
    eth_api_t eth_api; /* API being used to move packets */
    union {
#ifdef HAVE_PCAP_NETWORK
        pcap_t *pcap; /* PCAP handle */
#endif
#if defined(HAVE_VDE_NETWORK)
        VDECONN *vde; /* VDE connection */
#endif
#if defined(HAVE_SLIRP_NETWORK)
        sim_slirp_handle *slirp; /* SLiRP handle */
#endif
    ETH_TEST_BACKEND *test_backend; /* Test backend handle */
    } state;
} eth_backend_t;

#endif
