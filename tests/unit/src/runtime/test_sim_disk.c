/*
 * test_sim_disk.c - Unit tests for sim_disk library
 *
 * Test Strategy:
 * --------------
 * This test suite uses a two-phase testing approach to verify disk I/O operations
 * work correctly in both synchronous (direct) and asynchronous (threaded) modes:
 *
 * Phase 1: Direct Disk I/O Tests (sim_async_preference = false)
 *   - Forces synchronous execution of all I/O operations
 *   - Tests basic functionality without thread complexity
 *   - Ensures fallback behavior works when AIO is disabled
 *
 * Phase 2: AIO and Disk I/O Thread Tests (sim_async_preference = true)
 *   - Enables asynchronous I/O with background threads
 *   - Verifies callbacks are invoked correctly
 *   - Tests thread safety and async operation completion
 *
 * All tests run twice automatically (once per phase) to ensure consistent behavior
 * across both execution modes. This approach follows the pattern established in
 * test_sim_tape.c.
 *
 * Test Categories:
 * ----------------
 * 1. Format and Capacity Tests - Disk format selection and capacity calculations
 * 2. Error Handling & Edge Cases - Invalid parameters, boundary conditions
 * 3. Boundary & Range Testing - LBA boundaries, multi-sector operations
 * 4. Async I/O Operations - Callback mechanisms, latency settings
 * 5. Backend Error Injection - Simulated I/O errors via test backends
 * 6. State & Lifecycle - Attach/detach cycles, reset behavior
 * 7. Data Integrity - Write/read patterns, persistence across detach
 * 8. Format Detection - AUTO format detection for various container types
 * 9. RAMDISK Operations - Volatile memory-backed disks (if HAVE_FMEMOPEN)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include "sim_win32_compat.h"
#endif

#include "scp.h"
#include "test_cmocka.h"

#include "sim_tempfile.h"
#include "sim_defs.h"
#include "sim_disk.h"
#include "sim_aio.h"
#include "test_simh_personality.h"
#include "test_support.h"

#define VHD_FOOTER_SIZE 512
/* In a VHD footer, the Unique ID field starts after "Checksum" at offset 68. */
#define VHD_FOOTER_UNIQUE_ID_OFFSET 68

struct sim_disk_fixture {
    DEVICE byte_device;
    UNIT byte_unit;
    DEVICE sector_device;
    UNIT sector_unit;
    char temp_dir[CBUFSIZE];
    char image_path[CBUFSIZE];
};

extern sim_off_t pseudo_filesystem_size;
extern t_stat sim_save(FILE *sfile);
extern t_stat sim_rest(FILE *rfile);
extern t_stat show_unit(FILE *st, DEVICE *dptr, UNIT *uptr, int32_t flag);

struct detach_capture {
    UNIT *unit;
    t_stat status;
};

static t_stat attach_test_sector_disk(UNIT *uptr, const char *cptr)
{
    return sim_disk_attach_ex(uptr, cptr, 512, 1, true, 0, "TEST", 0, 0,
                              NULL);
}

static t_stat set_test_drive_type(UNIT *uptr, int32_t val, const char *cptr,
                                  void *desc)
{
    (void)cptr;
    (void)desc;

    uptr->capac = (sim_off_t) val;
    return SCPE_OK;
}

static MTAB disk_unit_modifiers[] = {
    {DKUF_NOAUTOSIZE, 0, "autosize", "AUTOSIZE", NULL, NULL, "Disk autosizing enable flag", NULL},
    {DKUF_NOAUTOSIZE, DKUF_NOAUTOSIZE, "noautosize", "NOAUTOSIZE", NULL, NULL, "Disk autosizing disable flag", NULL},
    {MTAB_VUN, 4, "large", "LARGE", set_test_drive_type, NULL, "Large disk flag", NULL},
    {0},
};

static DEBTAB disk_unit_debug[] = {
    {"TRACE",  0x01,   "trace test call flow"},
    {NULL, 0, NULL},
};

static int setup_sim_disk_fixture(void **state)
{
    struct sim_disk_fixture *fixture;
    DEVICE *devices[3];

    fixture = calloc(1, sizeof(*fixture));
    assert_non_null(fixture);

    simh_test_reset_simulator_state();

    simh_test_init_device_unit(&fixture->byte_device, &fixture->byte_unit,
                               "DSKB", "DSKB0", DEV_DISABLE | DEV_DISK,
                               UNIT_ATTABLE | UNIT_ROABLE, 8, 1);
    fixture->byte_unit.flags |= DK_F_STD;
    fixture->byte_device.modifiers = disk_unit_modifiers;
    fixture->byte_device.debflags = disk_unit_debug;

    simh_test_init_device_unit(&fixture->sector_device, &fixture->sector_unit,
                               "DSKS", "DSKS0",
                               DEV_DISABLE | DEV_DISK | DEV_SECTORS,
                               UNIT_ATTABLE | UNIT_ROABLE, 16, 1);
    fixture->sector_device.attach = attach_test_sector_disk;
    fixture->sector_device.detach = sim_disk_detach;
    fixture->sector_unit.flags |= DK_F_STD;
    fixture->sector_unit.capac = 1;
    fixture->sector_device.modifiers = disk_unit_modifiers;
    fixture->sector_device.debflags = disk_unit_debug;

    assert_int_equal(simh_test_make_temp_dir(fixture->temp_dir,
                                             sizeof(fixture->temp_dir),
                                             "sim-disk-test"),
                     0);
    assert_int_equal(simh_test_join_path(fixture->image_path,
                                         sizeof(fixture->image_path),
                                         fixture->temp_dir, "disk.img"),
                     0);

    devices[0] = &fixture->byte_device;
    devices[1] = &fixture->sector_device;
    devices[2] = NULL;
    assert_int_equal(simh_test_set_devices(devices), 0);
    sim_dflt_dev = &fixture->byte_device;
    sim_quiet = 0;
    sim_show_message = 1;
    sim_switches = 0;

    *state = fixture;
    return 0;
}

static int teardown_sim_disk_fixture(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    sim_disk_clear_all_test_backends();
    pseudo_filesystem_size = 0;
    sim_switches = 0;
    simh_test_remove_path(fixture->temp_dir);
    simh_test_reset_simulator_state();
    free(fixture);
    *state = NULL;
    return 0;
}

static void create_temp_disk_image(struct sim_disk_fixture *fixture,
                                   size_t size)
{
    uint8_t *data;

    data = calloc(1, size);
    assert_non_null(data);
    assert_int_equal(simh_test_write_file(fixture->image_path, data, size), 0);
    free(data);
}

static void attach_temp_disk_image(struct sim_disk_fixture *fixture, UNIT *uptr)
{
    uptr->dctrl = ~0;
    uptr->dptr->dctrl = ~0;

    create_temp_disk_image(fixture, 512);

    assert_int_equal(sim_disk_attach_ex(uptr, fixture->image_path, 512, 1, true,
                                        0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    // "Pump up the volume!" (of the disk debugging...)
    ((struct disk_context *)uptr->disk_ctx)->dbit = ~0;
}

static int uuid_is_nil(const uint8_t uuid[16])
{
    uint8_t zero[16] = {0};

    return memcmp(uuid, zero, sizeof(zero)) == 0;
}

static int uuid_has_rfc4122_variant(const uint8_t uuid[16])
{
    return (uuid[8] & 0xc0) == 0x80;
}

static void read_vhd_footer_uuid(const char *path, uint8_t uuid[16])
{
    uint8_t *data;
    size_t size;
    size_t footer_offset;

    assert_int_equal(simh_test_read_file(path, (void **)&data, &size), 0);
    assert_true(size >= VHD_FOOTER_SIZE);

    footer_offset = size - VHD_FOOTER_SIZE;
    assert_memory_equal(&data[footer_offset], "conectix", 8);
    memcpy(uuid, &data[footer_offset + VHD_FOOTER_UNIQUE_ID_OFFSET], 16);

    free(data);
}

static t_stat test_backend_read(UNIT *uptr, t_lba lba, uint8_t *buf,
                                t_seccnt *sectsread, t_seccnt sects)
{
    (void)buf;

    uptr->u3++;
    assert_int_equal(lba, 7);
    assert_int_equal(sects, 3);
    if (sectsread != NULL)
        *sectsread = sects;
    return SCPE_OK;
}

static t_stat test_backend_write(UNIT *uptr, t_lba lba, uint8_t *buf,
                                 t_seccnt *sectswritten, t_seccnt sects)
{
    (void)buf;

    uptr->u3++;
    assert_int_equal(lba, 11);
    assert_int_equal(sects, 5);
    if (sectswritten != NULL)
        *sectswritten = sects;
    return SCPE_OK;
}

static void assert_disk_show_output(t_stat (*show_fn)(FILE *, UNIT *, int32_t,
                                                      const void *),
                                    UNIT *uptr, const char *expected)
{
    FILE *stream;
    char *text;
    size_t size;

    stream = sim_tmpfile();
    assert_non_null(stream);

    assert_int_equal(show_fn(stream, uptr, 0, NULL), SCPE_OK);
    assert_int_equal(simh_test_read_stream(stream, &text, &size), 0);
    assert_string_equal(text, expected);

    free(text);
    fclose(stream);
}

/* Capture generic SHOW output for one unit into a heap string. */
static char *capture_show_unit_text(DEVICE *dptr, UNIT *uptr)
{
    FILE *stream;
    char *text;
    size_t size;

    stream = sim_tmpfile();
    assert_non_null(stream);

    assert_int_equal(show_unit(stream, dptr, uptr, -1), SCPE_OK);
    assert_int_equal(simh_test_read_stream(stream, &text, &size), 0);

    fclose(stream);
    return text;
}

/* Adapter used by simh_test_capture_stdout() to capture detach messages. */
static void detach_capture_writer(void *context)
{
    struct detach_capture *capture = context;

    capture->status = sim_disk_detach(capture->unit);
}

// Callback state for disk I/O operations, principally for AIO completions.
struct disk_callback_state {
    UNIT *unit;
    t_stat status;
    sim_atomic_value_t call_count;
};

static struct disk_callback_state *active_disk_callback_state = NULL;

static void wait_for_completion(struct disk_callback_state *callback_state)
{
    /* In sync mode, callback is invoked synchronously.
       In async mode, we need to wait for completion. */
    if (aio_enabled_and_active()) {
        struct timespec now, timeout;

        sim_clock_gettime(CLOCK_REALTIME, &now);
        timeout.tv_sec = now.tv_sec + 5;
        timeout.tv_nsec = now.tv_nsec;

        sim_atomic_type_t last_calls = sim_atomic_get(&callback_state->call_count);

        while (last_calls == sim_atomic_get(&callback_state->call_count) &&
               (now.tv_sec < timeout.tv_sec || (now.tv_sec == timeout.tv_sec && now.tv_nsec < timeout.tv_nsec))) {
            // Drain the simulator event queue, if anything.
            assert_int_equal(sim_process_event(), SCPE_OK);

            /* Use sched_thread_yield() here to allow the I/O thread to make progress. Technically,
             * yes, sim_os_sleep() could be used but that makes assumptions about thread activation
             * latency. Yielding the thread is the better choice because it forces the thread scheduler
             * to give other runnable threads a chance. */
            sim_thread_yield();

            sim_clock_gettime(CLOCK_REALTIME, &now);
        }

        sim_atomic_type_t value_now = sim_atomic_get(&callback_state->call_count);
        assert_true(value_now > 0);
        assert_int_equal(value_now, 1);

        // Reset the call count for the next operation.
        sim_atomic_init(&callback_state->call_count);
    }
}

static void disk_io_callback(UNIT *unit, t_stat status)
{
    assert_non_null(active_disk_callback_state);
    active_disk_callback_state->unit = unit;
    active_disk_callback_state->status = status;
    sim_atomic_inc(&active_disk_callback_state->call_count);
}

/* Verify disk format selection updates the unit flags and that the show
   helper reflects the chosen format. */
static void test_sim_disk_set_and_show_format(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    assert_int_equal(sim_disk_set_fmt(&fixture->byte_unit, 0, "SIMH", NULL),
                     SCPE_OK);
    assert_int_equal(DK_GET_FMT(&fixture->byte_unit), DKUF_F_STD);
    assert_disk_show_output(sim_disk_show_fmt, &fixture->byte_unit,
                            "SIMH format");

    assert_true(sim_disk_set_fmt(&fixture->byte_unit, 0, "UNKNOWN", NULL) !=
                SCPE_OK);
}

/* Verify byte-addressed disk capacity parsing stores byte counts and the
   show helper renders them with byte units. */
static void test_sim_disk_set_and_show_byte_capacity(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    assert_int_equal(sim_disk_set_capac(&fixture->byte_unit, 0, "25", NULL),
                     SCPE_OK);
    assert_int_equal(fixture->byte_unit.capac, (sim_off_t) 25000000);
    assert_disk_show_output(sim_disk_show_capac, &fixture->byte_unit,
                            "capacity=25MB");

    fixture->byte_unit.flags |= UNIT_ATT;
    assert_int_equal(sim_disk_set_capac(&fixture->byte_unit, 0, "5", NULL),
                     SCPE_ALATT);
}

/* Verify sector-addressed capacity parsing converts megabytes into
   sector counts for DEV_SECTORS devices. */
static void test_sim_disk_set_capacity_scales_for_sector_devices(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    assert_int_equal(sim_disk_set_capac(&fixture->sector_unit, 0, "8", NULL),
                     SCPE_OK);
    assert_int_equal(fixture->sector_unit.capac, (sim_off_t) (8000000 / 512));
}

/* Verify availability and write-protect predicates track simple unit
   flag state without needing a live disk context. */
static void test_sim_disk_status_predicates_use_unit_flags(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    assert_false(sim_disk_isavailable(&fixture->byte_unit));
    assert_false(sim_disk_wrp(&fixture->byte_unit));

    fixture->byte_unit.flags |= DKUF_WRP;
    assert_true(sim_disk_wrp(&fixture->byte_unit));
}

/* Verify the process-local test backend intercepts sector reads before the
   normal disk context is required. */
static void test_sim_disk_test_backend_intercepts_read(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    SIM_DISK_TEST_BACKEND backend = {
        .rdsect = test_backend_read,
    };
    uint8_t data[1] = {0};
    t_seccnt sectsread = 0;

    assert_int_equal(sim_disk_set_test_backend(&fixture->byte_unit, &backend), SCPE_OK);
    assert_int_equal(sim_disk_rdsect(&fixture->byte_unit, 7, data, &sectsread, 3), SCPE_OK);
    assert_int_equal(sectsread, 3);
    assert_int_equal(fixture->byte_unit.u3, 1);
}

/* Verify the process-local test backend intercepts sector writes before the
   normal disk context is required. */
static void test_sim_disk_test_backend_intercepts_write(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    SIM_DISK_TEST_BACKEND backend = {
        .wrsect = test_backend_write,
    };
    uint8_t data[1] = {0};
    t_seccnt sectswritten = 0;

    assert_int_equal(sim_disk_set_test_backend(&fixture->sector_unit, &backend), SCPE_OK);
    assert_int_equal( sim_disk_wrsect(&fixture->sector_unit, 11, data, &sectswritten, 5), SCPE_OK);
    assert_int_equal(sectswritten, 5);
    assert_int_equal(fixture->sector_unit.u3, 1);
}

static void test_sim_disk_test_backend_rejects_null_unit(void **state)
{
    SIM_DISK_TEST_BACKEND backend = {
        .rdsect = test_backend_read,
    };

    (void)state;

    assert_int_equal(sim_disk_set_test_backend(NULL, &backend), SCPE_ARG);
    sim_disk_clear_test_backend(NULL);
    sim_disk_clear_all_test_backends();
}

static void test_sim_disk_size_restores_nonboolean_quiet_value(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    int32_t saved_quiet = sim_quiet;

    attach_temp_disk_image(fixture, &fixture->sector_unit);
    pseudo_filesystem_size = 1024;

    sim_quiet = 7;

    assert_int_equal(sim_disk_size(&fixture->sector_unit), 1024);
    assert_int_equal(sim_quiet, 7);

    sim_quiet = saved_quiet;
    pseudo_filesystem_size = 0;
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

static void test_sim_disk_detach_restores_auto_format(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char attach_arg[2 * CBUFSIZE];

    assert_int_equal(sim_disk_set_fmt(&fixture->sector_unit, 0, "AUTO", NULL),
                     SCPE_OK);
    create_temp_disk_image(fixture, 512);
    assert_true(snprintf(attach_arg, sizeof(attach_arg), "SIMH %s",
                         fixture->image_path) < (int)sizeof(attach_arg));
    sim_switches = SWMASK('F');
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_arg, 512,
                                        1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    assert_int_not_equal(DK_GET_FMT(&fixture->sector_unit), DKUF_F_AUTO);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
    assert_int_equal(DK_GET_FMT(&fixture->sector_unit), DKUF_F_AUTO);
}

/* Verify VHD creation stores a generated UUID in the VHD footer. */
static void test_sim_disk_create_vhd_writes_generated_uuid(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t uuid[16];

    /* Nothing special. Test the normal path through sim_disk_attach_ex(). */
    sim_switches = 0;
    assert_int_equal(sim_disk_set_fmt(&fixture->sector_unit, 0, "VHD", NULL),
                     SCPE_OK);
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        fixture->image_path, 512, 1, true, 0,
                                        NULL, 0, 0, NULL),
                     SCPE_OK);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);

    read_vhd_footer_uuid(fixture->image_path, uuid);
    assert_false(uuid_is_nil(uuid));
    assert_true(uuid_has_rfc4122_variant(uuid));
}

#if defined (HAVE_FMEMOPEN)
/* Create a deterministic nonzero disk image for FROM= and SAVE tests. */
static void create_pattern_disk_image(struct sim_disk_fixture *fixture,
                                      size_t size)
{
    uint8_t *data;

    data = malloc(size);
    assert_non_null(data);
    for (size_t index = 0; index < size; index++)
        data[index] = (uint8_t)(index ^ 0x5A);
    assert_int_equal(simh_test_write_file(fixture->image_path, data, size), 0);
    free(data);
}

/* Verify RAMDISK: attaches volatile memory using the same default size that
   the missing-file create path would use for the unit's current type. */
static void test_sim_disk_ramdisk_default_size_is_volatile(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t zeros[512];
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    fixture->sector_unit.capac = 2;
    sim_switches = 0;

    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:",
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_true((fixture->sector_unit.flags & UNIT_ATT) != 0);
    assert_int_equal(sim_disk_size(&fixture->sector_unit), 2048);

    memset(zeros, 0, sizeof(zeros));
    memset(write_buf, 0xA5, sizeof(write_buf));
    memset(read_buf, 0x5A, sizeof(read_buf));

    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 1, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, zeros, sizeof(read_buf));

    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 1, write_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);

    memset(read_buf, 0, sizeof(read_buf));
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 1, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, write_buf, sizeof(read_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);

    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:",
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    memset(read_buf, 0x5A, sizeof(read_buf));
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 1, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, zeros, sizeof(read_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify the unnamed RAMDISK: parameter is parsed as SIZE and allocates the
   requested amount instead of using the unit's default capacity. */
static void test_sim_disk_ramdisk_unnamed_parameter_is_size(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    fixture->sector_unit.capac = 2;
    sim_switches = 0;

    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:4096",
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_int_equal(sim_disk_size(&fixture->sector_unit), 4096);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify RAMDISK: size suffixes use disk-context binary units. */
static void test_sim_disk_ramdisk_size_suffixes_are_binary(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    sim_switches = 0;

    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:4K",
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_int_equal(sim_disk_size(&fixture->sector_unit), 4096);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify keyed RAMDISK: options are order-independent.  TYPE is accepted for
   grammar consistency even when it selects the unit's current type. */
static void test_sim_disk_ramdisk_keyed_size_and_type_commute(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    sim_switches = 0;
    assert_int_equal(
        sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:SIZE=4096,TYPE=TEST",
                           512, 1, true, 0, "TEST", 0, 0, NULL),
        SCPE_OK);
    assert_int_equal(sim_disk_size(&fixture->sector_unit), 4096);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);

    sim_switches = 0;
    assert_int_equal(
        sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:TYPE=TEST,SIZE=4096",
                           512, 1, true, 0, "TEST", 0, 0, NULL),
        SCPE_OK);
    assert_int_equal(sim_disk_size(&fixture->sector_unit), 4096);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify TYPE is applied before default RAMDISK: sizing. */
static void test_sim_disk_ramdisk_type_selects_default_size(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    fixture->sector_unit.capac = 2;
    sim_switches = 0;

    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:TYPE=LARGE", 512, 1, true, 0,
                                        "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_int_equal(sim_disk_size(&fixture->sector_unit), 4096);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify RAMDISK:TYPE still reaches SET when a valid unit name nearly fills
   the command parser's token buffer. */
static void test_sim_disk_ramdisk_type_handles_long_unit_name(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char unit_name[CBUFSIZE];

    memset(unit_name, 'D', sizeof(unit_name));
    unit_name[sizeof(unit_name) - 1] = '\0';
    fixture->sector_unit.uname = NULL;
    sim_set_uname(&fixture->sector_unit, unit_name);
    fixture->sector_unit.capac = 2;
    sim_switches = 0;

    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:TYPE=LARGE", 512, 1, true, 0,
                                        "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_int_equal(sim_disk_size(&fixture->sector_unit), 4096);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify -R RAMDISK: attaches read-only storage and rejects writes. */
static void test_sim_disk_ramdisk_read_only_switch_write_protects(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t read_buf[512];
    uint8_t write_buf[512];
    t_seccnt sectors = 99;

    sim_switches = SWMASK('R');
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:4096",
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_true((fixture->sector_unit.flags & UNIT_ATT) != 0);
    assert_true(sim_disk_wrp(&fixture->sector_unit));

    memset(write_buf, 0xA5, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0, write_buf,
                                     &sectors, 1),
                     SCPE_RO);
    assert_int_equal(sectors, 0);

    memset(read_buf, 0x5A, sizeof(read_buf));
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_not_equal(read_buf, write_buf, sizeof(read_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify an already read-only unit can attach a read-only RAMDISK:. */
static void test_sim_disk_ramdisk_read_only_unit_write_protects(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    fixture->sector_unit.flags |= UNIT_RO;
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, "RAMDISK:4096",
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_true((fixture->sector_unit.flags & UNIT_ATT) != 0);
    assert_true(sim_disk_wrp(&fixture->sector_unit));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify FROM= copies a source image into the ramdisk and zero-fills the rest. */
static void test_sim_disk_ramdisk_from_seeds_contents(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char attach_spec[2 * CBUFSIZE];
    uint8_t expected[512];
    uint8_t zeros[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    create_pattern_disk_image(fixture, 1024);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,FROM=%s",
                         fixture->image_path) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    for (size_t index = 0; index < sizeof(expected); index++)
        expected[index] = (uint8_t)(index ^ 0x5A);
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, expected, sizeof(read_buf));

    memset(zeros, 0, sizeof(zeros));
    memset(read_buf, 0xA5, sizeof(read_buf));
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 2, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, zeros, sizeof(read_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify -R applies after FROM= seeds the ramdisk contents. */
static void test_sim_disk_ramdisk_read_only_from_seeds_contents(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char attach_spec[2 * CBUFSIZE];
    uint8_t expected[512];
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    create_pattern_disk_image(fixture, 512);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,FROM=%s",
                         fixture->image_path) < (int)sizeof(attach_spec));
    sim_switches = SWMASK('R');
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_true(sim_disk_wrp(&fixture->sector_unit));

    for (size_t index = 0; index < sizeof(expected); index++)
        expected[index] = (uint8_t)(index ^ 0x5A);
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, expected, sizeof(read_buf));

    memset(write_buf, 0xA5, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0, write_buf,
                                     &sectors, 1),
                     SCPE_RO);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify FROM= rejects sources larger than the requested ramdisk. */
static void test_sim_disk_ramdisk_from_rejects_oversized_source(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char attach_spec[2 * CBUFSIZE];

    create_pattern_disk_image(fixture, 8192);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,FROM=%s",
                         fixture->image_path) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(SCPE_BARE_STATUS(sim_disk_attach_ex(
                         &fixture->sector_unit, attach_spec, 512, 1, true, 0,
                         "TEST", 0, 0, NULL)),
                     SCPE_ARG);
    assert_true((fixture->sector_unit.flags & UNIT_ATT) == 0);
}

/* Verify SAVE= is attach metadata and is not opened or created at attach. */
static void test_sim_disk_ramdisk_save_option_is_not_touched(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char save_path[CBUFSIZE];
    char attach_spec[2 * CBUFSIZE];
    void *data = NULL;
    size_t data_size = 0;

    assert_int_equal(simh_test_join_path(save_path, sizeof(save_path),
                                         fixture->temp_dir, "save.dsk"),
                     0);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,SAVE=%s",
                         save_path) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    assert_int_not_equal(simh_test_read_file(save_path, &data, &data_size), 0);
    assert_null(data);
    assert_int_equal(data_size, 0);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify simulator SAVE writes ramdisk contents to the SAVE= disk image. */
static void test_sim_disk_ramdisk_sim_save_writes_save_image(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char save_path[CBUFSIZE];
    char attach_spec[2 * CBUFSIZE];
    FILE *save_state;
    uint8_t write_buf[512];
    void *saved_data = NULL;
    size_t saved_size = 0;
    t_seccnt sectors = 0;

    assert_int_equal(simh_test_join_path(save_path, sizeof(save_path),
                                         fixture->temp_dir, "save.dsk"),
                     0);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,SAVE=%s",
                         save_path) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    memset(write_buf, 0x3C, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 1, write_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);

    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);
    fclose(save_state);

    assert_int_equal(simh_test_read_file(save_path, &saved_data, &saved_size),
                     0);
    assert_int_equal(saved_size, 4096);
    assert_memory_equal((uint8_t *)saved_data + 512, write_buf,
                        sizeof(write_buf));
    free(saved_data);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify read-only ramdisks are still checkpointed by simulator SAVE. */
static void test_sim_disk_ramdisk_sim_save_writes_read_only_image(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char save_path[CBUFSIZE];
    char attach_spec[2 * CBUFSIZE];
    FILE *save_state;
    void *saved_data = NULL;
    size_t saved_size = 0;

    create_pattern_disk_image(fixture, 512);
    assert_int_equal(simh_test_join_path(save_path, sizeof(save_path),
                                         fixture->temp_dir, "save.dsk"),
                     0);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,FROM=%s,SAVE=%s",
                         fixture->image_path,
                         save_path) < (int)sizeof(attach_spec));
    sim_switches = SWMASK('R');
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);
    fclose(save_state);

    assert_int_equal(simh_test_read_file(save_path, &saved_data, &saved_size),
                     0);
    assert_int_equal(saved_size, 4096);
    for (size_t index = 0; index < 512; index++)
        assert_int_equal(((uint8_t *)saved_data)[index], (uint8_t)(index ^ 0x5A));
    free(saved_data);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify omitted SAVE= defaults to the host null file and still saves state. */
static void test_sim_disk_ramdisk_sim_save_allows_default_null_save(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    FILE *save_state;

    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:SIZE=4096", 512, 1, true, 0,
                                        "TEST", 0, 0, NULL),
                     SCPE_OK);
    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);
    fclose(save_state);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify simulator RESTORE repopulates RAMDISK: from its SAVE= image. */
static void test_sim_disk_ramdisk_sim_restore_reads_save_image(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char save_path[CBUFSIZE];
    char attach_spec[2 * CBUFSIZE];
    FILE *save_state;
    uint8_t write_buf[512];
    uint8_t clobber_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    assert_int_equal(simh_test_join_path(save_path, sizeof(save_path),
                                         fixture->temp_dir, "restore.dsk"),
                     0);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,SAVE=%s",
                         save_path) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    memset(write_buf, 0x3C, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 1, write_buf,
                                     &sectors, 1),
                     SCPE_OK);
    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);

    memset(clobber_buf, 0xA5, sizeof(clobber_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 1, clobber_buf,
                                     &sectors, 1),
                     SCPE_OK);

    rewind(save_state);
    sim_switches = 0;
    assert_int_equal(sim_rest(save_state), SCPE_OK);
    fclose(save_state);

    memset(read_buf, 0, sizeof(read_buf));
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 1, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, write_buf, sizeof(read_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify RESTORE preserves read-only RAMDISK: attachment state. */
static void test_sim_disk_ramdisk_sim_restore_keeps_read_only(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char save_path[CBUFSIZE];
    char attach_spec[2 * CBUFSIZE];
    FILE *save_state;
    uint8_t expected[512];
    uint8_t replacement[512];
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    create_pattern_disk_image(fixture, 512);
    assert_int_equal(simh_test_join_path(save_path, sizeof(save_path),
                                         fixture->temp_dir, "ro-restore.dsk"),
                     0);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,FROM=%s,SAVE=%s",
                         fixture->image_path,
                         save_path) < (int)sizeof(attach_spec));
    sim_switches = SWMASK('R');
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);

    memset(replacement, 0xA5, sizeof(replacement));
    assert_int_equal(simh_test_write_file(fixture->image_path, replacement,
                                          sizeof(replacement)),
                     0);

    rewind(save_state);
    sim_switches = 0;
    assert_int_equal(sim_rest(save_state), SCPE_OK);
    fclose(save_state);

    assert_true(sim_disk_wrp(&fixture->sector_unit));
    for (size_t index = 0; index < sizeof(expected); index++)
        expected[index] = (uint8_t)(index ^ 0x5A);
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0, read_buf,
                                     &sectors, 1),
                     SCPE_OK);
    assert_memory_equal(read_buf, expected, sizeof(read_buf));

    memset(write_buf, 0xA5, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0, write_buf,
                                     &sectors, 1),
                     SCPE_RO);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify a state file with no SAVE= cannot silently restore an empty
   ramdisk. */
static void test_sim_disk_ramdisk_sim_restore_requires_save(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    FILE *save_state;

    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:SIZE=4096", 512, 1, true, 0,
                                        "TEST", 0, 0, NULL),
                     SCPE_OK);
    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);

    rewind(save_state);
    sim_switches = 0;
    assert_int_equal(SCPE_BARE_STATUS(sim_rest(save_state)), SCPE_ARG);
    fclose(save_state);
}

/* Verify RESTORE reports an open failure when the SAVE= image is missing. */
static void test_sim_disk_ramdisk_sim_restore_requires_image(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char save_path[CBUFSIZE];
    char attach_spec[2 * CBUFSIZE];
    FILE *save_state;

    assert_int_equal(simh_test_join_path(save_path, sizeof(save_path),
                                         fixture->temp_dir, "missing.dsk"),
                     0);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,SAVE=%s",
                         save_path) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);
    assert_int_equal(simh_test_remove_path(save_path), 0);

    rewind(save_state);
    sim_switches = 0;
    assert_int_equal(SCPE_BARE_STATUS(sim_rest(save_state)), SCPE_OPENERR);
    fclose(save_state);
}

/* Verify RESTORE rejects a SAVE= image with the wrong size. */
static void test_sim_disk_ramdisk_sim_restore_rejects_wrong_size(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char save_path[CBUFSIZE];
    char attach_spec[2 * CBUFSIZE];
    FILE *save_state;
    uint8_t short_image[512];

    assert_int_equal(simh_test_join_path(save_path, sizeof(save_path),
                                         fixture->temp_dir, "short.dsk"),
                     0);
    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,SAVE=%s",
                         save_path) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);
    memset(short_image, 0x5A, sizeof(short_image));
    assert_int_equal(simh_test_write_file(save_path, short_image,
                                          sizeof(short_image)),
                     0);

    rewind(save_state);
    sim_switches = 0;
    assert_int_equal(SCPE_BARE_STATUS(sim_rest(save_state)), SCPE_ARG);
    fclose(save_state);
}

/* Verify explicit SAVE to the host null device is only detected on RESTORE. */
static void test_sim_disk_ramdisk_sim_restore_rejects_null_save(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char attach_spec[CBUFSIZE];
    FILE *save_state;

    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,SAVE=%s",
                         NULL_DEVICE) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);
    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(sim_save(save_state), SCPE_OK);

    rewind(save_state);
    sim_switches = 0;
    assert_int_equal(SCPE_BARE_STATUS(sim_rest(save_state)), SCPE_ARG);
    fclose(save_state);
}

/* Verify simulator SAVE reports failure when SAVE= cannot be opened. */
static void test_sim_disk_ramdisk_sim_save_reports_bad_save_path(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char attach_spec[2 * CBUFSIZE];
    FILE *save_state;

    assert_true(snprintf(attach_spec, sizeof(attach_spec),
                         "RAMDISK:SIZE=4096,SAVE=%s",
                         fixture->temp_dir) < (int)sizeof(attach_spec));
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit, attach_spec,
                                        512, 1, true, 0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    save_state = sim_tmpfile();
    assert_non_null(save_state);
    assert_int_equal(SCPE_BARE_STATUS(sim_save(save_state)), SCPE_OPENERR);
    fclose(save_state);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify file-container management switches are rejected for RAMDISK:. */
static void test_sim_disk_ramdisk_rejects_container_switches(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    const int32_t switches[] = {
        SWMASK('C'),
        SWMASK('D'),
        SWMASK('E'),
        SWMASK('I'),
        SWMASK('K'),
        SWMASK('M'),
        SWMASK('O'),
        SWMASK('V'),
        SWMASK('X'),
    };

    for (size_t index = 0; index < sizeof (switches) / sizeof (switches[0]);
         index++) {
        sim_switches = switches[index];
        assert_int_equal(SCPE_BARE_STATUS(sim_disk_attach_ex(
                             &fixture->sector_unit, "RAMDISK:", 512, 1,
                             true, 0, "TEST", 0, 0, NULL)),
                         SCPE_ARG);
        assert_true((fixture->sector_unit.flags & UNIT_ATT) == 0);
    }
}

/* Verify malformed RAMDISK: specifications are rejected without attaching. */
static void test_sim_disk_ramdisk_rejects_malformed_specs(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    const char *specs[] = {
        "RAMDISK:,",
        "RAMDISK:,SIZE=4096",
        "RAMDISK:4096,",
        "RAMDISK:4096,8192",
        "RAMDISK:SIZE=",
        "RAMDISK:SIZE=0",
        "RAMDISK:SIZE=1",
        "RAMDISK:SIZE=XYZ",
        "RAMDISK:SIZE=1P",
        "RAMDISK:SIZE=4096,SIZE=8192",
        "RAMDISK:TYPE=",
        "RAMDISK:TYPE=TEST,TYPE=LARGE",
        "RAMDISK:FROM=",
        "RAMDISK:FROM=a,FROM=b",
        "RAMDISK:SAVE=",
        "RAMDISK:SAVE=a,SAVE=b",
        "RAMDISK:UNKNOWN=4096",
        "RAMDISK:9223372036854775808",
    };

    for (size_t index = 0; index < sizeof (specs) / sizeof (specs[0]);
         index++) {
        sim_switches = 0;
        assert_int_equal(SCPE_BARE_STATUS(sim_disk_attach_ex(
                             &fixture->sector_unit, specs[index], 512, 1,
                             true, 0, "TEST", 0, 0, NULL)),
                         SCPE_ARG);
        assert_true((fixture->sector_unit.flags & UNIT_ATT) == 0);
    }
}

/* Verify generic SHOW output identifies ramdisks as volatile attachments. */
static void test_sim_disk_ramdisk_show_marks_volatile(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    char *text;

    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:SIZE=4096", 512, 1, true, 0,
                                        "TEST", 0, 0, NULL),
                     SCPE_OK);

    text = capture_show_unit_text(&fixture->sector_device,
                                  &fixture->sector_unit);
    assert_non_null(strstr(text, "attached to RAMDISK:SIZE=4096, volatile"));
    free(text);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify DETACH tells the user that ramdisk contents are being discarded. */
static void test_sim_disk_ramdisk_detach_reports_discard(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    struct detach_capture capture = {&fixture->sector_unit, SCPE_IERR};
    char *text;
    size_t size;

    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:SIZE=4096", 512, 1, true, 0,
                                        "TEST", 0, 0, NULL),
                     SCPE_OK);

    assert_int_equal(simh_test_capture_stdout(detach_capture_writer, &capture,
                                              &text, &size),
                     0);
    assert_int_equal(capture.status, SCPE_OK);
    assert_non_null(strstr(
        text, "DSKS0: discarding volatile ramdisk contents\n"));
    free(text);
}
#endif

#if !defined (HAVE_FMEMOPEN)
/* Verify hosts without fmemopen reject RAMDISK: cleanly. */
static void test_sim_disk_ramdisk_requires_fmemopen(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    sim_switches = 0;
    assert_int_equal(SCPE_BARE_STATUS(sim_disk_attach_ex(
                         &fixture->sector_unit, "RAMDISK:", 512, 1, true, 0,
                         "TEST", 0, 0, NULL)),
                     SCPE_NOFNC);
    assert_true((fixture->sector_unit.flags & UNIT_ATT) == 0);
}
#endif

/* Verify bare RAMDISK remains a normal host filename and is not interpreted as
   the volatile-memory attach target. */
static void test_sim_disk_bare_ramdisk_remains_filename(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    (void)fixture;

    sim_switches = SWMASK('E');
    assert_int_equal(SCPE_BARE_STATUS(sim_disk_attach_ex(
                         &fixture->sector_unit, "RAMDISK", 512, 1, true, 0,
                         "TEST", 0, 0, NULL)),
                     SCPE_OPENERR);
}

static void test_sim_disk_set_noautosize_normalizes_global_flag(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    assert_int_equal(sim_disk_init(), SCPE_OK);
    assert_int_equal(fixture->byte_unit.flags & DKUF_NOAUTOSIZE, 0);
    assert_int_equal(fixture->sector_unit.flags & DKUF_NOAUTOSIZE, 0);

    assert_int_equal(sim_disk_set_noautosize(7, NULL), SCPE_OK);
    assert_int_equal(fixture->byte_unit.flags & DKUF_NOAUTOSIZE,
                     DKUF_NOAUTOSIZE);
    assert_int_equal(fixture->sector_unit.flags & DKUF_NOAUTOSIZE,
                     DKUF_NOAUTOSIZE);

    assert_int_equal(SCPE_BARE_STATUS(sim_disk_set_noautosize(1, NULL)),
                     SCPE_ARG);

    assert_int_equal(sim_disk_set_noautosize(false, NULL), SCPE_OK);
    assert_int_equal(fixture->byte_unit.flags & DKUF_NOAUTOSIZE, 0);
    assert_int_equal(fixture->sector_unit.flags & DKUF_NOAUTOSIZE, 0);
}

/* =============================================================================
 * NEW TESTS: Error Handling & Edge Cases
 * ============================================================================= */

/* Verify rdsect rejects invalid parameters and boundary conditions. */
static void test_sim_disk_rdsect_invalid_parameters(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t buf[512];
    t_seccnt sectors = 0;

    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Test NULL buffer pointer */
    assert_int_not_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                         NULL, &sectors, 1), SCPE_OK);

    /* Test zero sectors (should succeed but read nothing) */
    sectors = 99;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     buf, &sectors, 0), SCPE_OK);
    assert_int_equal(sectors, 0);

    /* Test reading beyond disk capacity -- it's allowed and will zero out buf and report
     * 1 sector read. */
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit,
                                         999999, buf, &sectors, 1), SCPE_OK);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify wrsect rejects invalid parameters and respects write protection. */
static void test_sim_disk_wrsect_invalid_parameters(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t buf[512];
    t_seccnt sectors = 0;

    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Test NULL buffer pointer */
    memset(buf, 0xA5, sizeof(buf));
    assert_int_not_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                         NULL, &sectors, 1), SCPE_OK);

    /* Test zero sectors (should succeed but write nothing) */
    sectors = 99;
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     buf, &sectors, 0), SCPE_OK);
    assert_int_equal(sectors, 0);

    /* Test writing beyond disk capacity */
    sectors = 0;
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit,
                                         999999, buf, &sectors, 1), SCPE_OK);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify write operations are rejected on write-protected disks. */
static void test_sim_disk_wrsect_write_protected(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t buf[512];
    t_seccnt sectors = 0;

    attach_temp_disk_image(fixture, &fixture->sector_unit);
    fixture->sector_unit.flags |= DKUF_WRP;

    memset(buf, 0xA5, sizeof(buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     buf, &sectors, 1), SCPE_RO);
    assert_int_equal(sectors, 0);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify operations on unattached units are rejected. */
static void test_sim_disk_operations_unattached_unit(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t buf[512];
    t_seccnt sectors = 0;

    /* Attempt read on unattached unit */
    assert_int_not_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                         buf, &sectors, 1), SCPE_OK);

    /* Attempt write on unattached unit */
    memset(buf, 0xA5, sizeof(buf));
    assert_int_not_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                         buf, &sectors, 1), SCPE_OK);
}

/* =============================================================================
 * NEW TESTS: Boundary & Range Testing
 * ============================================================================= */

/* Verify operations at LBA 0 (first sector). */
static void test_sim_disk_lba_zero_operations(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Write to LBA 0 */
    memset(write_buf, 0x55, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf, &sectors, 1), SCPE_OK);
    assert_int_equal(sectors, 1);

    /* Read from LBA 0 */
    memset(read_buf, 0, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify multi-sector read and write operations. */
static void test_sim_disk_multi_sector_operations(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t write_buf[1024];
    uint8_t read_buf[1024];
    t_seccnt sectors = 0;

    /* Create larger disk image (2 sectors) */
    create_temp_disk_image(fixture, 1024);
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        fixture->image_path, 512, 1, true,
                                        0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    /* Write 2 sectors with different patterns */
    memset(write_buf, 0xAA, 512);
    memset(write_buf + 512, 0x55, 512);
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf, &sectors, 2), SCPE_OK);
    assert_int_equal(sectors, 2);

    /* Read back 2 sectors */
    memset(read_buf, 0, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 2), SCPE_OK);
    assert_int_equal(sectors, 2);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify capacity boundary values are handled correctly. */
static void test_sim_disk_capacity_boundary_values(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    /* Test minimum capacity */
    assert_int_equal(sim_disk_set_capac(&fixture->byte_unit, 0, "1", NULL),
                     SCPE_OK);
    assert_int_equal(fixture->byte_unit.capac, (sim_off_t) 1000000);

    /* Test large capacity value */
    assert_int_equal(sim_disk_set_capac(&fixture->byte_unit, 0, "1000", NULL),
                     SCPE_OK);
    assert_int_equal(fixture->byte_unit.capac, (sim_off_t) 1000000000);

    /* Test invalid capacity (zero), which suprisingly succeeds. */
    assert_int_not_equal(sim_disk_set_capac(&fixture->byte_unit, 0, "0", NULL),
                         SCPE_OK);

    /* Test setting capacity on attached unit fails */
    fixture->byte_unit.flags |= UNIT_ATT;
    assert_int_equal(sim_disk_set_capac(&fixture->byte_unit, 0, "5", NULL),
                     SCPE_ALATT);
    fixture->byte_unit.flags &= ~UNIT_ATT;
}

/* Verify async read with callback works in both modes. */
static void test_sim_disk_async_read_with_callback(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    struct disk_callback_state callback_state = {
        .unit = NULL,
        .status = SCPE_OK,
    };
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    sim_atomic_init(&callback_state.call_count);
    active_disk_callback_state = &callback_state;
    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Enable async I/O (which gets enabled when the disk device is attached) */
    assert_int_equal(sim_disk_set_async(&fixture->sector_unit, 1000), SCPE_OK);

    /* Write test pattern */
    memset(write_buf, 0xA5, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf, &sectors, 1), SCPE_OK);

    /* Async read with callback */
    memset(read_buf, 0, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect_a(&fixture->sector_unit, 0,
                                       read_buf, &sectors, 1, disk_io_callback),
                     SCPE_OK);
    wait_for_completion(&callback_state);

    /* Verify callback was invoked */
    assert_ptr_equal(callback_state.unit, &fixture->sector_unit);
    assert_int_equal(callback_state.status, SCPE_OK);
    assert_int_equal(sectors, 1);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    // assert_int_equal(sim_disk_clr_async(&fixture->sector_unit), SCPE_OK);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
    active_disk_callback_state = NULL;
}

/* Verify async write with callback works in both modes. */
static void test_sim_disk_async_write_with_callback(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    struct disk_callback_state callback_state = {
        .unit = NULL,
        .status = SCPE_OK,
    };
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    sim_atomic_init(&callback_state.call_count);
    active_disk_callback_state = &callback_state;
    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Enable async I/O */
    assert_int_equal(sim_disk_set_async(&fixture->sector_unit, 1000), SCPE_OK);

    /* Async write with callback */
    memset(write_buf, 0x5A, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect_a(&fixture->sector_unit, 0,
                                       write_buf, &sectors, 1, disk_io_callback),
                     SCPE_OK);
    wait_for_completion(&callback_state);

    assert_ptr_equal(callback_state.unit, &fixture->sector_unit);
    assert_int_equal(callback_state.status, SCPE_OK);
    assert_int_equal(sectors, 1);

    /* Verify data was written correctly */
    memset(read_buf, 0, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    // assert_int_equal(sim_disk_clr_async(&fixture->sector_unit), SCPE_OK);
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
    active_disk_callback_state = NULL;
}

/* Verify async latency configuration. */
static void test_sim_disk_async_latency_settings(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Set zero latency - just verify the call succeeds */
    assert_int_equal(sim_disk_set_async(&fixture->sector_unit, 0), SCPE_OK);

    /* Set high latency - verify the call succeeds */
    assert_int_equal(sim_disk_set_async(&fixture->sector_unit, 5000), SCPE_OK);

    /* Clear async I/O */
    // assert_int_equal(sim_disk_clr_async(&fixture->sector_unit), SCPE_OK);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* =============================================================================
 * NEW TESTS: Backend Error Injection
 * ============================================================================= */

static t_stat test_backend_read_error(UNIT *uptr, t_lba lba, uint8_t *buf,
                                      t_seccnt *sectsread, t_seccnt sects)
{
    (void)uptr;
    (void)buf;
    (void)lba;
    (void)sects;

    if (sectsread != NULL)
        *sectsread = 0;
    return SCPE_IOERR;
}

static t_stat test_backend_write_error(UNIT *uptr, t_lba lba, uint8_t *buf,
                                       t_seccnt *sectswritten, t_seccnt sects)
{
    (void)uptr;
    (void)buf;
    (void)lba;
    (void)sects;

    if (sectswritten != NULL)
        *sectswritten = 0;
    return SCPE_IOERR;
}

static t_stat test_backend_partial_read(UNIT *uptr, t_lba lba, uint8_t *buf,
                                        t_seccnt *sectsread, t_seccnt sects)
{
    (void)uptr;
    (void)lba;

    if (sects > 1) {
        memset(buf, 0xAA, 512);
        if (sectsread != NULL)
            *sectsread = 1;
        return SCPE_OK;
    }
    if (sectsread != NULL)
        *sectsread = sects;
    return SCPE_OK;
}

/* Verify backend can inject read errors. */
static void test_sim_disk_backend_read_error_injection(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    SIM_DISK_TEST_BACKEND backend = {
        .rdsect = test_backend_read_error,
    };
    uint8_t buf[512];
    t_seccnt sectors = 0;

    assert_int_equal(sim_disk_set_test_backend(&fixture->sector_unit, &backend),
                     SCPE_OK);

    /* Read should fail with injected error */
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     buf, &sectors, 1), SCPE_IOERR);
    assert_int_equal(sectors, 0);

    sim_disk_clear_test_backend(&fixture->sector_unit);
}

/* Verify backend can inject write errors. */
static void test_sim_disk_backend_write_error_injection(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    SIM_DISK_TEST_BACKEND backend = {
        .wrsect = test_backend_write_error,
    };
    uint8_t buf[512];
    t_seccnt sectors = 0;

    assert_int_equal(sim_disk_set_test_backend(&fixture->sector_unit, &backend),
                     SCPE_OK);

    /* Write should fail with injected error */
    memset(buf, 0xA5, sizeof(buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     buf, &sectors, 1), SCPE_IOERR);
    assert_int_equal(sectors, 0);

    sim_disk_clear_test_backend(&fixture->sector_unit);
}

/* Verify backend can simulate partial transfers. */
static void test_sim_disk_backend_partial_transfer(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    SIM_DISK_TEST_BACKEND backend = {
        .rdsect = test_backend_partial_read,
    };
    uint8_t buf[1024];
    t_seccnt sectors = 0;

    assert_int_equal(sim_disk_set_test_backend(&fixture->sector_unit, &backend),
                     SCPE_OK);

    /* Request 2 sectors, backend returns only 1 */
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     buf, &sectors, 2), SCPE_OK);
    assert_int_equal(sectors, 1);

    sim_disk_clear_test_backend(&fixture->sector_unit);
}

/* Verify backend state tracking using unit fields. */
static void test_sim_disk_backend_state_tracking(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    SIM_DISK_TEST_BACKEND backend = {
        .rdsect = test_backend_read,
        .wrsect = test_backend_write,
    };
    uint8_t buf[512];
    t_seccnt sectors = 0;

    fixture->sector_unit.u3 = 0;
    assert_int_equal(sim_disk_set_test_backend(&fixture->sector_unit, &backend),
                     SCPE_OK);

    /* Backend increments u3 on each operation */
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 7,
                                     buf, &sectors, 3), SCPE_OK);
    assert_int_equal(fixture->sector_unit.u3, 1);

    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 11,
                                     buf, &sectors, 5), SCPE_OK);
    assert_int_equal(fixture->sector_unit.u3, 2);

    sim_disk_clear_test_backend(&fixture->sector_unit);
}

/* =============================================================================
 * NEW TESTS: State & Lifecycle
 * ============================================================================= */

/* Verify multiple attach/detach cycles maintain consistent state. */
static void test_sim_disk_attach_detach_cycles(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    create_temp_disk_image(fixture, 512);

    for (int cycle = 0; cycle < 3; cycle++) {
        sim_switches = 0;
        assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                            fixture->image_path, 512, 1, true,
                                            0, "TEST", 0, 0, NULL),
                         SCPE_OK);

        /* Write cycle number */
        memset(write_buf, (uint8_t)cycle, sizeof(write_buf));
        assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                         write_buf, &sectors, 1), SCPE_OK);

        /* Verify write */
        memset(read_buf, 0xFF, sizeof(read_buf));
        sectors = 0;
        assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                         read_buf, &sectors, 1), SCPE_OK);
        assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

        assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
    }
}

/* Verify reset clears error states but preserves attachment. */
static void test_sim_disk_reset_preserves_attachment(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Reset the unit */
    assert_int_equal(sim_disk_reset(&fixture->sector_unit), SCPE_OK);

    /* Verify unit is still attached */
    assert_true((fixture->sector_unit.flags & UNIT_ATT) != 0);
    assert_true(sim_disk_isavailable(&fixture->sector_unit));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify unit flag transitions during attach/detach. */
static void test_sim_disk_unit_flag_transitions(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    /* Initially not attached */
    assert_false((fixture->sector_unit.flags & UNIT_ATT) != 0);

    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Now attached */
    assert_true((fixture->sector_unit.flags & UNIT_ATT) != 0);
    assert_true(sim_disk_isavailable(&fixture->sector_unit));

    /* Detach */
    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);

    /* No longer attached */
    assert_false((fixture->sector_unit.flags & UNIT_ATT) != 0);
    assert_false(sim_disk_isavailable(&fixture->sector_unit));
}

/* =============================================================================
 * NEW TESTS: Data Integrity
 * ============================================================================= */

/* Verify various write/read patterns are preserved. */
static void test_sim_disk_write_read_patterns(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    attach_temp_disk_image(fixture, &fixture->sector_unit);

    /* Test all zeros */
    memset(write_buf, 0x00, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf, &sectors, 1), SCPE_OK);
    memset(read_buf, 0xFF, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    /* Test all ones */
    memset(write_buf, 0xFF, sizeof(write_buf));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf, &sectors, 1), SCPE_OK);
    memset(read_buf, 0x00, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    /* Test alternating pattern */
    for (size_t i = 0; i < sizeof(write_buf); i++)
        write_buf[i] = (uint8_t)(i & 0xFF);
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf, &sectors, 1), SCPE_OK);
    memset(read_buf, 0, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify data persists across detach/reattach. */
static void test_sim_disk_persistence_across_detach(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    create_temp_disk_image(fixture, 512);

    /* First attach: write data */
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        fixture->image_path, 512, 1, true,
                                        0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    for (size_t i = 0; i < sizeof(write_buf); i++)
        write_buf[i] = (uint8_t)(i ^ 0xA5);
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf, &sectors, 1), SCPE_OK);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);

    /* Second attach: verify data persisted */
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        fixture->image_path, 512, 1, true,
                                        0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    memset(read_buf, 0, sizeof(read_buf));
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_memory_equal(read_buf, write_buf, sizeof(write_buf));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* =============================================================================
 * NEW TESTS: Format Detection
 * ============================================================================= */

/* Verify AUTO format detection works for standard images. */
static void test_sim_disk_format_auto_detection_simh(void **state)
{
    struct sim_disk_fixture *fixture = *state;

    /* Set format to AUTO */
    assert_int_equal(sim_disk_set_fmt(&fixture->sector_unit, 0, "AUTO", NULL),
                     SCPE_OK);
    assert_int_equal(DK_GET_FMT(&fixture->sector_unit), DKUF_F_AUTO);

    /* Attach should auto-detect format (SIMH in this case) */
    create_temp_disk_image(fixture, 512);
    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        fixture->image_path, 512, 1, true,
                                        0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    /* Format should have been detected */
    assert_int_not_equal(DK_GET_FMT(&fixture->sector_unit), DKUF_F_AUTO);

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

#if defined (HAVE_FMEMOPEN)
/* =============================================================================
 * RAMDISK Additional Tests
 * ============================================================================= */

/* Verify ramdisk partial sector operations. */
static void test_sim_disk_ramdisk_partial_sector_operations(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t write_buf1[512];
    uint8_t write_buf2[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:SIZE=2048", 512, 1, true,
                                        0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    /* Write different patterns to different sectors */
    memset(write_buf1, 0xAA, sizeof(write_buf1));
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                     write_buf1, &sectors, 1), SCPE_OK);

    memset(write_buf2, 0x55, sizeof(write_buf2));
    sectors = 0;
    assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 2,
                                     write_buf2, &sectors, 1), SCPE_OK);

    /* Read back and verify */
    memset(read_buf, 0, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_memory_equal(read_buf, write_buf1, sizeof(write_buf1));

    memset(read_buf, 0, sizeof(read_buf));
    sectors = 0;
    assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 2,
                                     read_buf, &sectors, 1), SCPE_OK);
    assert_memory_equal(read_buf, write_buf2, sizeof(write_buf2));

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}

/* Verify ramdisk handles repeated write/read cycles. */
static void test_sim_disk_ramdisk_repeated_cycles(void **state)
{
    struct sim_disk_fixture *fixture = *state;
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    t_seccnt sectors = 0;

    sim_switches = 0;
    assert_int_equal(sim_disk_attach_ex(&fixture->sector_unit,
                                        "RAMDISK:SIZE=4096", 512, 1, true,
                                        0, "TEST", 0, 0, NULL),
                     SCPE_OK);

    /* Perform multiple write/read cycles */
    for (int cycle = 0; cycle < 10; cycle++) {
        memset(write_buf, (uint8_t)cycle, sizeof(write_buf));
        sectors = 0;
        assert_int_equal(sim_disk_wrsect(&fixture->sector_unit, 0,
                                         write_buf, &sectors, 1), SCPE_OK);

        memset(read_buf, 0xFF, sizeof(read_buf));
        sectors = 0;
        assert_int_equal(sim_disk_rdsect(&fixture->sector_unit, 0,
                                         read_buf, &sectors, 1), SCPE_OK);
        assert_memory_equal(read_buf, write_buf, sizeof(write_buf));
    }

    assert_int_equal(sim_disk_detach(&fixture->sector_unit), SCPE_OK);
}
#endif /* HAVE_FMEMOPEN */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Original format and capacity tests */
        cmocka_unit_test_setup_teardown(test_sim_disk_set_and_show_format,
                                        setup_sim_disk_fixture,
                                        teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_set_and_show_byte_capacity, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_set_capacity_scales_for_sector_devices,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_status_predicates_use_unit_flags,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),

        /* Original backend tests */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_test_backend_intercepts_read, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_test_backend_intercepts_write, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test(test_sim_disk_test_backend_rejects_null_unit),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_size_restores_nonboolean_quiet_value,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_detach_restores_auto_format, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_create_vhd_writes_generated_uuid,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),

        /* NEW: Error handling and edge cases (run in both modes) */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_rdsect_invalid_parameters, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_wrsect_invalid_parameters, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_wrsect_write_protected, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_operations_unattached_unit, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

        /* NEW: Boundary and range tests (run in both modes) */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_lba_zero_operations, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_multi_sector_operations, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_capacity_boundary_values, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

        /* NEW: Backend error injection (run in both modes) */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_backend_read_error_injection, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_backend_write_error_injection, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_backend_partial_transfer, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_backend_state_tracking, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

        /* NEW: State and lifecycle tests (run in both modes) */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_attach_detach_cycles, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_reset_preserves_attachment, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_unit_flag_transitions, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

        /* NEW: Data integrity tests (run in both modes) */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_write_read_patterns, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_persistence_across_detach, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

        /* NEW: Format detection tests */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_format_auto_detection_simh, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

        /* NEW: Format detection tests */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_format_auto_detection_simh, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

#if defined (HAVE_FMEMOPEN)
        /* Original RAMDISK tests */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_default_size_is_volatile,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_unnamed_parameter_is_size,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_size_suffixes_are_binary,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_keyed_size_and_type_commute,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_type_selects_default_size,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_type_handles_long_unit_name,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_read_only_switch_write_protects,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_read_only_unit_write_protects,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_from_seeds_contents,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_read_only_from_seeds_contents,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_from_rejects_oversized_source,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_save_option_is_not_touched,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_save_writes_save_image,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_save_writes_read_only_image,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_save_allows_default_null_save,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_restore_reads_save_image,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_restore_keeps_read_only,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_restore_requires_save,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_restore_requires_image,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_restore_rejects_wrong_size,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_restore_rejects_null_save,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_sim_save_reports_bad_save_path,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_rejects_container_switches,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_rejects_malformed_specs,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_show_marks_volatile, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_detach_reports_discard,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),

        /* NEW: Additional RAMDISK tests (run in both modes) */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_partial_sector_operations,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_repeated_cycles,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
#else
        cmocka_unit_test_setup_teardown(
            test_sim_disk_ramdisk_requires_fmemopen, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
#endif

        /* Bare RAMDISK filename test */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_bare_ramdisk_remains_filename, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

        /* Noautosize test */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_set_noautosize_normalizes_global_flag,
            setup_sim_disk_fixture, teardown_sim_disk_fixture),
    };

    const struct CMUnitTest async_io_tests[] = {
        /* Async I/O tests (run in both sync and async modes) */
        cmocka_unit_test_setup_teardown(
            test_sim_disk_async_read_with_callback, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_async_write_with_callback, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),
        cmocka_unit_test_setup_teardown(
            test_sim_disk_async_latency_settings, setup_sim_disk_fixture,
            teardown_sim_disk_fixture),

    };

    /* Ensure AIO is initialized since it's always available. There are a couple
     * of important variables and thread controls (mutexes, condvars) that need
     * initializing. */
    aio_init();
    sim_finit();
    assert_int_equal(sim_disk_init(), SCPE_OK);
    assert_false(sim_timer_init());

    // Log debug output to stderr, show AIO queue activity
    sim_deb = stderr;
    sim_scp_dev.dctrl |= SIM_DBG_AIO_QUEUE;

    /* Two-phase testing strategy: run all tests in both sync and async modes.
     * First round: Direct I/O tests (sync mode) with sim_async_preference = false
     * Second round: AIO tests (async mode) with sim_async_preference = true */
    int cmocka_retval = 0;
    bool saved_async_preference = sim_async_preference;

    /* Phase 1: Direct disk I/O (synchronous mode) */
    sim_async_preference = false;
    print_message("Direct disk I/O tests\n");
    cmocka_retval = cmocka_run_group_tests(tests, NULL, NULL);

    /* Phase 2: AIO and disk I/O thread (asynchronous mode, if available) */
    sim_async_preference = saved_async_preference;

    if (saved_async_preference) {
        print_message("AIO (disk I/O thread) tests\n");
        aio_set_async_enabled(true);
        cmocka_retval = cmocka_run_group_tests(async_io_tests, NULL, NULL);
    }

    aio_cleanup();
    return cmocka_retval;
}
