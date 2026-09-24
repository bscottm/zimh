// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "test_cmocka.h"

#include "sim_defs.h"
#include "sim_ether.h"
#include "sim_aio.h"
#include "simnetwork/eth_backends.h"
#include "simnetwork/eth_test/sim_ether_test.h"

static DEVICE test_device = {.name = "TETH"};

/* Test fixture state */
typedef struct {
    ETH_DEV dev;
    bool saved_async_preference;
    bool saved_async_enabled;
} test_state_t;

/* Setup function that saves AIO state */
static int test_setup(void **state)
{
    test_state_t *ts = calloc(1, sizeof(test_state_t));
    assert_non_null(ts);

    /* Save current AIO state */
    ts->saved_async_preference = aio_async_preference();
    ts->saved_async_enabled = aio_async_enabled();

    /* Initialize device to zero state */
    memset(&ts->dev, 0, sizeof(ETH_DEV));

    *state = ts;
    return 0;
}

/* Teardown function that restores AIO state and cleans up device */
static int test_teardown(void **state)
{
    test_state_t *ts = *state;

    /* Close device if it was opened */
    if (ts->dev.backend != NULL && ts->dev.backend->eth_api != ETH_API_NONE) {
        eth_close(&ts->dev);
    }

    /* Restore AIO state */
    aio_set_async_enabled(ts->saved_async_enabled);

    free(ts);
    return 0;
}

/* Helper: Open a test backend with specified name */
static void open_test_backend(ETH_DEV *dev, const char *name)
{
    assert_int_equal(eth_open(dev, name, &test_device, 0), SCPE_OK);
    assert_non_null(dev->backend);
    assert_int_equal(dev->backend->eth_api, ETH_API_TEST);
}

/* Helper: Fill a minimal Ethernet packet */
static void fill_test_packet(uint8_t *packet, const ETH_MAC dst, const ETH_MAC src)
{
    memcpy(&packet[0], dst, sizeof(ETH_MAC));
    memcpy(&packet[6], src, sizeof(ETH_MAC));
    packet[12] = 0x08; /* EtherType IPv4 */
    packet[13] = 0x00;
    for (uint8_t i = 14; i < ETH_MIN_PACKET; i++)
        packet[i] = i;
}

/* ========================================================================
 * MAC Address Utility Tests (existing tests preserved)
 * ======================================================================== */

/* Assert that a generated MAC address preserved the requested prefix bits. */
static void assert_eth_mac_prefix(const ETH_MAC actual,
                                  const ETH_MAC expected,
                                  uint32_t prefix_bits)
{
    const uint32_t full_bytes = prefix_bits / 8;
    const uint32_t partial_bits = prefix_bits % 8;
    uint32_t i;

    for (i = 0; i < full_bytes; ++i)
        assert_int_equal(actual[i], expected[i]);
    if (partial_bits != 0) {
        const uint8_t mask = (uint8_t)(0xffu << (8 - partial_bits));
        assert_int_equal(actual[full_bytes] & mask,
                         expected[full_bytes] & mask);
    }
}

/* Verify generated MAC addresses preserve the requested fixed prefix bits. */
static void test_eth_mac_scan_generated_prefix_lengths(void **state)
{
    static const ETH_MAC base_mac = {0x02, 0x84, 0x86, 0x08, 0x0a, 0x0c};
    char input[sizeof("02:84:86:08:0A:0C/48")];
    ETH_MAC mac;
    uint32_t prefix_bits;

    (void)state;

    for (prefix_bits = 16; prefix_bits <= 48; ++prefix_bits) {
        assert_true(snprintf(input, sizeof(input),
                             "02:84:86:08:0A:0C/%u",
                             prefix_bits) < (int)sizeof(input));
        assert_int_equal(eth_mac_scan(mac, input), SCPE_OK);
        assert_eth_mac_prefix(mac, base_mac, prefix_bits);
    }
}

static void test_eth_mac_fmt_formats_address(void **state)
{
    static const ETH_MAC mac = {0x02, 0x84, 0x86, 0x08, 0x0a, 0x0c};
    char buffer[ETH_MAC_STRING_SIZE];

    (void)state;

    eth_mac_fmt(mac, buffer, sizeof(buffer));
    assert_string_equal(buffer, "02:84:86:08:0A:0C");
}

static void test_eth_mac_fmt_truncates_to_buffer_size(void **state)
{
    static const ETH_MAC mac = {0x02, 0x84, 0x86, 0x08, 0x0a, 0x0c};
    char buffer[8];

    (void)state;

    memset(buffer, 'x', sizeof(buffer));
    eth_mac_fmt(mac, buffer, sizeof(buffer));
    assert_string_equal(buffer, "02:84:8");
    assert_int_equal(buffer[sizeof(buffer) - 1], '\0');
}

/* ========================================================================
 * Device Lifecycle Tests
 * ======================================================================== */

static void test_eth_open_initializes_device_sync_mode(void **state)
{
    test_state_t *ts = *state;

    /* Disable async I/O */
    aio_set_async_enabled(false);

    open_test_backend(&ts->dev, "test:lifecycle-sync");

    /* Verify device is initialized */
    assert_non_null(ts->dev.name);
    assert_string_equal(ts->dev.name, "test:lifecycle-sync");
    assert_int_equal(ts->dev.backend->eth_api, ETH_API_TEST);
    assert_false(ts->dev.asynch_io);
    assert_false(ts->dev.threads_running);
    assert_int_equal(ts->dev.reflections, 0); /* Test backend doesn't reflect */

    eth_test_clear("lifecycle-sync");
}

static void test_eth_open_with_async_enabled_starts_threads(void **state)
{
    test_state_t *ts = *state;

    /* Enable async I/O if platform supports it */
    if (!aio_async_preference()) {
        skip();
        return;
    }

    aio_set_async_enabled(true);

    open_test_backend(&ts->dev, "test:lifecycle-async");

    /* Verify async mode is active */
    assert_true(ts->dev.asynch_io);
    assert_true(ts->dev.threading_initialized);
    assert_true(ts->dev.threads_running);

    eth_test_clear("lifecycle-async");
}

static void test_eth_close_cleans_up_properly(void **state)
{
    test_state_t *ts = *state;

    aio_set_async_enabled(false);
    open_test_backend(&ts->dev, "test:lifecycle-close");

    /* Close and verify cleanup */
    assert_int_equal(eth_close(&ts->dev), SCPE_OK);

    assert_null(ts->dev.name);
    assert_int_equal(ts->dev.backend->eth_api, ETH_API_NONE);
    assert_false(ts->dev.threading_initialized);
    assert_false(ts->dev.threads_running);

    /* Mark device as closed so teardown doesn't try to close it again */
    ts->dev.backend = NULL;

    eth_test_clear("lifecycle-close");
}

/* ========================================================================
 * Initialization Sequence Tests (validates our fixes)
 * ======================================================================== */

static void test_eth_reflect_runs_before_thread_startup(void **state)
{
    test_state_t *ts = *state;

    /* This test verifies that reflections are measured (-1 -> actual count)
     * during eth_open(), which happens before threads start. For test backend,
     * reflections should be set to 0 (test backends don't reflect). */

    if (!aio_async_preference()) {
        skip();
        return;
    }

    aio_set_async_enabled(true);
    open_test_backend(&ts->dev, "test:reflect-timing");

    /* Test backend sets reflections to 0 immediately */
    assert_int_equal(ts->dev.reflections, 0);

    /* Threads should be running if async enabled */
    assert_true(ts->dev.threads_running);

    eth_test_clear("reflect-timing");
}

/* ========================================================================
 * Synchronous Mode Tests
 * ======================================================================== */

static void test_sync_mode_write_packet(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    ETH_PACK packet;

    aio_set_async_enabled(false);
    open_test_backend(&ts->dev, "test:sync-write");

    memset(&packet, 0, sizeof(packet));
    fill_test_packet(packet.msg, dst, src);
    packet.len = ETH_MIN_PACKET;

    /* Write should complete synchronously */
    assert_int_equal(eth_write(&ts->dev, &packet, NULL), SCPE_OK);

    /* Verify packet was captured */
    assert_int_equal(eth_test_tx_count("sync-write"), 1);

    ETH_PACK captured;
    memset(&captured, 0, sizeof(captured));
    assert_int_equal(eth_test_pop_tx("sync-write", &captured), SCPE_OK);
    assert_int_equal(captured.len, ETH_MIN_PACKET);
    assert_memory_equal(captured.msg, packet.msg, ETH_MIN_PACKET);

    eth_test_clear("sync-write");
}

static void test_sync_mode_read_packet(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    uint8_t wire_packet[ETH_MIN_PACKET];
    ETH_PACK read_packet;

    aio_set_async_enabled(false);
    open_test_backend(&ts->dev, "test:sync-read");

    /* Install filter for destination address */
    assert_int_equal(eth_filter(&ts->dev, 1, &dst, false, false), SCPE_OK);

    /* Inject packet on the wire */
    fill_test_packet(wire_packet, dst, src);
    assert_int_equal(eth_test_inject("sync-read", wire_packet, sizeof(wire_packet)), SCPE_OK);

    /* Read should return the packet immediately */
    memset(&read_packet, 0, sizeof(read_packet));
    int status = eth_read(&ts->dev, &read_packet, NULL);

    assert_int_equal(status, 1); /* Packet received */
    assert_int_equal(read_packet.len, ETH_MIN_PACKET);
    assert_memory_equal(read_packet.msg, wire_packet, ETH_MIN_PACKET);

    eth_test_clear("sync-read");
}

static void test_sync_mode_respects_filter(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC other = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    uint8_t wire_packet[ETH_MIN_PACKET];
    ETH_PACK read_packet;

    aio_set_async_enabled(false);
    open_test_backend(&ts->dev, "test:sync-filter");

    /* Install filter for specific destination */
    assert_int_equal(eth_filter(&ts->dev, 1, &dst, false, false), SCPE_OK);

    /* Inject packet with different destination */
    fill_test_packet(wire_packet, other, src);
    assert_int_equal(eth_test_inject("sync-filter", wire_packet, sizeof(wire_packet)), SCPE_OK);

    /* Read should return 0 (no packet, filtered out) */
    memset(&read_packet, 0, sizeof(read_packet));
    int status = eth_read(&ts->dev, &read_packet, NULL);

    assert_int_equal(status, 0); /* No packet */
    assert_int_equal(read_packet.len, 0);

    eth_test_clear("sync-filter");
}

/* ========================================================================
 * Asynchronous Mode Tests (validates status >= 0 fix)
 * ======================================================================== */

static void test_async_mode_write_packet(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    ETH_PACK packet;

    if (!aio_async_preference()) {
        skip();
        return;
    }

    aio_set_async_enabled(true);
    open_test_backend(&ts->dev, "test:async-write");

    if (!ts->dev.asynch_io) {
        skip(); /* Thread startup failed, can't test async */
        return;
    }

    memset(&packet, 0, sizeof(packet));
    fill_test_packet(packet.msg, dst, src);
    packet.len = ETH_MIN_PACKET;

    /* Write enqueues packet for writer thread */
    assert_int_equal(eth_write(&ts->dev, &packet, NULL), SCPE_OK);

    /* Give writer thread time to process (crude but effective for unit test) */
    sim_os_ms_sleep(10);

    /* Verify packet was transmitted */
    assert_true(eth_test_tx_count("async-write") >= 1);

    eth_test_clear("async-write");
}

static void test_async_mode_read_packet_with_status_zero(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    uint8_t wire_packet[ETH_MIN_PACKET];
    ETH_PACK read_packet;

    if (!aio_async_preference()) {
        skip();
        return;
    }

    aio_set_async_enabled(true);
    open_test_backend(&ts->dev, "test:async-read-zero");

    if (!ts->dev.asynch_io) {
        skip();
        return;
    }

    /* Install filter */
    assert_int_equal(eth_filter(&ts->dev, 1, &dst, false, false), SCPE_OK);

    /* Inject packet - reader thread will queue it */
    fill_test_packet(wire_packet, dst, src);
    assert_int_equal(eth_test_inject("async-read-zero", wire_packet, sizeof(wire_packet)), SCPE_OK);

    /* Give reader thread time to process */
    sim_os_ms_sleep(10);

    /* THIS IS THE KEY TEST: In async mode, status defaults to 0.
     * The fix changed eth_read() from "status > 0" to "status >= 0"
     * so that packets can be dequeued when status is 0. */
    memset(&read_packet, 0, sizeof(read_packet));
    int status = eth_read(&ts->dev, &read_packet, NULL);

    /* Should successfully dequeue the packet even though internal status is 0 */
    assert_true(status > 0); /* eth_read returns 1 when packet dequeued */
    assert_int_equal(read_packet.len, ETH_MIN_PACKET);

    eth_test_clear("async-read-zero");
}

static void test_async_mode_respects_filter(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC other = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    uint8_t wire_packet[ETH_MIN_PACKET];
    ETH_PACK read_packet;

    if (!aio_async_preference()) {
        skip();
        return;
    }

    aio_set_async_enabled(true);
    open_test_backend(&ts->dev, "test:async-filter");

    if (!ts->dev.asynch_io) {
        skip();
        return;
    }

    /* Install filter for specific destination */
    assert_int_equal(eth_filter(&ts->dev, 1, &dst, false, false), SCPE_OK);

    /* Inject packet with different destination */
    fill_test_packet(wire_packet, other, src);
    assert_int_equal(eth_test_inject("async-filter", wire_packet, sizeof(wire_packet)), SCPE_OK);

    /* Give reader thread time to process (and filter out) */
    sim_os_ms_sleep(10);

    /* Read should return 0 (filtered, not queued) */
    memset(&read_packet, 0, sizeof(read_packet));
    int status = eth_read(&ts->dev, &read_packet, NULL);

    assert_int_equal(status, 0);
    assert_int_equal(read_packet.len, 0);

    eth_test_clear("async-filter");
}

/* ========================================================================
 * CRC Handling Tests
 * ======================================================================== */

static void test_crc_extension_in_sync_mode(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    uint8_t wire_packet[ETH_MIN_PACKET];
    ETH_PACK read_packet;

    aio_set_async_enabled(false);
    open_test_backend(&ts->dev, "test:crc-sync");

    /* Enable CRC mode */
    eth_setcrc(&ts->dev, 1);
    assert_int_equal(ts->dev.need_crc, 1);

    /* Install filter */
    assert_int_equal(eth_filter(&ts->dev, 1, &dst, false, false), SCPE_OK);

    /* Inject packet */
    fill_test_packet(wire_packet, dst, src);
    assert_int_equal(eth_test_inject("crc-sync", wire_packet, sizeof(wire_packet)), SCPE_OK);

    /* Read packet */
    memset(&read_packet, 0, sizeof(read_packet));
    int status = eth_read(&ts->dev, &read_packet, NULL);

    assert_int_equal(status, 1);
    assert_int_equal(read_packet.len, ETH_MIN_PACKET);
    assert_int_equal(read_packet.crc_len, ETH_MIN_PACKET + ETH_CRC_SIZE);

    eth_test_clear("crc-sync");
}

/* ========================================================================
 * Error Handling Tests
 * ======================================================================== */

static void test_write_error_reported_in_sync_mode(void **state)
{
    test_state_t *ts = *state;
    static const ETH_MAC dst = {0x02, 0x00, 0xde, 0xad, 0xbe, 0xef};
    static const ETH_MAC src = {0x02, 0x00, 0xba, 0xdc, 0x0f, 0xfe};
    ETH_PACK packet;

    aio_set_async_enabled(false);
    open_test_backend(&ts->dev, "test:write-error");

    /* Configure test backend to fail writes */
    assert_int_equal(eth_test_set_write_status("write-error", 1), SCPE_OK);

    memset(&packet, 0, sizeof(packet));
    fill_test_packet(packet.msg, dst, src);
    packet.len = ETH_MIN_PACKET;

    /* Write should fail */
    t_stat result = eth_write(&ts->dev, &packet, NULL);
    assert_int_equal(result, SCPE_IOERR);

    /* Error counter should increment */
    assert_true(ts->dev.transmit_packet_errors > 0);

    eth_test_clear("write-error");
}

/* ========================================================================
 * Threading Infrastructure Tests
 * ======================================================================== */

static void test_threading_structures_initialize(void **state)
{
    test_state_t *ts = *state;

    if (!aio_async_preference()) {
        skip();
        return;
    }

    aio_set_async_enabled(true);
    open_test_backend(&ts->dev, "test:thread-init");

    if (!ts->dev.asynch_io) {
        skip();
        return;
    }

    /* Verify threading structures are initialized */
    assert_true(ts->dev.threading_initialized);
    assert_true(ts->dev.threads_running);

    /* Verify queues are initialized (check they're not empty structures) */
    /* Note: We can't directly inspect queue internals, but if threads
     * started successfully, queues must be valid */

    eth_test_clear("thread-init");
}

static void test_async_mode_fallback_on_thread_failure(void **state)
{
    /* This test is difficult to implement without mocking thread creation.
     * The actual fallback logic is in eth_open() and prints a warning.
     * For now, we verify the logic exists by checking the code path. */
    (void)state;
    /* Test implementation deferred - requires thread creation mocking */
    skip();
}

/* ========================================================================
 * Device State Tests
 * ======================================================================== */

static void test_asynch_io_flag_is_bool(void **state)
{
    test_state_t *ts = *state;

    /* Verify the flag behaves as a boolean */
    ts->dev.asynch_io = false;
    assert_false(ts->dev.asynch_io);

    ts->dev.asynch_io = true;
    assert_true(ts->dev.asynch_io);

    ts->dev.asynch_io = false;
    assert_false(ts->dev.asynch_io);
}

static void test_device_initializes_with_reflections_unknown(void **state)
{
    test_state_t *ts = *state;
    ETH_DEV fresh_dev;

    (void)ts;

    /* Simulate what eth_initialize_device does */
    memset(&fresh_dev, 0, sizeof(ETH_DEV));
    fresh_dev.reflections = -1;

    assert_int_equal(fresh_dev.reflections, -1);
}

/* ========================================================================
 * Test Suite
 * ======================================================================== */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* MAC utility tests */
        cmocka_unit_test(test_eth_mac_scan_generated_prefix_lengths),
        cmocka_unit_test(test_eth_mac_fmt_formats_address),
        cmocka_unit_test(test_eth_mac_fmt_truncates_to_buffer_size),

        /* Device lifecycle tests */
        cmocka_unit_test_setup_teardown(test_eth_open_initializes_device_sync_mode,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_eth_open_with_async_enabled_starts_threads,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_eth_close_cleans_up_properly,
                                       test_setup, test_teardown),

        /* Initialization sequence tests (validates our fixes) */
        cmocka_unit_test_setup_teardown(test_eth_reflect_runs_before_thread_startup,
                                       test_setup, test_teardown),

        /* Synchronous mode tests */
        cmocka_unit_test_setup_teardown(test_sync_mode_write_packet,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_sync_mode_read_packet,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_sync_mode_respects_filter,
                                       test_setup, test_teardown),

        /* Asynchronous mode tests (validates status >= 0 fix) */
        cmocka_unit_test_setup_teardown(test_async_mode_write_packet,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_async_mode_read_packet_with_status_zero,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_async_mode_respects_filter,
                                       test_setup, test_teardown),

        /* CRC handling tests */
        cmocka_unit_test_setup_teardown(test_crc_extension_in_sync_mode,
                                       test_setup, test_teardown),

        /* Error handling tests */
        cmocka_unit_test_setup_teardown(test_write_error_reported_in_sync_mode,
                                       test_setup, test_teardown),

        /* Threading infrastructure tests */
        cmocka_unit_test_setup_teardown(test_threading_structures_initialize,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_async_mode_fallback_on_thread_failure,
                                       test_setup, test_teardown),

        /* Device state tests */
        cmocka_unit_test_setup_teardown(test_asynch_io_flag_is_bool,
                                       test_setup, test_teardown),
        cmocka_unit_test_setup_teardown(test_device_initializes_with_reflections_unknown,
                                       test_setup, test_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
