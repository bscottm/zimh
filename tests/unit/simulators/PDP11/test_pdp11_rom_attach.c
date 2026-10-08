#include <stdint.h>
#include <string.h>

#include "test_cmocka.h"

#include "pdp11_defs.h"

static sim_off_t test_rom_stub_size;
static unsigned test_rom_stub_calls;

static sim_off_t test_sim_fsize_name(const char *fname)
{
    (void)fname;
    test_rom_stub_calls++;
    return test_rom_stub_size;
}

static t_stat test_rom_stub_attach_unit(UNIT *uptr, const char *cptr)
{
    (void)cptr;
    uptr->flags |= UNIT_ATT;
    return SCPE_OK;
}

static t_stat test_rom_stub_build_ubus_tab(DEVICE *dptr, DIB *dibp)
{
    (void)dptr;
    (void)dibp;
    return SCPE_OK;
}

void cpu_set_boot(int32_t pc)
{
    (void)pc;
}

#define sim_fsize_name test_sim_fsize_name
#define attach_unit test_rom_stub_attach_unit
#define build_ubus_tab test_rom_stub_build_ubus_tab
#include "pdp11_rom.c"
#undef build_ubus_tab
#undef attach_unit
#undef sim_fsize_name

static void reset_rom_unit(int32_t base_addr)
{
    memset(&rom_unit[0], 0, sizeof(rom_unit[0]));
    rom_unit[0].unit_base = base_addr;
    rom_unit[0].unit_end = base_addr;
    test_rom_stub_calls = 0;
    test_rom_stub_size = 0;
}

/* A known file size becomes the unit's capacity, widened past 32 bits,
   and extends unit_end by that many bytes from unit_base. */
static void test_rom_attach_sets_capac_and_end_from_file_size(void **state)
{
    (void)state;

    reset_rom_unit(IOPAGEBASE + 0100);
    test_rom_stub_size = 8192;

    assert_int_equal(rom_attach(&rom_unit[0], "image.rom"), SCPE_OK);
    assert_int_equal(test_rom_stub_calls, 1);
    assert_int_equal((int64_t)rom_unit[0].capac, 8192);
    assert_int_equal(rom_unit[0].unit_end, rom_unit[0].unit_base + 8192);
}

/* A zero-length ROM image is rejected before attach_unit is even
   called. */
static void test_rom_attach_rejects_zero_size(void **state)
{
    (void)state;

    reset_rom_unit(IOPAGEBASE + 0100);
    test_rom_stub_size = 0;

    assert_int_equal(rom_attach(&rom_unit[0], "image.rom"), SCPE_OPENERR);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_rom_attach_sets_capac_and_end_from_file_size),
        cmocka_unit_test(test_rom_attach_rejects_zero_size),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
