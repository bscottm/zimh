#include <stdint.h>
#include <string.h>

#include "test_cmocka.h"

#include "nova_defs.h"

static sim_off_t test_dsk_stub_size;
static unsigned test_dsk_stub_calls;

static sim_off_t test_sim_fsize_name(const char *fname)
{
    (void)fname;
    test_dsk_stub_calls++;
    return test_dsk_stub_size;
}

static t_stat test_dsk_stub_attach_unit(UNIT *uptr, const char *cptr)
{
    (void)cptr;
    uptr->flags |= UNIT_ATT;
    return SCPE_OK;
}

uint16_t M[MAXMEMSIZE];
UNIT cpu_unit = { 0 };
int32_t int_req = 0;
int32_t dev_busy = 0;
int32_t dev_done = 0;
int32_t dev_disable = 0;
int32_t saved_PC = 0;
int32_t SR = 0;
int32_t AMASK = 0;

int32_t MapAddr(int32_t map, int32_t addr)
{
    (void)map;
    return addr;
}

#define sim_fsize_name test_sim_fsize_name
#define attach_unit test_dsk_stub_attach_unit
#include "nova_dsk.c"
#undef attach_unit
#undef sim_fsize_name

static void reset_dsk_unit(void)
{
    memset(&dsk_unit, 0, sizeof(dsk_unit));
    dsk_unit.flags = UNIT_AUTO;
    test_dsk_stub_calls = 0;
    test_dsk_stub_size = 0;
}

/* The stored platter field is one less than the decoded platter count
   (UNIT_GETP adds 1), so a file holding exactly one platter's worth of
   bytes autosizes to a decoded count of 2. This mirrors the existing,
   unchanged field-encoding behavior of this device. */
static void test_dsk_attach_autosizes_platters_from_file_size(void **state)
{
    (void)state;

    reset_dsk_unit();
    test_dsk_stub_size =
        (sim_off_t)DSK_DKSIZE * (sim_off_t)sizeof(int16_t);

    assert_int_equal(dsk_attach(&dsk_unit, "disk.dsk"), SCPE_OK);
    assert_int_equal(test_dsk_stub_calls, 1);
    assert_int_equal(UNIT_GETP(dsk_unit.flags), 2);
    assert_int_equal(dsk_unit.capac,
                     (t_addr)(UNIT_GETP(dsk_unit.flags) * DSK_DKSIZE));
}

/* A file far larger than the controller supports clamps to the maximum
   platter count instead of indexing out of range. */
static void test_dsk_attach_clamps_platters_at_dsk_numdk(void **state)
{
    (void)state;

    reset_dsk_unit();
    test_dsk_stub_size = (sim_off_t)DSK_DKSIZE * (sim_off_t)sizeof(int16_t) *
                         (DSK_NUMDK + 50);

    assert_int_equal(dsk_attach(&dsk_unit, "disk.dsk"), SCPE_OK);
    assert_int_equal(UNIT_GETP(dsk_unit.flags), DSK_NUMDK);
}

/* Regression: before widening sz to sim_off_t, sim_fsize_name()
   truncated the file size to 32 bits, so a size just above 4GiB
   wrapped down to a tiny value instead of clamping to the maximum.
   Confirm the fixed path still clamps correctly for a size that does
   not fit in uint32_t. */
static void test_dsk_attach_does_not_truncate_size_above_32_bits(
    void **state)
{
    (void)state;

    reset_dsk_unit();
    test_dsk_stub_size = ((sim_off_t)1 << 32) + 4096;

    assert_int_equal(dsk_attach(&dsk_unit, "disk.dsk"), SCPE_OK);
    assert_int_equal(UNIT_GETP(dsk_unit.flags), DSK_NUMDK);
    assert_int_equal(dsk_unit.capac, (t_addr)(DSK_NUMDK * DSK_DKSIZE));
}

/* A zero-length file is falsy, so autosizing is skipped and existing
   platter flags are left untouched. */
static void test_dsk_attach_zero_size_skips_autosize(void **state)
{
    (void)state;

    reset_dsk_unit();
    dsk_unit.flags = (dsk_unit.flags & ~UNIT_PLAT) | (1 << UNIT_V_PLAT);
    test_dsk_stub_size = 0;

    assert_int_equal(dsk_attach(&dsk_unit, "disk.dsk"), SCPE_OK);
    assert_int_equal(test_dsk_stub_calls, 1);
    assert_int_equal(UNIT_GETP(dsk_unit.flags), 2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_dsk_attach_autosizes_platters_from_file_size),
        cmocka_unit_test(test_dsk_attach_clamps_platters_at_dsk_numdk),
        cmocka_unit_test(
            test_dsk_attach_does_not_truncate_size_above_32_bits),
        cmocka_unit_test(test_dsk_attach_zero_size_skips_autosize),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
