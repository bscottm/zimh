#include <stdint.h>
#include <string.h>

#include "test_cmocka.h"

#include "pdp18b_defs.h"

static sim_off_t test_rf_stub_size;
static unsigned test_rf_stub_calls;

static sim_off_t test_sim_fsize_name(const char *fname)
{
    (void)fname;
    test_rf_stub_calls++;
    return test_rf_stub_size;
}

static t_stat test_rf_stub_attach_unit(UNIT *uptr, const char *cptr)
{
    (void)cptr;
    uptr->flags |= UNIT_ATT;
    return SCPE_OK;
}

int32_t *M = NULL;
int32_t int_hwre[API_HLVL + 1] = { 0 };
int32_t api_vec[API_HLVL][32] = { { 0 } };
UNIT cpu_unit = { 0 };

t_stat set_devno(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat show_devno(FILE *st, UNIT *uptr, int32_t val, const void *desc)
{
    (void)st;
    (void)uptr;
    (void)val;
    (void)desc;
    return SCPE_OK;
}

t_stat set_3cyc_reg(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat show_3cyc_reg(FILE *st, UNIT *uptr, int32_t val, const void *desc)
{
    (void)st;
    (void)uptr;
    (void)val;
    (void)desc;
    return SCPE_OK;
}

#define sim_fsize_name test_sim_fsize_name
#define attach_unit test_rf_stub_attach_unit
#include "pdp18b_rf.c"
#undef attach_unit
#undef sim_fsize_name

static void reset_rf_unit(void)
{
    memset(&rf_unit, 0, sizeof(rf_unit));
    rf_unit.flags = UNIT_AUTO;
    test_rf_stub_calls = 0;
    test_rf_stub_size = 0;
}

/* The stored platter field is one less than the decoded platter count
   (UNIT_GETP adds 1), so a file holding exactly one platter's worth of
   bytes autosizes to a decoded count of 2. This mirrors the existing,
   unchanged field-encoding behavior of this device. */
static void test_rf_attach_autosizes_platters_from_file_size(void **state)
{
    (void)state;

    reset_rf_unit();
    test_rf_stub_size = (sim_off_t)RF_DKSIZE * (sim_off_t)sizeof(int32_t);

    assert_int_equal(rf_attach(&rf_unit, "disk.rf"), SCPE_OK);
    assert_int_equal(test_rf_stub_calls, 1);
    assert_int_equal(UNIT_GETP(rf_unit.flags), 2);
    assert_int_equal(rf_unit.capac,
                     (t_addr)(UNIT_GETP(rf_unit.flags) * RF_DKSIZE));
}

/* A file far larger than the controller supports clamps to the maximum
   platter count instead of indexing out of range. */
static void test_rf_attach_clamps_platters_at_rf_numdk(void **state)
{
    (void)state;

    reset_rf_unit();
    test_rf_stub_size = (sim_off_t)RF_DKSIZE * (sim_off_t)sizeof(int32_t) *
                        (RF_NUMDK + 50);

    assert_int_equal(rf_attach(&rf_unit, "disk.rf"), SCPE_OK);
    assert_int_equal(UNIT_GETP(rf_unit.flags), RF_NUMDK);
}

/* Regression: before widening sz to sim_off_t, sim_fsize_name()
   truncated the file size to 32 bits, so a size just above 4GiB
   wrapped down to a tiny value instead of clamping to the maximum.
   Confirm the fixed path still clamps correctly for a size that does
   not fit in uint32_t. */
static void test_rf_attach_does_not_truncate_size_above_32_bits(void **state)
{
    (void)state;

    reset_rf_unit();
    test_rf_stub_size = ((sim_off_t)1 << 32) + 4096;

    assert_int_equal(rf_attach(&rf_unit, "disk.rf"), SCPE_OK);
    assert_int_equal(UNIT_GETP(rf_unit.flags), RF_NUMDK);
    assert_int_equal(rf_unit.capac, (t_addr)(RF_NUMDK * RF_DKSIZE));
}

/* A zero-length file is falsy, so autosizing is skipped and existing
   platter flags are left untouched. */
static void test_rf_attach_zero_size_skips_autosize(void **state)
{
    (void)state;

    reset_rf_unit();
    rf_unit.flags = (rf_unit.flags & ~UNIT_PLAT) | (1 << UNIT_V_PLAT);
    test_rf_stub_size = 0;

    assert_int_equal(rf_attach(&rf_unit, "disk.rf"), SCPE_OK);
    assert_int_equal(test_rf_stub_calls, 1);
    assert_int_equal(UNIT_GETP(rf_unit.flags), 2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_rf_attach_autosizes_platters_from_file_size),
        cmocka_unit_test(test_rf_attach_clamps_platters_at_rf_numdk),
        cmocka_unit_test(
            test_rf_attach_does_not_truncate_size_above_32_bits),
        cmocka_unit_test(test_rf_attach_zero_size_skips_autosize),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
