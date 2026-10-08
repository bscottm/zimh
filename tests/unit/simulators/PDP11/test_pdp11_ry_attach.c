#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "test_cmocka.h"

#include "pdp11_defs.h"

static sim_off_t test_ry_stub_size;
static unsigned test_ry_stub_calls;

static sim_off_t test_sim_fsize_name(const char *fname)
{
    (void)fname;
    test_ry_stub_calls++;
    return test_ry_stub_size;
}

static t_stat test_ry_stub_attach_unit(UNIT *uptr, const char *cptr)
{
    (void)cptr;
    uptr->flags |= UNIT_ATT;
    return SCPE_OK;
}

int32_t int_req[IPL_HLVL];
uint32_t cpu_opt;
uint32_t cpu_type;
uint16_t *M;
jmp_buf save_env;

t_stat auto_config(const char *name, int32_t nctrl)
{
    (void)name;
    (void)nctrl;
    return SCPE_OK;
}

t_stat set_addr(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat show_addr(FILE *st, UNIT *uptr, int32_t val, const void *desc)
{
    (void)st;
    (void)uptr;
    (void)val;
    (void)desc;
    return SCPE_OK;
}

t_stat set_addr_flt(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat set_vec(UNIT *uptr, int32_t val, const char *cptr, void *desc)
{
    (void)uptr;
    (void)val;
    (void)cptr;
    (void)desc;
    return SCPE_OK;
}

t_stat show_vec(FILE *st, UNIT *uptr, int32_t val, const void *desc)
{
    (void)st;
    (void)uptr;
    (void)val;
    (void)desc;
    return SCPE_OK;
}

void cpu_set_boot(int32_t pc)
{
    (void)pc;
}

int32_t Map_ReadB(uint32_t ba, int32_t bc, uint8_t *buf)
{
    (void)ba;
    (void)bc;
    (void)buf;
    return 0;
}

int32_t Map_WriteB(uint32_t ba, int32_t bc, const uint8_t *buf)
{
    (void)ba;
    (void)bc;
    (void)buf;
    return 0;
}

#define sim_fsize_name test_sim_fsize_name
#define attach_unit test_ry_stub_attach_unit
#include "pdp11_ry.c"
#undef attach_unit
#undef sim_fsize_name

static void reset_ry_unit(UNIT *uptr)
{
    memset(uptr, 0, sizeof(*uptr));
    uptr->flags = UNIT_AUTO;
    test_ry_stub_calls = 0;
    test_ry_stub_size = 0;
}

/* A file no larger than a single-density image stays single density. */
static void test_ry_attach_stays_single_density_at_or_below_rx_size(
    void **state)
{
    UNIT unit;
    (void)state;

    reset_ry_unit(&unit);
    test_ry_stub_size = (sim_off_t)RX_SIZE;

    assert_int_equal(ry_attach(&unit, "disk.ry"), SCPE_OK);
    assert_int_equal(test_ry_stub_calls, 1);
    assert_true((unit.flags & UNIT_DEN) == 0);
    assert_int_equal(unit.capac, RX_SIZE);
}

/* A file larger than a single-density image switches to double
   density. */
static void test_ry_attach_switches_to_double_density_above_rx_size(
    void **state)
{
    UNIT unit;
    (void)state;

    reset_ry_unit(&unit);
    test_ry_stub_size = (sim_off_t)RX_SIZE + 1;

    assert_int_equal(ry_attach(&unit, "disk.ry"), SCPE_OK);
    assert_true((unit.flags & UNIT_DEN) != 0);
    assert_int_equal(unit.capac, RY_SIZE);
}

/* Regression: before widening sz to sim_off_t, sim_fsize_name()
   truncated the file size to 32 bits. Confirm the fixed path still
   selects double density for a size that does not fit in uint32_t. */
static void test_ry_attach_does_not_truncate_size_above_32_bits(
    void **state)
{
    UNIT unit;
    (void)state;

    reset_ry_unit(&unit);
    test_ry_stub_size = ((sim_off_t)1 << 32) + 4096;

    assert_int_equal(ry_attach(&unit, "disk.ry"), SCPE_OK);
    assert_true((unit.flags & UNIT_DEN) != 0);
    assert_int_equal(unit.capac, RY_SIZE);
}

/* A zero-length file is falsy, so autosizing is skipped and the
   existing density flag is left untouched. */
static void test_ry_attach_zero_size_skips_autosize(void **state)
{
    UNIT unit;
    (void)state;

    reset_ry_unit(&unit);
    unit.flags |= UNIT_DEN;
    test_ry_stub_size = 0;

    assert_int_equal(ry_attach(&unit, "disk.ry"), SCPE_OK);
    assert_int_equal(test_ry_stub_calls, 1);
    assert_true((unit.flags & UNIT_DEN) != 0);
    assert_int_equal(unit.capac, RY_SIZE);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(
            test_ry_attach_stays_single_density_at_or_below_rx_size),
        cmocka_unit_test(
            test_ry_attach_switches_to_double_density_above_rx_size),
        cmocka_unit_test(
            test_ry_attach_does_not_truncate_size_above_32_bits),
        cmocka_unit_test(test_ry_attach_zero_size_skips_autosize),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
