// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_ETH_BACKENDS_H)
#    define SIM_ETH_BACKENDS_H

#    include <stdint.h>

#    include "eth_backends/eth_types.h"

/* On BSD/macOS, include net/bpf.h BEFORE any pcap headers to establish BPF definitions.
 * This prevents redefinition errors on macOS 15.5+ where both net/bpf.h and pcap/bpf.h
 * define bpf_program/bpf_insn. pcap.h is included transitively via sim_ether_internal.h. */
#    if (defined(xBSD) || defined(__APPLE__)) && (defined(HAVE_TAP_NETWORK) || defined(HAVE_PCAP_NETWORK))
#        include <sys/ioctl.h>
#        include <net/bpf.h>
#    endif

#    if defined(HAVE_PCAP_NETWORK)
#        include <pcap.h>
#        include <string.h>
#    else
struct pcap_pkthdr {
    uint32_t caplen;  /* length of portion present */
    uint32_t len;     /* length this packet (off wire) */
};
#        define PCAP_ERRBUF_SIZE 256
typedef void *pcap_t; /* Pseudo Type to avoid compiler errors */
#        define DLT_EN10MB 1 /* Dummy Value to avoid compiler errors */
#    endif                   /* HAVE_PCAP_NETWORK */

#    ifdef HAVE_TAP_NETWORK
#        if defined(__linux) || defined(__linux__)
#            include <sys/ioctl.h>
#            include <net/if.h>
#            include <linux/if_tun.h>
#        elif defined(HAVE_BSDTUNTAP)
#            include <sys/types.h>
#            include <net/if_types.h>
#            include <net/if.h>
#        else /* We don't know how to do this on the current platform */
#            undef HAVE_TAP_NETWORK
#        endif
#    endif    /* HAVE_TAP_NETWORK */

#    ifdef HAVE_VDE_NETWORK
#        include <libvdeplug.h>
#    endif /* HAVE_VDE_NETWORK */

#    ifdef HAVE_SLIRP_NETWORK
#        include "eth_backends/slirp/sim_slirp.h"
#    endif

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
typedef struct eth_backend_s {
    /* API being used to move packets */
    eth_api_t eth_api;

    /* API interface: */

    /* Wait for a packet's arrival at the reader. This is the poll/select point.
     * Returns:
     * > 0: One or more packets have arrived.
     *   0: No packet arrival, not an error.
     * < 0: Error waiting for packet arrival.
     */
    int (*packet_wait)(struct eth_backend_s *backend, ETH_DEV *dev);
    /* Read a packet and queue it for simulator device processing.
     * > 0: Packet read successfully, queued for simulator device.
     *   0: No packet read (not an error, safe to retry)
     * < 0: Error reading packet.
     */
    int (*packet_read)(struct eth_backend_s *backend, ETH_DEV *dev);
    
    /* Housekeeping before invoking write_packet(), optional and may be NULL.
     * Returns true if successful, false on error.
     *
     * Note: This is used by the libslirp backend to acquire the mutex that serializes access to libslirp,
     * which is not thread safe.
     */
    bool (*before_packet_write)(struct eth_backend_s *self, ETH_DEV *dev);
    /* Packet writer function: writes one packet for the API.
     * Returns: 0 = success, non-zero = error
     */
    int (*write_packet)(ETH_DEV *dev, const ETH_PACK *packet);
    /* Housekeeping after invoking write_packet(), optional and may be NULL.
     * Returns true if successful, false on error.
     *
     * Note: This is used by the libslirp backend to release the previously acquired mutex that serializes
     * access to libslirp, which is not thread safe.
     */
    bool (*after_packet_write)(struct eth_backend_s *self, ETH_DEV *dev);

    union {
#    ifdef HAVE_PCAP_NETWORK
        pcap_t *pcap;                   /* PCAP handle */
#    endif
#    if defined(HAVE_VDE_NETWORK)
        VDECONN *vde;                   /* VDE connection */
#    endif
#    if defined(HAVE_SLIRP_NETWORK)
        sim_slirp_network *slirp;       /* SLiRP network state */
#    endif
        ETH_TEST_BACKEND *test_backend; /* Test backend handle */
    } state;
} eth_backend_t;

#endif

/*--- API functions for eth_backend_t ---*/

int eth_wait_pcap(eth_backend_t *backend, ETH_DEV *dev);
int eth_wait_tap(eth_backend_t *backend, ETH_DEV *dev);
int eth_wait_vde(eth_backend_t *backend, ETH_DEV *dev);
int eth_wait_nat(eth_backend_t *backend, ETH_DEV *dev);
int eth_wait_udp(eth_backend_t *backend, ETH_DEV *dev);
int eth_wait_test(eth_backend_t *backend, ETH_DEV *dev);

int eth_reader_pcap(eth_backend_t *backend, ETH_DEV *dev);
int eth_reader_tap(eth_backend_t *backend, ETH_DEV *dev);
int eth_reader_vde(eth_backend_t *backend, ETH_DEV *dev);
int eth_reader_nat(eth_backend_t *backend, ETH_DEV *dev);
int eth_reader_udp(eth_backend_t *backend, ETH_DEV *dev);
int eth_reader_none(eth_backend_t *backend, ETH_DEV *dev);
int eth_reader_test(eth_backend_t *backend, ETH_DEV *dev);

/* PCAP writer */
int eth_writer_pcap(ETH_DEV *dev, const ETH_PACK *packet);
/* TAP writer */
int eth_writer_tap(ETH_DEV *dev, const ETH_PACK *packet);
/* VDE writer */
int eth_writer_vde(ETH_DEV *dev, const ETH_PACK *packet);
/* libslirp mutex acquisition */
bool before_slirp_send(eth_backend_t *self, ETH_DEV *dev);
/* libslirp writer */
int eth_writer_nat(ETH_DEV *dev, const ETH_PACK *packet);
/* libslirp mutex release */
bool after_slirp_send(eth_backend_t *self, ETH_DEV *dev);
/* UDP writer */
int eth_writer_udp(ETH_DEV *dev, const ETH_PACK *packet);
/* Empty/no network writer: This does nothing. Reallly. */
int eth_writer_none(ETH_DEV *dev, const ETH_PACK *packet);
/* Test backend writer. */
int eth_writer_test(ETH_DEV *dev, const ETH_PACK *packet);
