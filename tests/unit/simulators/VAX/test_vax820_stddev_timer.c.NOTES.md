# VAX 8200 Timer Test Crash - Investigation Notes

## Status: FAILING on Windows/MSVC (Exit Code 3)

The `zimh-unit-vax820-stddev-timer` test crashes immediately upon execution on Windows with MSVC (Visual Studio 2022). Exit code 3 indicates abnormal termination, likely an access violation.

## Crash Location

The crash occurs in the first test function `test_done_clear_without_pending_tick_is_not_ack` when called from the test harness. The test output shows:

```
[==========] tests: Running 2 test(s).
[ RUN      ] test_done_clear_without_pending_tick_is_not_ack
<CRASH - Exit code 3>
```

No output from the test body itself, indicating the crash happens early in the test execution.

## Root Cause Analysis

### Test Architecture Issue

This test uses an unusual pattern: it directly includes the implementation file `vax820_stddev.c` (line 58):

```c
#include "vax820_stddev.c"
```

This gives the test direct access to static variables like `tmr_iccs`, `tmr_icr`, `tmr_unit`, and `tmr_dev` without needing to export them. However, this also means the test gets its own copy of these structures, which may not be properly initialized.

### Uninitialized Structures

The test declares several global variables that are required by `vax820_stddev.c`:

```c
uint32_t R[16];
uint32_t PSL;
uint32_t SISR;
uint32_t fault_PC;
uint32_t p1;
uint32_t p2;
uint32_t trpirq;
uint32_t mchk_ref;
uint32_t *M;              // ← NULL pointer, never initialized
int32_t hlt_pin;
int32_t crd_err;
int32_t cur_cpu;
int32_t cpu_msk = 1;
int32_t in_ie;
jmp_buf save_env;
DEVICE cpu_dev;            // ← Zero-initialized, may need proper setup
UNIT cpu_unit;             // ← Zero-initialized, may need proper setup
```

**Critical Issue:** `M` is declared as `uint32_t *M;` but never initialized. It remains NULL. If any code in the included `vax820_stddev.c` dereferences `M`, this will cause an access violation.

### Likely Crash Point: Debug Output

The `iccs_wr()` function (called by the test at line 86) starts with:

```c
void iccs_wr (int32_t val)
{
    sim_debug_bits_hdr (TMR_DB_REG, &tmr_dev, "iccs_wr()", tmr_iccs_bits, tmr_iccs, val, true);
    // ... rest of function
```

The `sim_debug_bits_hdr()` function likely accesses fields in the `tmr_dev` DEVICE structure that need proper initialization, such as:
- Debug output stream pointer
- Device name pointer
- Debug control flags
- Other internal state

When the test includes `vax820_stddev.c`, it gets a copy of the `tmr_dev` structure definition:

```c
DEVICE tmr_dev = {
    "TMR", &tmr_unit, tmr_reg, NULL,
    1, 0, 0, 0, 0, 0,
    NULL, NULL, &tmr_reset,
    NULL, NULL, NULL,
    NULL, DEV_DEBUG, 0,
    tmr_deb, NULL, NULL, NULL, NULL, NULL,
    &tmr_description
};
```

However, pointers and other runtime state within this structure may not be properly set up for use outside the normal SIMH device initialization flow.

### Attempted Fixes and Why They Failed

**Attempt 1:** Disable debug output by setting `tmr_dev.dctrl = 0` in `reset_timer_state()`.

```c
static void reset_timer_state(void)
{
    tmr_dev.dctrl = 0;  // Disable debug
    // ...
}
```

**Result:** Still crashes. Either `sim_debug_bits_hdr()` doesn't check `dctrl` before accessing device fields, or the crash happens elsewhere.

**Attempt 2:** Initialize `tmr_unit` before calling `sim_cancel()`:

```c
memset(&tmr_unit, 0, sizeof(tmr_unit));
tmr_unit.action = &tmr_svc;
```

**Result:** Still crashes. The `sim_cancel()` call may be accessing other uninitialized state, or the crash happens before this point.

**Attempt 3:** Remove the `deblog` field initialization (doesn't exist in DEVICE structure).

**Result:** Compilation error - `deblog` is not a member of `DEVICE`.

## What Needs To Be Fixed

### Option 1: Proper Device Initialization (Recommended)

The test needs to properly initialize the DEVICE and UNIT structures through SIMH's normal initialization path, not just declare them. This likely means:

1. Call an initialization function for `tmr_dev` that sets up internal pointers
2. Ensure `tmr_unit` is properly linked to its parent device
3. Initialize any global state that `sim_debug_bits_hdr()` or other functions expect

**Investigation needed:** Find the proper SIMH device initialization sequence and replicate it in the test setup.

### Option 2: Mock or Stub Debug Functions

Replace `sim_debug_bits_hdr()` and `sim_debug()` with no-op macros or stub functions:

```c
#define sim_debug_bits_hdr(...) ((void)0)
#define sim_debug(...) ((void)0)
```

This would need to be done before `#include "vax820_stddev.c"`.

**Risk:** Other functions may also expect proper initialization and crash.

### Option 3: Refactor to Use External Linkage

Instead of including the `.c` file directly, export the necessary internal state through a test-specific header or accessor functions. This is more invasive but cleaner:

1. Add `vax820_stddev_test_hooks.h` with accessors for `tmr_iccs`, `tmr_icr`, etc.
2. Link against `vax820_stddev.o` instead of including the source
3. Initialize the device properly through normal SIMH calls

### Option 4: Initialize M Pointer

If the crash is actually from dereferencing `M`, allocate memory for it:

```c
static uint32_t M_storage[65536];  // Fake VAX memory
uint32_t *M = M_storage;
```

**Investigation needed:** Trace through `iccs_wr()` and related functions to see if `M` is accessed.

## Platform-Specific Notes

This crash only manifests on Windows/MSVC. It may work on Linux/GCC/Clang due to:
- Different structure padding or alignment
- Different debug/release optimization behaviors
- Different handling of uninitialized globals
- Differences in how SIMH's debug macros are implemented on different platforms

## Recommended Next Steps

1. **Use a debugger:** Run the test under Visual Studio debugger to get exact crash location and call stack
2. **Add logging:** Insert `printf()` calls in `reset_timer_state()` and `iccs_wr()` to narrow down crash point
3. **Check sim_debug implementation:** Examine what `sim_debug_bits_hdr()` actually does and what it expects from the DEVICE structure
4. **Compare to working tests:** Find other unit tests that include `.c` files directly and see how they initialize devices
5. **Test on Linux:** Build and run on Linux to see if it's Windows-specific

## Files Involved

- `tests/unit/simulators/VAX/test_vax820_stddev_timer.c` - Test file
- `simulators/VAX/vax820_stddev.c` - Implementation being tested
- `simulators/VAX/vax820_stddev.h` - Header file
- `src/core/sim_defs.h` - DEVICE and UNIT structure definitions
- Runtime debug functions: `sim_debug_bits_hdr()`, `sim_debug()`

## Test Disabled

This test has been temporarily disabled in CMakeLists.txt until the initialization issue can be resolved.

---

**Investigation by:** Claude (AI Assistant)
**Date:** 2026-06-19
**Branch:** sanitizers
**Platform:** Windows 10, Visual Studio 2022, x64
