// SPDX-FileCopyrightText: 2002-2005 David T. Hittner
// SPDX-License-Identifier: X11

#include "sim_defs.h"
#include "sim_ether.h"
#include "simnetwork/eth_backends.h"

/* Forward decl's: */
static uint16_t ip_checksum(uint16_t *buffer, int size);
static uint16_t pseudo_checksum(uint16_t len, uint16_t proto, void *nsrc_addr, void *ndest_addr, uint8_t *buff);
static int eth_hash_lookup(ETH_MULTIHASH hash, const u_char *data);
static bool eth_process_loopback(ETH_DEV *dev, const u_char *data, uint32_t len);
static void eth_fix_ip_xsum_offload(ETH_DEV *dev, const u_char *msg, int len);
static void eth_fix_ip_jumbo_offload(ETH_DEV *dev, u_char *msg, int len);
static int eth_get_packet_crc32_data(const uint8_t *msg, int len, uint8_t *crcdata);
static int eth_add_packet_crc32(uint8_t *msg, int len);

/* Core packet processing logic - backend agnostic */
void eth_process_received_packet(ETH_DEV *dev, const uint8_t *data, uint32_t len, uint32_t caplen)
{
    bool to_me;
    bool from_me = false;
    bool bpf_used;

    if (LOOPBACK_PHYSICAL_RESPONSE(dev, data)) {
        uint8_t *datacopy = (uint8_t *) malloc(len);

        if (datacopy != NULL) {
            /* Since we changed the outgoing loopback packet to have the physical MAC address of the
               host's interface instead of the programmatically set physical address of this pseudo
               device, we restore parts of the modified packet back as needed */
            memcpy(datacopy, data, len);
            eth_copy_mac(datacopy, dev->physical_addr);
            eth_copy_mac(datacopy + 18, dev->physical_addr);
            eth_process_received_packet(dev, datacopy, len, caplen);
            free(datacopy);
        }

        return;
    }
    switch (dev->backend->eth_api) {
    case ETH_API_PCAP:
#ifdef USE_BPF
        bpf_used = true;
        to_me = true;
        /* AUTODIN II hash mode? */
        if ((dev->hash_filter) && is_eth_groupmac(data) && (!dev->promiscuous) && (!dev->all_multicast))
            to_me = (eth_hash_lookup(dev->hash, data) != 0);
        break;
#endif /* USE_BPF */
    case ETH_API_TAP:
    case ETH_API_VDE:
    case ETH_API_UDP:
    case ETH_API_NAT:
        bpf_used = false;
        eth_packet_trace(dev, data, len, "received");
        eth_packet_filter_status(dev, data, &to_me, &from_me);
        break;
    default:
        bpf_used = to_me = false; /* Should NEVER happen */
        abort();
        break;
    }

    /* detect reception of loopback packet to our physical address */
    if ((LOOPBACK_SELF_FRAME(dev->physical_addr, data)) || (LOOPBACK_PHYSICAL_REFLECTION(dev, data))) {
#ifdef USE_READER_THREAD
        sim_mutex_lock(&dev->self_lock);
#endif
        dev->loopback_self_rcvd_total++;
        /* lower reflection count - if already zero, pass it on */
        if (dev->loopback_self_sent > 0) {
            eth_packet_trace(dev, data, len, "ignored");
            dev->loopback_self_sent--;
            to_me = false;
        } else if (!bpf_used)
            from_me = false;
#ifdef USE_READER_THREAD
        sim_mutex_unlock(&dev->self_lock);
#endif
    }

    if (bpf_used ? to_me : (to_me && !from_me)) {
        if (len > ETH_MIN_JUMBO_FRAME) {
            if (len <= caplen) { /* Whole Frame captured? */
                u_char *datacopy = (u_char *)malloc(len);
                memcpy(datacopy, data, len);
                eth_fix_ip_jumbo_offload(dev, datacopy, len);
                free(datacopy);
            } else
                ++dev->jumbo_truncated;
            return;
        }
        if (!eth_process_loopback(dev, data, len)) {
#if defined(USE_READER_THREAD)
            int crc_len = 0;
            uint8_t crc_data[4] = {0, 0, 0, 0};
            uint32_t pkt_len = len;
            uint8_t *moved_data = NULL;

            if (len < ETH_MIN_PACKET) { /* Pad runt packets before CRC append */
                moved_data = (uint8_t *)malloc(ETH_MIN_PACKET);
                memcpy(moved_data, data, pkt_len);
                memset(moved_data + pkt_len, 0, ETH_MIN_PACKET - pkt_len);
                pkt_len = ETH_MIN_PACKET;
                data = moved_data;
            }

            /* If necessary, fix IP header checksums for packets originated locally */
            /* but were presumed to be traversing a NIC which was going to handle that task */
            /* This must be done before any needed CRC calculation */
            eth_fix_ip_xsum_offload(dev, (const u_char *)data, pkt_len);

            if (dev->need_crc)
                crc_len = eth_get_packet_crc32_data(data, pkt_len, crc_data);

            eth_packet_trace(dev, data, pkt_len, "rcvqd");

            /* Lock-free enqueue - sim_tailq_t is SPSC safe */
            eth_tailq_insert_data(&dev->read_queue, ETH_ITM_NORMAL, data, 0, pkt_len, crc_len, crc_data, 0);
            ++dev->packets_received;
            free(moved_data);
#else /* !USE_READER_THREAD */
            /* set data in passed read packet */
            dev->read_packet->len = len;
            memcpy(dev->read_packet->msg, data, len);
            /* Handle runt case and pad with zeros.  */
            /* The real NIC won't hand us runts from the wire, BUT we may be getting */
            /* some packets looped back before they actually traverse the wire */
            /* (by an internal bridge device for instance) */
            if (len < ETH_MIN_PACKET) {
                memset(&dev->read_packet->msg[len], 0, ETH_MIN_PACKET - len);
                dev->read_packet->len = ETH_MIN_PACKET;
            }
            /* If necessary, fix IP header checksums for packets originated by the local host */
            /* but were presumed to be traversing a NIC which was going to handle that task */
            /* This must be done before any needed CRC calculation */
            eth_fix_ip_xsum_offload(dev, dev->read_packet->msg, dev->read_packet->len);
            if (dev->need_crc)
                dev->read_packet->crc_len = eth_add_packet_crc32(dev->read_packet->msg, dev->read_packet->len);
            else
                dev->read_packet->crc_len = 0;

            eth_packet_trace(dev, dev->read_packet->msg, dev->read_packet->len, "reading");

            ++dev->packets_received;

            /* call optional read callback function */
            if (dev->read_callback)
                (dev->read_callback)(0);
#endif
        }
    }
}

/* Return non-BPF address filter state for a received packet. */
void eth_packet_filter_status(ETH_DEV *dev, const uint8_t *data, bool *to_me, bool *from_me)
{
    int i;

    *to_me = false;
    *from_me = false;
    for (i = 0; i < dev->addr_count; i++) {
        *to_me = *to_me || (memcmp(data, dev->filter_address[i], sizeof(ETH_MAC)) == 0);
        *from_me = *from_me || (memcmp(&data[sizeof(ETH_MAC)], dev->filter_address[i], sizeof(ETH_MAC)) == 0);
    }

    /* all multicast mode and multicast frame? */
    if (dev->all_multicast && is_eth_groupmac(data))
        *to_me = true;

    /* promiscuous mode? */
    if (dev->promiscuous)
        *to_me = true;

    /* AUTODIN II hash mode? */
    if (dev->hash_filter && !*to_me && is_eth_groupmac(data))
        *to_me = eth_hash_lookup(dev->hash, data) != 0;
}

/* Recompute the IP header checksum. */
uint16_t ip_checksum(uint16_t *buffer, int size)
{
    unsigned long cksum = 0;

    /* Sum all the words together, adding the final byte if size is odd  */
    while (size > 1) {
        cksum += *buffer++;
        size -= sizeof(*buffer);
    }
    if (size) {
        uint16_t endword;
        uint8_t *endbytes = (uint8_t *)&endword;

        endbytes[0] = *((uint8_t *)buffer);
        endbytes[1] = 0;
        cksum += endword;
    }

    /* Do a little shuffling  */
    cksum = (cksum >> 16) + (cksum & 0xffff);
    cksum += (cksum >> 16);

    /* Return the bitwise complement of the resulting mishmash  */
    return (uint16_t)(~cksum);
}

/*
 * src_addr and dest_addr are presented in network byte order
 */

uint16_t pseudo_checksum(uint16_t len, uint16_t proto, void *nsrc_addr, void *ndest_addr, uint8_t *buff)
{
    uint32_t sum;
    uint16_t *src_addr = (uint16_t *)nsrc_addr;
    uint16_t *dest_addr = (uint16_t *)ndest_addr;

    /* Sum the data first */
    sum = 0xffff & (~ip_checksum((uint16_t *)buff, len));

    /* add the pseudo header which contains the IP source and
       destination addresses already in network byte order */
    sum += src_addr[0];
    sum += src_addr[1];
    sum += dest_addr[0];
    sum += dest_addr[1];
    /* and the protocol number and the length of the UDP packet */
    sum = sum + htons(proto) + htons(len);

    /* Do a little shuffling  */
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);

    /* Return the bitwise complement of the resulting mishmash  */
    return (uint16_t)(~sum);
}

/* Ethernet multicast address hashing: */
int eth_hash_lookup(ETH_MULTIHASH hash, const u_char *data)
{
    int key = 0x3f & (eth_crc32(0, data, 6) >> 26);

    key ^= 0x3f;
    return (hash[key >> 3] & (1 << (key & 0x7)));
}

/* eth_process_loopback: A bit of a misnomer. This function processes Ethernet
 * Configuration Test Protocol (ECTP) frames.
 *
 * Returns:
 * true: This was an ECTP frame and the frame was forwarded.
 * false: The frame was discarded (wasn't ECTP, skip count invalid, didn't need
 *   to forward the frame.)
 */
bool eth_process_loopback(ETH_DEV *dev, const uint8_t *data, uint32_t len)
{
    uint32_t protocol = data[13] | (data[12] << 8);
    ETH_PACK response;
    uint32_t offset, function;

    /* !ethernet loopback test (ECTP frame.) */
    if (protocol != 0x9000)
        return false;

    if (LOOPBACK_REFLECTION_TEST_PACKET(dev, data))
        return false; /* Ignore reflection check packet */

    /* NOTE: According to the ECTP spec in the Ethernet 2 standard, data in the
     * ECTP message(s) are little endian. */
    offset = 16 + (data[14] | (data[15] << 8));
    if (offset >= len)
        return false;
    function = data[offset] | (data[offset + 1] << 8);

    if (function != 2) /*forward*/
        return false;

    /* The only packets we should be responding to are ones which
       we received due to them being directed to our physical MAC address,
       OR the Broadcast address OR to a Multicast address we're listening to
       (we may receive others if we're in promiscuous mode, but shouldn't
       respond to them) */
    if (!is_broadcast_mac(data) && !is_eth_groupmac(data) && !eth_mac_equal(dev->filter_address[0], data))
        return false;

    /* Attempts to forward to multicast or broadcast addresses are explicitly
       ignored by consuming the packet and doing nothing else */
    if (is_broadcast_mac(&data[offset + 2]) || is_eth_groupmac(&data[offset + 2]))
        return true;

    eth_packet_trace(dev, data, len, "rcvd");

    sim_debug(dev->dbit, dev->dptr, "eth_process_loopback()\n");

    /* create forward response packet */
    memset(&response, 0, sizeof(response));
    response.len = len;
    memcpy(response.msg, data, len);
    eth_copy_mac(&response.msg[0], &response.msg[offset + 2]);
    eth_copy_mac(&response.msg[6], dev->filter_address[0]);
    offset += 8 - 16; /* Account for the Ethernet Header and Offset value in this number  */
    response.msg[14] = offset & 0xFF;
    response.msg[15] = (offset >> 8) & 0xFF;

    /* send response packet */
    eth_write(dev, &response, NULL);

    eth_packet_trace(dev, response.msg, response.len, "loopbackforward");

    ++dev->loopback_packets_processed;

    return true;
}

/* The IP header */
struct IPHeader {
    uint8_t verhlen;                                  /* Version & Header Length in dwords */
#define IP_HLEN(IP) (((IP)->verhlen & 0xF) << 2)      /* Header Length in Bytes */
#define IP_VERSION(IP) ((((IP)->verhlen) >> 4) & 0xF) /* IP Version */
    uint8_t tos;                                      /* Type of service */
    uint16_t total_len;                               /* Length of the packet in dwords */
    uint16_t ident;                                   /* unique identifier */
    uint16_t flags;                                   /* Fragmentation Flags */
#define IP_DF_FLAG (0x4000)
#define IP_MF_FLAG (0x2000)
#define IP_OFFSET_MASK (0x1FFF)
#define IP_FRAG_DF(IP) (ntohs(((IP)->flags)) & IP_DF_FLAG)
#define IP_FRAG_MF(IP) (ntohs(((IP)->flags)) & IP_MF_FLAG)
#define IP_FRAG_OFFSET(IP) (ntohs(((IP)->flags)) & IP_OFFSET_MASK)
    uint8_t ttl;        /* Time to live */
    uint8_t proto;      /* Protocol number (TCP, UDP etc) */
    uint16_t checksum;  /* IP checksum */
    uint32_t source_ip; /* Source Address */
    uint32_t dest_ip;   /* Destination Address */
};

/* ICMP header */
struct ICMPHeader {
    uint8_t type;           /* ICMP packet type */
    uint8_t code;           /* Type sub code */
    uint16_t checksum;      /* ICMP Checksum */
    uint32_t otherstuff[1]; /* optional data */
};

struct UDPHeader {
    uint16_t source_port;
    uint16_t dest_port;
    uint16_t length; /* The length of the entire UDP datagram, including both header and Data fields. */
    uint16_t checksum;
};

struct TCPHeader {
    uint16_t source_port;
    uint16_t dest_port;
    uint32_t sequence_number;
    uint32_t acknowledgement_number;
    uint16_t data_offset_and_flags;
#define TCP_DATA_OFFSET(TCP) ((ntohs((TCP)->data_offset_and_flags) >> 12) << 2)
#define TCP_CWR_FLAG (0x80)
#define TCP_ECR_FLAG (0x40)
#define TCP_URG_FLAG (0x20)
#define TCP_ACK_FLAG (0x10)
#define TCP_PSH_FLAG (0x08)
#define TCP_RST_FLAG (0x04)
#define TCP_SYN_FLAG (0x02)
#define TCP_FIN_FLAG (0x01)
#define TCP_FLAGS_MASK (0xFFF)
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
    uint16_t otherstuff[1]; /* The rest of the packet */
};

#ifndef IPPROTO_TCP
#    define IPPROTO_TCP 6  /* tcp */
#endif
#ifndef IPPROTO_UDP
#    define IPPROTO_UDP 17 /* user datagram protocol */
#endif
#ifndef IPPROTO_ICMP
#    define IPPROTO_ICMP 1 /* control message protocol */
#endif

void eth_fix_ip_xsum_offload(ETH_DEV *dev, const u_char *msg, int len)
{
    const unsigned short *proto = (const unsigned short *)&msg[12];

    /* Destined for this host and an IP frame? */
    if (dev->have_host_nic_phy_addr && memcmp(msg + 6, dev->host_nic_phy_hw_addr, 6) == 0 && ntohs(*proto) == 0x0800) {
        struct IPHeader *IP = (struct IPHeader *)&msg[14];
        uint16_t orig_checksum;

        if (IP_VERSION(IP) != 4)
            return; /* Only interested in IPv4 frames */
        if ((IP_HLEN(IP) > len) || (ntohs(IP->total_len) > len))
            return; /* Bogus header length */

        orig_checksum = IP->checksum;
        IP->checksum = 0;
        IP->checksum = ip_checksum((uint16_t *)IP, IP_HLEN(IP));
        if (orig_checksum != IP->checksum)
            eth_packet_trace(dev, msg, len, "reading IP header Checksum Fixed");
        if (IP_FRAG_OFFSET(IP) || IP_FRAG_MF(IP))
            return; /* Insufficient data to compute payload checksum */

        switch (IP->proto) {
        case IPPROTO_UDP:
            struct UDPHeader *UDP = (struct UDPHeader *)(((char *)IP) + IP_HLEN(IP));
            if (ntohs(UDP->length) > (len - IP_HLEN(IP)))
                return; /* packet contained length exceeds packet size */
            if (UDP->checksum == 0)
                return; /* UDP Checksums are disabled */
            orig_checksum = UDP->checksum;
            UDP->checksum = 0;
            UDP->checksum =
                pseudo_checksum(ntohs(UDP->length), IPPROTO_UDP, &IP->source_ip, &IP->dest_ip, (uint8_t *)UDP);
            if (orig_checksum != UDP->checksum)
                eth_packet_trace(dev, msg, len, "reading UDP header Checksum Fixed");
            break;
        case IPPROTO_TCP:
            struct TCPHeader *TCP = (struct TCPHeader *)(((char *)IP) + IP_HLEN(IP));
            orig_checksum = TCP->checksum;
            TCP->checksum = 0;
            TCP->checksum = pseudo_checksum(ntohs(IP->total_len) - IP_HLEN(IP), IPPROTO_TCP, &IP->source_ip,
                                            &IP->dest_ip, (uint8_t *)TCP);
            if (orig_checksum != TCP->checksum)
                eth_packet_trace(dev, msg, len, "reading TCP header Checksum Fixed");
            break;
        case IPPROTO_ICMP:
            struct ICMPHeader *ICMP = (struct ICMPHeader *)(((char *)IP) + IP_HLEN(IP));
            orig_checksum = ICMP->checksum;
            ICMP->checksum = 0;
            ICMP->checksum = ip_checksum((uint16_t *)ICMP, ntohs(IP->total_len) - IP_HLEN(IP));
            if (orig_checksum != ICMP->checksum)
                eth_packet_trace(dev, msg, len, "reading ICMP header Checksum Fixed");
            break;
        }
    }
}

void eth_fix_ip_jumbo_offload(ETH_DEV *dev, u_char *msg, int len)
{
    const unsigned short *proto = (const unsigned short *)&msg[12];
    struct IPHeader *IP;
    struct TCPHeader *TCP = NULL;
    struct UDPHeader *UDP;
    struct ICMPHeader *ICMP;
    uint16_t orig_checksum;
    uint16_t payload_len;
    uint16_t mtu_payload;
    uint16_t ip_flags;
    uint16_t frag_offset;
    uint16_t orig_tcp_flags;

    /* Only interested in IP frames */
    if (ntohs(*proto) != 0x0800) {
        ++dev->jumbo_dropped; /* Non IP Frames are dropped */
        return;
    }
    IP = (struct IPHeader *)&msg[14];
    if (IP_VERSION(IP) != 4) {
        ++dev->jumbo_dropped; /* Non IPv4 jumbo frames are dropped */
        return;
    }
    if ((IP_HLEN(IP) > len) || (ntohs(IP->total_len) > len)) {
        ++dev->jumbo_dropped; /* Bogus header length frames are dropped */
        return;
    }
    if (IP_FRAG_OFFSET(IP) || IP_FRAG_MF(IP)) {
        ++dev->jumbo_dropped; /* Previously fragmented, but currently jumbo sized frames are dropped */
        return;
    }
    switch (IP->proto) {
    case IPPROTO_UDP:
        UDP = (struct UDPHeader *)(((char *)IP) + IP_HLEN(IP));
        if (ntohs(UDP->length) > (len - IP_HLEN(IP))) {
            ++dev->jumbo_dropped; /* Bogus UDP packet length (packet contained length exceeds packet size) frames are
                                     dropped */
            return;
        }
        if (UDP->checksum == 0)
            break; /* UDP Checksums are disabled */
        orig_checksum = UDP->checksum;
        UDP->checksum = 0;
        UDP->checksum = pseudo_checksum(ntohs(UDP->length), IPPROTO_UDP, &IP->source_ip, &IP->dest_ip, (uint8_t *)UDP);
        if (orig_checksum != UDP->checksum)
            eth_packet_trace(dev, msg, len, "reading jumbo UDP header Checksum Fixed");
        break;
    case IPPROTO_ICMP:
        ICMP = (struct ICMPHeader *)(((char *)IP) + IP_HLEN(IP));
        orig_checksum = ICMP->checksum;
        ICMP->checksum = 0;
        ICMP->checksum = ip_checksum((uint16_t *)ICMP, ntohs(IP->total_len) - IP_HLEN(IP));
        if (orig_checksum != ICMP->checksum)
            eth_packet_trace(dev, msg, len, "reading jumbo ICMP header Checksum Fixed");
        break;
    case IPPROTO_TCP:
        TCP = (struct TCPHeader *)(((char *)IP) + IP_HLEN(IP));
        if ((TCP_DATA_OFFSET(TCP) > (len - IP_HLEN(IP))) || (TCP_DATA_OFFSET(TCP) < 20)) {
            ++dev->jumbo_dropped; /* Bogus TCP packet header length (packet contained length exceeds packet size) frames
                                     are dropped */
            return;
        }
        /* We don't do anything with the TCP checksum since we're going to resegment the TCP data below */
        break;
    default:
        ++dev->jumbo_dropped; /* We only handle UDP, ICMP and TCP jumbo frames others are dropped */
        return;
    }
    /* Reasonable Checksums are now in the jumbo packet, but we've got to actually */
    /* deliver ONLY standard sized ethernet frames.  Our job here is to now act as */
    /* a router might have to and fragment these IPv4 frames as they are delivered */
    /* into the virtual NIC. We do this by walking down the packet and dispatching */
    /* a chunk at a time recomputing an appropriate header for each chunk. For */
    /* datagram oriented protocols (UDP and ICMP) this is done by simple packet */
    /* fragmentation.  For TCP this is done by breaking large packets into separate */
    /* TCP packets. */
    switch (IP->proto) {
    case IPPROTO_UDP:
    case IPPROTO_ICMP:
        ++dev->jumbo_fragmented;
        /* When we're performing LSO (Large Send Offload), we're given a
           'template' header which may not include a value being populated
           in the IP header length (which is only 16 bits).
           We process as payload everything which isn't known header data. */
        payload_len = (uint16_t)(len - (14 + IP_HLEN(IP)));
        mtu_payload = ETH_MIN_JUMBO_FRAME - (14 + IP_HLEN(IP));
        frag_offset = 0;
        while (payload_len > 0) {
            ip_flags = frag_offset;
            if (payload_len > mtu_payload) {
                ip_flags |= IP_MF_FLAG;
                IP->total_len = htons(((mtu_payload >> 3) << 3) + IP_HLEN(IP));
            } else {
                IP->total_len = htons(payload_len + IP_HLEN(IP));
            }
            IP->flags = htons(ip_flags);
            IP->checksum = 0;
            IP->checksum = ip_checksum((uint16_t *)IP, IP_HLEN(IP));
            eth_packet_trace(dev, ((u_char *)IP) - 14, 14 + ntohs(IP->total_len), "reading Datagram fragment");
#if ETH_MIN_JUMBO_FRAME < ETH_MAX_PACKET
            {
                /* Debugging is easier if we read packets directly with pcap
                   (i.e. we can use Wireshark to verify packet contents)
                   we don't want to do this all the time for 2 reasons:
                     1) sending through pcap involves kernel transitions and
                     2) if the current system reflects sent packets, the
                        receiving side will receive and process 2 copies of
                        any packets sent this way. */
                ETH_PACK pkt;

                memset(&pkt, 0, sizeof(pkt));
                memcpy(pkt.msg, ((u_char *)IP) - 14, 14 + ntohs(IP->total_len));
                pkt.len = 14 + ntohs(IP->total_len);
                _eth_write(dev, &pkt, NULL);
            }
#else
            eth_process_received_packet(dev, ((uint8_t *)IP) - 14, 14 + ntohs(IP->total_len), 14 + ntohs(IP->total_len));
#endif
            payload_len -= (ntohs(IP->total_len) - IP_HLEN(IP));
            frag_offset += (ntohs(IP->total_len) - IP_HLEN(IP)) >> 3;
            if (payload_len > 0) {
                /* Move the MAC and IP headers down to just prior to the next payload segment */
                memcpy(((u_char *)IP) + ntohs(IP->total_len) - (14 + IP_HLEN(IP)), ((u_char *)IP) - 14,
                       14 + IP_HLEN(IP));
                IP = (struct IPHeader *)(((u_char *)IP) + ntohs(IP->total_len) - IP_HLEN(IP));
            }
        }
        break;
    case IPPROTO_TCP:
        ++dev->jumbo_fragmented;
        eth_packet_trace_ex(dev, ((u_char *)IP) - 14, len, "Fragmenting Jumbo TCP segment", 1, dev->dbit);
        TCP = (struct TCPHeader *)(((char *)IP) + IP_HLEN(IP));
        orig_tcp_flags = ntohs(TCP->data_offset_and_flags);
        /* When we're performing LSO (Large Send Offload), we're given a
           'template' header which may not include a value being populated
           in the IP header length (which is only 16 bits).
           We process as payload everything which isn't known header data. */
        payload_len = (uint16_t)(len - (14 + IP_HLEN(IP) + TCP_DATA_OFFSET(TCP)));
        mtu_payload = ETH_MIN_JUMBO_FRAME - (14 + IP_HLEN(IP) + TCP_DATA_OFFSET(TCP));
        while (payload_len > 0) {
            if (payload_len > mtu_payload) {
                TCP->data_offset_and_flags = htons(orig_tcp_flags & ~(TCP_PSH_FLAG | TCP_FIN_FLAG | TCP_RST_FLAG));
                IP->total_len = htons(mtu_payload + IP_HLEN(IP) + TCP_DATA_OFFSET(TCP));
            } else {
                TCP->data_offset_and_flags = htons(orig_tcp_flags);
                IP->total_len = htons(payload_len + IP_HLEN(IP) + TCP_DATA_OFFSET(TCP));
            }
            IP->checksum = 0;
            IP->checksum = ip_checksum((uint16_t *)IP, IP_HLEN(IP));
            TCP->checksum = 0;
            TCP->checksum = pseudo_checksum(ntohs(IP->total_len) - IP_HLEN(IP), IPPROTO_TCP, &IP->source_ip,
                                            &IP->dest_ip, (uint8_t *)TCP);
            eth_packet_trace_ex(dev, ((u_char *)IP) - 14, 14 + ntohs(IP->total_len), "reading TCP segment", 1, dev->dbit);
#if ETH_MIN_JUMBO_FRAME < ETH_MAX_PACKET
            {
                /* Debugging is easier if we read packets directly with pcap
                   (i.e. we can use Wireshark to verify packet contents)
                   we don't want to do this all the time for 2 reasons:
                     1) sending through pcap involves kernel transitions and
                     2) if the current system reflects sent packets, the
                        receiving side will receive and process 2 copies of
                        any packets sent this way. */
                ETH_PACK pkt;

                memset(&pkt, 0, sizeof(pkt));
                memcpy(pkt.msg, ((u_char *)IP) - 14, 14 + ntohs(IP->total_len));
                pkt.len = 14 + ntohs(IP->total_len);
                _eth_write(dev, &pkt, NULL);
            }
#else
            eth_process_received_packet(dev, ((uint8_t *)IP) - 14, 14 + ntohs(IP->total_len), 14 + ntohs(IP->total_len));
#endif
            payload_len -= (ntohs(IP->total_len) - (IP_HLEN(IP) + TCP_DATA_OFFSET(TCP)));
            if (payload_len > 0) {
                /* Move the MAC, IP and TCP headers down to just prior to the next payload segment */
                memcpy(((u_char *)IP) + ntohs(IP->total_len) - (14 + IP_HLEN(IP) + TCP_DATA_OFFSET(TCP)),
                       ((u_char *)IP) - 14, 14 + IP_HLEN(IP) + TCP_DATA_OFFSET(TCP));
                IP = (struct IPHeader *)(((u_char *)IP) + ntohs(IP->total_len) - (IP_HLEN(IP) + TCP_DATA_OFFSET(TCP)));
                TCP = (struct TCPHeader *)(((char *)IP) + IP_HLEN(IP));
                TCP->sequence_number = htonl(mtu_payload + ntohl(TCP->sequence_number));
            }
        }
        break;
    }
}

int eth_get_packet_crc32_data(const uint8_t *msg, int len, uint8_t *crcdata)
{
    int crc_len;

    if (len <= ETH_MAX_PACKET) {
        uint32_t crc = eth_crc32(0, msg, len); /* calculate CRC */
        uint32_t ncrc = htonl(crc);            /* CRC in network order */
        int size = sizeof(ncrc);               /* size of crc field */
        memcpy(crcdata, &ncrc, size);          /* append crc to packet */
        crc_len = len + size;                  /* set packet crc length */
    } else {
        crc_len = 0;                           /* appending crc would destroy packet */
    }
    return crc_len;
}

/* Append Ethernet CRC bytes directly to the packet buffer used by the polled
   receive path.  Threaded receive cannot use this helper because it queues a
   separate copy of the packet and stores generated CRC bytes beside that copy
   instead of modifying the callback buffer in place. */
int eth_add_packet_crc32(uint8_t *msg, int len)
{
    int crc_len;

    if (len <= ETH_MAX_PACKET) {
        crc_len = eth_get_packet_crc32_data(msg, len, &msg[len]); /* append crc to packet */
    } else {
        crc_len = 0;                                              /* appending crc would destroy packet */
    }
    return crc_len;
}
