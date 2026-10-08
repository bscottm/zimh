#include <stdint.h>
#include <string.h>

#include "test_cmocka.h"

#include "pdp8_defs.h"

static sim_off_t test_df_stub_size;
static unsigned test_df_stub_calls;

static sim_off_t test_sim_fsize_name(const char *fname)
{
    (void)fname;
    test_df_stub_calls++;
    return test_df_stub_size;
}

static t_stat test_df_stub_attach_unit(UNIT *uptr, const char *cptr)
{
    (void)cptr;
    uptr->flags |= UNIT_ATT;
    return SCPE_OK;
}

uint16_t M[MAXMEMSIZE];
int32_t int_req = 0;
int32_t stop_inst = 0;
UNIT cpu_unit = { UDATA(NULL, UNIT_FIX + UNIT_BINK, MAXMEMSIZE) };

void cpu_set_bootpc(int32_t pc)
{
    (void)pc;
}

t_stat set_dev(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat show_dev(FILE *st, UNIT *uptr, int32_t val, const void *desc)
{
    (void)st;
    (void)uptr;
    (void)val;
    (void)desc;
    return SCPE_OK;
}

#define sim_fsize_name test_sim_fsize_name
#define attach_unit test_df_stub_attach_unit
#include "pdp8_df.c"
#undef attach_unit
#undef sim_fsize_name

static void reset_df_unit(UNIT *uptr)
{
    memset(uptr, 0, sizeof(*uptr));
    uptr->flags = UNIT_AUTO;
    test_df_stub_calls = 0;
    test_df_stub_size = 0;
}

/* The stored platter field is one less than the decoded platter count
   (UNIT_GETP adds 1), so a file holding exactly one platter's worth of
   bytes autosizes to a decoded count of 2. This mirrors the existing,
   unchanged field-encoding behavior of this device. */
static void test_df_attach_autosizes_platters_from_file_size(void **state)
{
    UNIT unit;
    (void)state;

    reset_df_unit(&unit);
    test_df_stub_size = (sim_off_t)DF_DKSIZE * (sim_off_t)sizeof(int16_t);

    assert_int_equal(df_attach(&unit, "disk.df"), SCPE_OK);
    assert_int_equal(test_df_stub_calls, 1);
    assert_int_equal(UNIT_GETP(unit.flags), 2);
    assert_int_equal(unit.capac,
                     (t_addr)(UNIT_GETP(unit.flags) * DF_DKSIZE));
}

/* A file far larger than the controller supports clamps to the maximum
   platter count instead of indexing out of range. */
static void test_df_attach_clamps_platters_at_df_numdk(void **state)
{
    UNIT unit;
    (void)state;

    reset_df_unit(&unit);
    test_df_stub_size = (sim_off_t)DF_DKSIZE * (sim_off_t)sizeof(int16_t) *
                        (DF_NUMDK + 50);

    assert_int_equal(df_attach(&unit, "disk.df"), SCPE_OK);
    assert_int_equal(UNIT_GETP(unit.flags), DF_NUMDK);
}

/* Regression: before widening sz to sim_off_t, sim_fsize_name()
   truncated the file size to 32 bits, so a size just above 4GiB
   wrapped down to a tiny value instead of clamping to the maximum.
   Confirm the fixed path still clamps correctly for a size that does
   not fit in uint32_t. */
static void test_df_attach_does_not_truncate_size_above_32_bits(void **state)
{
    UNIT unit;
    (void)state;

    reset_df_unit(&unit);
    test_df_stub_size = ((sim_off_t)1 << 32) + 4096;

    assert_int_equal(df_attach(&unit, "disk.df"), SCPE_OK);
    assert_int_equal(UNIT_GETP(unit.flags), DF_NUMDK);
    assert_int_equal(unit.capac, (t_addr)(DF_NUMDK * DF_DKSIZE));
}

/* A zero-length file is falsy, so autosizing is skipped and existing
   platter flags are left untouched. */
static void test_df_attach_zero_size_skips_autosize(void **state)
{
    UNIT unit;
    (void)state;

    reset_df_unit(&unit);
    unit.flags = (unit.flags & ~UNIT_PLAT) | (1 << UNIT_V_PLAT);
    test_df_stub_size = 0;

    assert_int_equal(df_attach(&unit, "disk.df"), SCPE_OK);
    assert_int_equal(test_df_stub_calls, 1);
    assert_int_equal(UNIT_GETP(unit.flags), 2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_df_attach_autosizes_platters_from_file_size),
        cmocka_unit_test(test_df_attach_clamps_platters_at_df_numdk),
        cmocka_unit_test(
            test_df_attach_does_not_truncate_size_above_32_bits),
        cmocka_unit_test(test_df_attach_zero_size_skips_autosize),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
