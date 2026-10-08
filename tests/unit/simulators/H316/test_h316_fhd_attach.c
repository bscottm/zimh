#include <stdint.h>
#include <string.h>

#include "test_cmocka.h"

#include "h316_defs.h"

static sim_off_t test_fhd_stub_size;
static unsigned test_fhd_stub_calls;

static sim_off_t test_sim_fsize_name(const char *fname)
{
    (void)fname;
    test_fhd_stub_calls++;
    return test_fhd_stub_size;
}

static t_stat test_fhd_stub_attach_unit(UNIT *uptr, const char *cptr)
{
    (void)cptr;
    uptr->flags |= UNIT_ATT;
    return SCPE_OK;
}

int32_t dev_int = 0;
int32_t dev_enb = 0;
uint32_t chan_req = 0;
int32_t stop_inst = 0;
uint32_t dma_ad[DMA_MAX];

t_stat io_set_iobus(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat io_set_dma(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat io_set_dmc(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat io_show_chan(FILE *st, UNIT *uptr, int32_t val, const void *desc)
{
    (void)st;
    (void)uptr;
    (void)val;
    (void)desc;
    return SCPE_OK;
}

#define sim_fsize_name test_sim_fsize_name
#define attach_unit test_fhd_stub_attach_unit
#include "h316_fhd.c"
#undef attach_unit
#undef sim_fsize_name

static void reset_fhd_unit(void)
{
    memset(&fhd_unit, 0, sizeof(fhd_unit));
    fhd_unit.flags = UNIT_AUTO;
    test_fhd_stub_calls = 0;
    test_fhd_stub_size = 0;
}

/* The stored surface field is one less than the decoded surface count
   (UNIT_GETSF adds 1), so a file holding exactly one surface's worth
   of bytes autosizes to a decoded count of 2. This mirrors the
   existing, unchanged field-encoding behavior of this device. */
static void test_fhd_attach_autosizes_surfaces_from_file_size(void **state)
{
    (void)state;

    reset_fhd_unit();
    test_fhd_stub_size = (sim_off_t)FH_WDPSF * (sim_off_t)sizeof(int16_t);

    assert_int_equal(fhd_attach(&fhd_unit, "disk.fhd"), SCPE_OK);
    assert_int_equal(test_fhd_stub_calls, 1);
    assert_int_equal(UNIT_GETSF(fhd_unit.flags), 2);
    assert_int_equal(fhd_unit.capac,
                     (t_addr)(UNIT_GETSF(fhd_unit.flags) * FH_WDPSF));
}

/* A file far larger than the controller supports clamps to the maximum
   surface count instead of indexing out of range. */
static void test_fhd_attach_clamps_surfaces_at_fh_numsf(void **state)
{
    (void)state;

    reset_fhd_unit();
    test_fhd_stub_size = (sim_off_t)FH_WDPSF * (sim_off_t)sizeof(int16_t) *
                         (FH_NUMSF + 50);

    assert_int_equal(fhd_attach(&fhd_unit, "disk.fhd"), SCPE_OK);
    assert_int_equal(UNIT_GETSF(fhd_unit.flags), FH_NUMSF);
}

/* Regression: before widening sz to sim_off_t, sim_fsize_name()
   truncated the file size to 32 bits, so a size just above 4GiB
   wrapped down to a tiny value instead of clamping to the maximum.
   Confirm the fixed path still clamps correctly for a size that does
   not fit in uint32_t. */
static void test_fhd_attach_does_not_truncate_size_above_32_bits(
    void **state)
{
    (void)state;

    reset_fhd_unit();
    test_fhd_stub_size = ((sim_off_t)1 << 32) + 4096;

    assert_int_equal(fhd_attach(&fhd_unit, "disk.fhd"), SCPE_OK);
    assert_int_equal(UNIT_GETSF(fhd_unit.flags), FH_NUMSF);
    assert_int_equal(fhd_unit.capac, (t_addr)(FH_NUMSF * FH_WDPSF));
}

/* A zero-length file is falsy, so autosizing is skipped and existing
   surface flags are left untouched. */
static void test_fhd_attach_zero_size_skips_autosize(void **state)
{
    (void)state;

    reset_fhd_unit();
    fhd_unit.flags = (fhd_unit.flags & ~UNIT_SF) | (1 << UNIT_V_SF);
    test_fhd_stub_size = 0;

    assert_int_equal(fhd_attach(&fhd_unit, "disk.fhd"), SCPE_OK);
    assert_int_equal(test_fhd_stub_calls, 1);
    assert_int_equal(UNIT_GETSF(fhd_unit.flags), 2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fhd_attach_autosizes_surfaces_from_file_size),
        cmocka_unit_test(test_fhd_attach_clamps_surfaces_at_fh_numsf),
        cmocka_unit_test(
            test_fhd_attach_does_not_truncate_size_above_32_bits),
        cmocka_unit_test(test_fhd_attach_zero_size_skips_autosize),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
