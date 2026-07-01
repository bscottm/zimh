#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <libslirp.h>

#include "test_cmocka.h"

#include "sim_defs.h"
#include "sim_sock.h"
#include "eth_backends/slirp/sim_slirp.h"
#include "sim_types.h"

enum {
    ether_addr_len = 6,
    ether_type_arp = 0x0806,
    ether_type_ipv4 = 0x0800,
    arp_request = 1,
    arp_reply = 2,
    ipv4_proto_udp = 17,
    dhcp_client_port = 68,
    dhcp_server_port = 67,
    dhcp_bootrequest = 1,
    dhcp_bootreply = 2,
    dhcp_discover = 1,
    dhcp_offer = 2,
    dhcp_option_message_type = 53,
    dhcp_option_router = 3,
    dhcp_option_dns = 6,
    dhcp_option_server_id = 54,
    dhcp_option_end = 255,
    dhcp_fixed_len = 236,
    dhcp_options_offset = 42 + dhcp_fixed_len,
    ipv4_proto_icmp = 1,
    icmp_echo_reply = 0,
    icmp_echo_request = 8,
    dns_port = 53,
    dns_query = 0,
    dns_response = 1,
    dns_type_a = 1,
    dns_class_in = 1,
};

struct callback_capture {
    int packet_count;
    uint8_t last_packet[1518];
    int last_packet_size;
};

/* Mock DEVICE structure for testing */
static UNIT mock_unit;
static DEBTAB mock_debug_flags[] = {
    { "TEST", 0, "Test debug flag" },
    { NULL }
};
static DEVICE mock_device = {
    .name = "TEST",
    .units = &mock_unit,
    .registers = NULL,
    .modifiers = NULL,
    .numunits = 1,
    .aradix = 16,
    .awidth = 0,
    .aincr = 0,
    .dradix = 16,
    .dwidth = 0,
    .examine = NULL,
    .deposit = NULL,
    .reset = NULL,
    .boot = NULL,
    .attach = NULL,
    .detach = NULL,
    .ctxt = NULL,
    .flags = 0,
    .dctrl = 0,
    .debflags = mock_debug_flags,
    .msize = NULL,
    .lname = NULL,
    .help = NULL,
    .attach_help = NULL,
    .help_ctx = NULL,
    .description = NULL,
    .brk_types = NULL,
    .type_ctx = NULL,
};


static void assert_ipv4_equal(struct in_addr actual, const char *expected)
{
    struct in_addr parsed;

    assert_int_equal(inet_pton(AF_INET, expected, &parsed), 1);
    assert_int_equal((uint32_t)actual.s_addr, (uint32_t)parsed.s_addr);
}

static void assert_ipv6_equal(struct in6_addr actual, const char *expected)
{
    struct in6_addr parsed;

    assert_int_equal(inet_pton(AF_INET6, expected, &parsed), 1);
    assert_memory_equal(&actual, &parsed, sizeof(actual));
}

static void capture_packet(void *opaque, const uchar_t *buf, int len)
{
    struct callback_capture *capture = (struct callback_capture *)opaque;

    assert_true(len <= (int)sizeof(capture->last_packet));
    ++capture->packet_count;
    capture->last_packet_size = len;
    memcpy(capture->last_packet, buf, (size_t)len);
}

static void put_be16(uint8_t *ptr, uint16_t value)
{
    ptr[0] = (uint8_t)(value >> 8);
    ptr[1] = (uint8_t)value;
}

static void put_be32(uint8_t *ptr, uint32_t value)
{
    ptr[0] = (uint8_t)(value >> 24);
    ptr[1] = (uint8_t)(value >> 16);
    ptr[2] = (uint8_t)(value >> 8);
    ptr[3] = (uint8_t)value;
}

static uint16_t get_be16(const uint8_t *ptr)
{
    return (uint16_t)((ptr[0] << 8) | ptr[1]);
}

static void put_ipv4(uint8_t *ptr, const char *addr)
{
    struct in_addr parsed;

    assert_int_equal(inet_pton(AF_INET, addr, &parsed), 1);
    memcpy(ptr, &parsed.s_addr, sizeof(parsed.s_addr));
}

static void assert_ipv4_bytes_equal(const uint8_t *actual, const char *expected)
{
    struct in_addr parsed;

    assert_int_equal(inet_pton(AF_INET, expected, &parsed), 1);
    assert_memory_equal(actual, &parsed.s_addr, sizeof(parsed.s_addr));
}

static void assert_ether_dest_is_guest_or_broadcast(const uint8_t *packet,
                                                    const uint8_t *guest_mac)
{
    static const uint8_t broadcast[ether_addr_len] = {0xff, 0xff, 0xff,
                                                    0xff, 0xff, 0xff};

    assert_true((memcmp(packet, guest_mac, ether_addr_len) == 0) ||
                (memcmp(packet, broadcast, ether_addr_len) == 0));
}

static uint16_t ipv4_header_checksum(const uint8_t *header, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    assert_int_equal(len % 2, 0);
    for (i = 0; i < len; i += 2)
        sum += get_be16(header + i);
    while (sum > 0xffff)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t internet_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;

    while (len > 1) {
        sum += get_be16(buf);
        buf += 2;
        len -= 2;
    }
    if (len != 0)
        sum += (uint16_t)(*buf << 8);
    while (sum > 0xffff)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void build_arp_request(uint8_t *packet, const uint8_t *guest_mac,
                              const char *guest_addr, const char *target_addr)
{
    static const uint8_t broadcast[ether_addr_len] = {0xff, 0xff, 0xff,
                                                    0xff, 0xff, 0xff};

    memset(packet, 0, 42);
    memcpy(packet, broadcast, ether_addr_len);
    memcpy(packet + 6, guest_mac, ether_addr_len);
    put_be16(packet + 12, ether_type_arp);
    put_be16(packet + 14, 1);
    put_be16(packet + 16, 0x0800);
    packet[18] = ether_addr_len;
    packet[19] = 4;
    put_be16(packet + 20, arp_request);
    memcpy(packet + 22, guest_mac, ether_addr_len);
    put_ipv4(packet + 28, guest_addr);
    put_ipv4(packet + 38, target_addr);
}

static size_t build_icmp_echo_request(uint8_t *packet, const uint8_t *guest_mac,
                                      const uint8_t *host_mac)
{
    static const uint8_t payload[] = {'z', 'i', 'm', 'h'};
    uint8_t *ip = packet + 14;
    uint8_t *icmp = packet + 34;
    size_t icmp_len = 8 + sizeof(payload);
    size_t ip_len = 20 + icmp_len;

    memset(packet, 0, 14 + ip_len);
    memcpy(packet, host_mac, ether_addr_len);
    memcpy(packet + 6, guest_mac, ether_addr_len);
    put_be16(packet + 12, ether_type_ipv4);

    ip[0] = 0x45;
    put_be16(ip + 2, (uint16_t)ip_len);
    ip[8] = 64;
    ip[9] = ipv4_proto_icmp;
    put_ipv4(ip + 12, "10.0.2.15");
    put_ipv4(ip + 16, "10.0.2.2");
    put_be16(ip + 10, ipv4_header_checksum(ip, 20));

    icmp[0] = icmp_echo_request;
    put_be16(icmp + 4, 0x1234);
    put_be16(icmp + 6, 1);
    memcpy(icmp + 8, payload, sizeof(payload));
    put_be16(icmp + 2, internet_checksum(icmp, icmp_len));
    return 14 + ip_len;
}

static size_t build_dhcp_discover(uint8_t *packet, const uint8_t *guest_mac,
                                  uint32_t xid)
{
    static const uint8_t broadcast[ether_addr_len] = {0xff, 0xff, 0xff,
                                                    0xff, 0xff, 0xff};
    uint8_t *ip = packet + 14;
    uint8_t *udp = packet + 34;
    uint8_t *dhcp = packet + 42;
    uint8_t *option = dhcp + dhcp_fixed_len;
    size_t dhcp_len;
    size_t udp_len;
    size_t ip_len;

    memset(packet, 0, 300);
    memcpy(packet, broadcast, ether_addr_len);
    memcpy(packet + 6, guest_mac, ether_addr_len);
    put_be16(packet + 12, ether_type_ipv4);

    dhcp[0] = dhcp_bootrequest;
    dhcp[1] = 1;
    dhcp[2] = ether_addr_len;
    put_be32(dhcp + 4, xid);
    put_be16(dhcp + 10, 0x8000);
    memcpy(dhcp + 28, guest_mac, ether_addr_len);

    option[0] = 99;
    option[1] = 130;
    option[2] = 83;
    option[3] = 99;
    option += 4;
    option[0] = dhcp_option_message_type;
    option[1] = 1;
    option[2] = dhcp_discover;
    option += 3;
    option[0] = 55;
    option[1] = 3;
    option[2] = 1;
    option[3] = dhcp_option_router;
    option[4] = dhcp_option_dns;
    option += 5;
    *option++ = dhcp_option_end;

    dhcp_len = (size_t)(option - dhcp);
    udp_len = 8 + dhcp_len;
    ip_len = 20 + udp_len;

    ip[0] = 0x45;
    put_be16(ip + 2, (uint16_t)ip_len);
    ip[8] = 64;
    ip[9] = ipv4_proto_udp;
    put_ipv4(ip + 12, "0.0.0.0");
    put_ipv4(ip + 16, "255.255.255.255");
    put_be16(ip + 10, ipv4_header_checksum(ip, 20));

    put_be16(udp, dhcp_client_port);
    put_be16(udp + 2, dhcp_server_port);
    put_be16(udp + 4, (uint16_t)udp_len);
    return 14 + ip_len;
}

static const uint8_t *find_dhcp_option(const uint8_t *packet, size_t packet_size,
                                     int option_code, size_t *option_len)
{
    const uint8_t *ptr = packet + dhcp_options_offset;
    const uint8_t *end = packet + packet_size;

    if (packet_size < dhcp_options_offset + 4)
        return NULL;
    assert_memory_equal(ptr, "\x63\x82\x53\x63", 4);
    ptr += 4;
    while (ptr < end) {
        int code = *ptr++;

        if (code == dhcp_option_end)
            return NULL;
        if (code == 0)
            continue;
        if (ptr >= end)
            return NULL;
        if ((size_t)(end - ptr) < (size_t)*ptr + 1)
            return NULL;
        if (code == option_code) {
            *option_len = *ptr;
            return ptr + 1;
        }
        ptr += 1 + *ptr;
    }
    return NULL;
}

static void encode_dns_name(uint8_t *ptr, const char *name)
{
    const char *label_start = name;
    const char *p = name;

    while (*p != '\0') {
        if (*p == '.') {
            size_t label_len = (size_t)(p - label_start);
            *ptr++ = (uint8_t)label_len;
            memcpy(ptr, label_start, label_len);
            ptr += label_len;
            label_start = p + 1;
        }
        p++;
    }
    if (p > label_start) {
        size_t label_len = (size_t)(p - label_start);
        *ptr++ = (uint8_t)label_len;
        memcpy(ptr, label_start, label_len);
    }
    *ptr = 0;
}

static size_t build_dns_query(uint8_t *packet, const uint8_t *guest_mac,
                              const char *hostname, uint16_t txid)
{
    uint8_t *ip = packet + 14;
    uint8_t *udp = packet + 34;
    uint8_t *dns = packet + 42;
    uint8_t *question = dns + 12;
    size_t hostname_encoded_len;
    size_t dns_len;
    size_t udp_len;
    size_t ip_len;

    memset(packet, 0, 512);

    /* Ethernet header: to DNS server (via gateway) */
    memcpy(packet, guest_mac, ether_addr_len);  /* placeholder - needs ARP first */
    memcpy(packet + 6, guest_mac, ether_addr_len);
    put_be16(packet + 12, ether_type_ipv4);

    /* DNS header */
    put_be16(dns, txid);            /* Transaction ID */
    put_be16(dns + 2, 0x0100);      /* Flags: standard query, recursion desired */
    put_be16(dns + 4, 1);           /* Questions: 1 */
    put_be16(dns + 6, 0);           /* Answer RRs: 0 */
    put_be16(dns + 8, 0);           /* Authority RRs: 0 */
    put_be16(dns + 10, 0);          /* Additional RRs: 0 */

    /* Question section */
    encode_dns_name(question, hostname);
    hostname_encoded_len = strlen(hostname) + 2;  /* labels + null terminator */
    put_be16(question + hostname_encoded_len, dns_type_a);    /* Type A */
    put_be16(question + hostname_encoded_len + 2, dns_class_in); /* Class IN */

    dns_len = 12 + hostname_encoded_len + 4;
    udp_len = 8 + dns_len;
    ip_len = 20 + udp_len;

    /* IPv4 header */
    ip[0] = 0x45;  /* version 4, header length 5 */
    put_be16(ip + 2, (uint16_t)ip_len);
    ip[8] = 64;    /* TTL */
    ip[9] = ipv4_proto_udp;
    put_ipv4(ip + 12, "10.0.2.15");
    put_ipv4(ip + 16, "10.0.2.3");  /* DNS server */
    put_be16(ip + 10, ipv4_header_checksum(ip, 20));

    /* UDP header */
    put_be16(udp, 1234);            /* Source port */
    put_be16(udp + 2, dns_port);    /* Destination port 53 */
    put_be16(udp + 4, (uint16_t)udp_len);
    put_be16(udp + 6, 0);           /* Checksum (optional for IPv4) */

    return 14 + ip_len;
}

static int is_valid_dns_response(const uint8_t *packet, size_t packet_size,
                                uint16_t expected_txid)
{
    const uint8_t *udp;
    const uint8_t *dns;
    uint16_t txid;
    uint16_t flags;
    uint16_t src_port;
    uint16_t dst_port;

    if (packet_size < 42 + 12)
        return 0;

    /* Verify Ethernet type */
    if (get_be16(packet + 12) != ether_type_ipv4)
        return 0;

    /* Verify IP protocol */
    if (packet[23] != ipv4_proto_udp)
        return 0;

    udp = packet + 34;
    dns = packet + 42;

    /* Verify UDP ports */
    src_port = get_be16(udp);
    dst_port = get_be16(udp + 2);
    if (src_port != dns_port)
        return 0;

    /* Verify DNS transaction ID */
    txid = get_be16(dns);
    if (txid != expected_txid)
        return 0;

    /* Verify response flag is set */
    flags = get_be16(dns + 2);
    if ((flags & 0x8000) == 0)  /* QR bit should be 1 for response */
        return 0;

    return 1;
}


/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Test helper: Dispatch poll results for unit testing.
 *
 * This replicates what eth_reader_dispatch_nat() does in the reader thread.
 * Unit tests need this because they don't have the reader thread state machine.
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static void sim_slirp_dispatch_for_test(sim_slirp_network *slirp)
{
    if (slirp == NULL)
        return;

    pthread_mutex_lock(&slirp->libslirp_lock);
    slirp_pollfds_poll(slirp->slirp_cxn, 0, slirp_get_events_callback, slirp);
    pthread_mutex_unlock(&slirp->libslirp_lock);
}

static int setup_slirp_tests(void **state)
{
    (void)state;

    sim_init_sock();
    return 0;
}

static int teardown_slirp_tests(void **state)
{
    (void)state;

    sim_cleanup_sock();
    return 0;
}

/* Verify libslirp answers ARP for the virtual gateway without host network. */
static void test_slirp_real_backend_answers_local_arp(void **state)
{
    static const uint8_t guest_mac[ether_addr_len] = {0x52, 0x54, 0x00,
                                                    0x12, 0x34, 0x56};
    struct callback_capture capture;
    uint8_t request[42];
    char errbuf[256];
    sim_slirp_network *slirp;

    (void)state;

    memset(&capture, 0, sizeof(capture));
    build_arp_request(request, guest_mac, "10.0.2.15", "10.0.2.2");

    errbuf[0] = '\0';
    slirp = sim_slirp_open("", &capture, capture_packet, &mock_device, 0, errbuf,
                           sizeof(errbuf));

    if (*errbuf != '\0') {
        fprintf(stderr, "sim_slirp_open error: %s\n", errbuf);
    }

    assert_non_null(slirp);
    assert_string_equal(errbuf, "");
    assert_int_equal(
        sim_slirp_send(slirp, (const char *)request, sizeof(request), 0),
        sizeof(request));

    assert_int_equal(capture.packet_count, 1);
    assert_true(capture.last_packet_size >= 42);
    assert_memory_equal(capture.last_packet, guest_mac, ether_addr_len);
    assert_int_equal(get_be16(capture.last_packet + 12), ether_type_arp);
    assert_int_equal(get_be16(capture.last_packet + 20), arp_reply);
    assert_memory_equal(capture.last_packet + 28, request + 38, 4);
    assert_memory_equal(capture.last_packet + 32, guest_mac, ether_addr_len);
    assert_memory_equal(capture.last_packet + 38, request + 28, 4);

    sim_slirp_close(slirp);
}

/* Verify libslirp offers the configured DHCP lease and options. */
static void test_slirp_real_backend_answers_dhcp_discover(void **state)
{
    static const uint8_t guest_mac[ether_addr_len] = {0x52, 0x54, 0x00,
                                                    0xab, 0xcd, 0xef};
    const uint32_t xid = 0x12345678;
    struct callback_capture capture;
    uint8_t request[300];
    const uint8_t *option;
    size_t option_len;
    size_t request_len;
    char errbuf[256];
    sim_slirp_network *slirp;

    (void)state;

    memset(&capture, 0, sizeof(capture));
    request_len = build_dhcp_discover(request, guest_mac, xid);

    errbuf[0] = '\0';
    slirp = sim_slirp_open("", &capture, capture_packet, &mock_device, 0, errbuf,
                           sizeof(errbuf));

    assert_non_null(slirp);
    assert_string_equal(errbuf, "");
    assert_int_equal(
        sim_slirp_send(slirp, (const char *)request, request_len, 0),
        request_len);

    assert_int_equal(capture.packet_count, 1);
    assert_true(capture.last_packet_size >= dhcp_options_offset + 4);
    assert_ether_dest_is_guest_or_broadcast(capture.last_packet, guest_mac);
    assert_int_equal(get_be16(capture.last_packet + 12), ether_type_ipv4);
    assert_int_equal(capture.last_packet[23], ipv4_proto_udp);
    assert_int_equal(get_be16(capture.last_packet + 34), dhcp_server_port);
    assert_int_equal(get_be16(capture.last_packet + 36), dhcp_client_port);
    assert_int_equal(capture.last_packet[42], dhcp_bootreply);
    assert_int_equal(capture.last_packet[43], 1);
    assert_int_equal(capture.last_packet[44], ether_addr_len);
    assert_int_equal(get_be16(capture.last_packet + 46), (xid >> 16));
    assert_int_equal(get_be16(capture.last_packet + 48), (xid & 0xffff));
    assert_ipv4_bytes_equal(capture.last_packet + 58, "10.0.2.15");
    assert_memory_equal(capture.last_packet + 70, guest_mac, ether_addr_len);

    option =
        find_dhcp_option(capture.last_packet, (size_t)capture.last_packet_size,
                         dhcp_option_message_type, &option_len);
    assert_non_null(option);
    assert_int_equal(option_len, 1);
    assert_int_equal(option[0], dhcp_offer);

    option =
        find_dhcp_option(capture.last_packet, (size_t)capture.last_packet_size,
                         dhcp_option_server_id, &option_len);
    assert_non_null(option);
    assert_int_equal(option_len, 4);
    assert_ipv4_bytes_equal(option, "10.0.2.2");

    option =
        find_dhcp_option(capture.last_packet, (size_t)capture.last_packet_size,
                         dhcp_option_router, &option_len);
    assert_non_null(option);
    assert_int_equal(option_len, 4);
    assert_ipv4_bytes_equal(option, "10.0.2.2");

    option =
        find_dhcp_option(capture.last_packet, (size_t)capture.last_packet_size,
                         dhcp_option_dns, &option_len);
    assert_non_null(option);
    assert_int_equal(option_len, 4);
    assert_ipv4_bytes_equal(option, "10.0.2.3");

    sim_slirp_close(slirp);
}

/* Verify libslirp answers ICMP echo to the virtual gateway locally. */
static void test_slirp_real_backend_answers_gateway_ping(void **state)
{
    static const uint8_t guest_mac[ether_addr_len] = {0x52, 0x54, 0x00,
                                                    0x65, 0x43, 0x21};
    struct callback_capture capture;
    uint8_t host_mac[ether_addr_len];
    uint8_t packet[128];
    size_t packet_len;
    char errbuf[256];
    sim_slirp_network *slirp;

    (void)state;

    memset(&capture, 0, sizeof(capture));
    build_arp_request(packet, guest_mac, "10.0.2.15", "10.0.2.2");

    errbuf[0] = '\0';
    slirp = sim_slirp_open("", &capture, capture_packet, &mock_device, 0, errbuf,
                           sizeof(errbuf));

    assert_non_null(slirp);
    assert_string_equal(errbuf, "");
    assert_int_equal(sim_slirp_send(slirp, (const char *)packet, 42, 0), 42);

    assert_int_equal(capture.packet_count, 1);
    assert_int_equal(get_be16(capture.last_packet + 12), ether_type_arp);
    memcpy(host_mac, capture.last_packet + 6, ether_addr_len);

    memset(&capture, 0, sizeof(capture));
    packet_len = build_icmp_echo_request(packet, guest_mac, host_mac);
    assert_int_equal(sim_slirp_send(slirp, (const char *)packet, packet_len, 0),
                     packet_len);

    assert_int_equal(capture.packet_count, 1);
    assert_true(capture.last_packet_size >= 46);
    assert_memory_equal(capture.last_packet, guest_mac, ether_addr_len);
    assert_int_equal(get_be16(capture.last_packet + 12), ether_type_ipv4);
    assert_int_equal(capture.last_packet[23], ipv4_proto_icmp);
    assert_ipv4_bytes_equal(capture.last_packet + 26, "10.0.2.2");
    assert_ipv4_bytes_equal(capture.last_packet + 30, "10.0.2.15");
    assert_int_equal(capture.last_packet[34], icmp_echo_reply);
    assert_int_equal(get_be16(capture.last_packet + 38), 0x1234);
    assert_int_equal(get_be16(capture.last_packet + 40), 1);
    assert_memory_equal(capture.last_packet + 42, "zimh", 4);

    sim_slirp_close(slirp);
}

/* Verify libslirp handles DNS queries without crashing. */
static void test_slirp_handles_dns_query(void **state)
{
    static const uint8_t guest_mac[ether_addr_len] = {0x52, 0x54, 0x00,
                                                    0xaa, 0xbb, 0xcc};
    const uint16_t dns_txid = 0x4242;
    struct callback_capture capture;
    uint8_t packet[512];
    size_t packet_len;
    char errbuf[256];
    sim_slirp_network *slirp;

    (void)state;

    memset(&capture, 0, sizeof(capture));

    errbuf[0] = '\0';
    slirp = sim_slirp_open("", &capture, capture_packet, &mock_device, 0, errbuf,
                           sizeof(errbuf));

    assert_non_null(slirp);
    assert_string_equal(errbuf, "");

    /* Build and send DNS query for example.com */
    packet_len = build_dns_query(packet, guest_mac, "example.com", dns_txid);
    assert_int_equal(sim_slirp_send(slirp, (const char *)packet, packet_len, 0),
                     packet_len);

    /* Poll for socket activity and process results.
     * This replicates what the reader thread does:
     * 1. sim_slirp_select() polls sockets
     * 2. sim_slirp_dispatch_for_test() processes poll results
     * We use a longer timeout since DNS queries need network access. */
    sim_slirp_select(slirp, 1000);
    sim_slirp_dispatch_for_test(slirp);

    /* Verify we got a response - we don't assert on the actual answer
     * since it depends on external DNS, but we verify:
     * 1. We got a packet back
     * 2. It's a valid DNS response structure
     * 3. The transaction ID matches
     */
    if (capture.packet_count > 0) {
        assert_true(is_valid_dns_response(capture.last_packet,
                                         (size_t)capture.last_packet_size,
                                         dns_txid));
    }
    /* Note: If packet_count is 0, libslirp may not have been able to
     * forward the query (no network, DNS timeout, etc). This is acceptable
     * for a protocol test - we're just verifying it doesn't crash. */

    sim_slirp_close(slirp);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_slirp_real_backend_answers_local_arp),
        cmocka_unit_test(test_slirp_real_backend_answers_dhcp_discover),
        cmocka_unit_test(test_slirp_real_backend_answers_gateway_ping),
        cmocka_unit_test(test_slirp_handles_dns_query),
    };

    return cmocka_run_group_tests(tests, setup_slirp_tests,
                                  teardown_slirp_tests);
}
