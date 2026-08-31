// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "simnetwork/eth_pcap/eth_pcap.h"

t_stat eth_pcap_open(const char *devname, ETH_DEV *dev, char *savname, size_t savname_size)
{
    pcap_t *pcap;
    const int bufsz = ETH_BUF_SIZE;
    char errbuf[PCAP_ERRBUF_SIZE];

    pcap = pcap_open_live(savname, bufsz, ETH_PROMISC, PCAP_READ_TIMEOUT, errbuf);

#if !defined(_WIN32)
    if (pcap == NULL) { /* can't open device */
        if (strstr(errbuf, "That device is not up")) {
            char command[1024];

            /* try to force an otherwise unused interface to be turned on */
            snprintf(command, sizeof(command),
                     (sim_get_tool_path("ifconfig")[0] != '\0') ? "ifconfig %s up" : "ip link set dev %s up", savname);
            if (system(command)) {
            };
            errbuf[0] = '\0';
            pcap = pcap_open_live(savname, bufsz, ETH_PROMISC, PCAP_READ_TIMEOUT, errbuf);
        }
    }
#endif

if (pcap == NULL) /* can't open device */
        return sim_messagef(SCPE_OPENERR, "Eth: pcap_open_live error - %s\n", errbuf);

#if !defined(HAS_PCAP_SENDPACKET) && defined(xBSD) && !defined(__APPLE__)
    /* Tell the kernel that the header is fully-formed when it gets it.
       This is required in order to fake the src address. */
    int one = 1;
    ioctl(pcap_fileno(backend->state.pcap), BIOCSHDRCMPLT, &one);
#endif /* xBSD */

#if defined(_WIN32)
    if (pcap_setmintocopy(pcap, 0) == -1 || pcap_getevent(pcap) == NULL) {
        pcap_close(pcap);
        return sim_messagef(SCPE_OPENERR, "Eth: Can't set min to copy or get event for interface: %s\n",
                            savname);
    }
#endif

#if !defined(USE_READER_THREAD)
#    ifdef USE_SETNONBLOCK
    /* set ethernet device non-blocking so pcap_dispatch() doesn't hang */
    if (pcap_setnonblock(backend->state.pcap, 1, errbuf) == -1) {
        sim_printf("Eth: Failed to set non-blocking: %s\n", errbuf);
    }
#    endif
#    if defined(__APPLE__)
    {
        /* Deliver packets immediately, needed for OS X 10.6.2 and later
         * (Snow-Leopard).
         * See this thread on libpcap and Mac Os X 10.6 Snow Leopard on
         * the tcpdump mailinglist: http://seclists.org/tcpdump/2010/q1/110
         */
        int v = 1;
        ioctl(pcap_fileno(backend->state.pcap), BIOCIMMEDIATE, &v);
    }
#    endif /* defined (__APPLE__) */
#endif     /* !defined (USE_READER_THREAD) */

#ifdef USE_BPF
    /* dev->bpf_filter may have been initialized previously, since the device was opened earlier and this is
     * an error re-open retry. */
    if (dev->bpf_filter != NULL) {
        struct bpf_program bpf;
        int status;
        bpf_u_int32 bpf_subnet, bpf_netmask;

        if (pcap_lookupnet(savname, &bpf_subnet, &bpf_netmask, errbuf) < 0)
            bpf_netmask = 0;
        /* compile filter string */
        if ((status = pcap_compile(pcap, &bpf, dev->bpf_filter, 1, bpf_netmask)) < 0) {
            strlcpy(errbuf, pcap_geterr(pcap), PCAP_ERRBUF_SIZE);
            /* show erroneous BPF string */
            sim_printf("Eth: BPF string is: |%s|\n", dev->bpf_filter);
            return sim_messagef(SCPE_OPENERR, "Eth: pcap_compile error: %s\n", errbuf);
        } else {
            /* apply compiled filter string */
            if ((status = pcap_setfilter(pcap, &bpf)) < 0) {
                strlcpy(errbuf, pcap_geterr(pcap), PCAP_ERRBUF_SIZE);
                pcap_close(pcap);
                return sim_messagef(SCPE_OPENERR, "Eth: pcap_setfilter error: %s\n", errbuf);
            } else {
#    ifdef USE_SETNONBLOCK
                /* set file non-blocking */
                status = pcap_setnonblock(pcap, 1, errbuf);
#    endif /* USE_SETNONBLOCK */
            }

            pcap_freecode(&bpf);
        }
    }
#endif /* USE_BPF */

    eth_backend_t *backend = (eth_backend_t *) calloc(1, sizeof(eth_backend_t));

    if (backend == NULL) {
        pcap_close(pcap);
        return sim_messagef(SCPE_MEM, "Eth: Failed to allocate memory for backend\n");
    }

    backend->eth_api = ETH_API_PCAP;
    backend->packet_wait = eth_wait_pcap;
    backend->packet_read = eth_reader_pcap;
    backend->before_packet_write = NULL;
    backend->write_packet = eth_writer_pcap;
    backend->after_packet_write = NULL;
    backend->reader_shutdown = NULL;
    backend->writer_shutdown = NULL;
    backend->state.pcap = pcap;

    dev->backend = backend;

    return SCPE_OK;
}
