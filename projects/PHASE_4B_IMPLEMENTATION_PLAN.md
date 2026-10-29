# Phase 4B Implementation Plan
# Complete Event System Migration: Rip-and-Replace

**Created:** 2026-10-29  
**Branch:** unit_event_refactor  
**Status:** Ready to implement

---

## Executive Summary

Phase 4C (prev_interval approach) successfully fixed the i650 timing issues but the PDP-11 still deadlocks during clock calibration. The deadlock persists because UPDATE_SIM_TIME's locking creates contention during timer calibration, even with a recursive lock.

**Solution:** Complete Phase 4B - remove UPDATE_SIM_TIME entirely, replacing delta-based time tracking with absolute time + instruction-driven interpolation.

---

## Why Phase 4C Isn't Enough

### Current Issues with Phase 4C

1. **PDP-11 Deadlock During Clock Calibration**
   - Stack trace shows lock contention in `sim_aio_update_queue()`
   - Timer calibration → `sim_instr()` → `sim_process_event()` → `UPDATE_SIM_TIME` → `AIO_LOCK`
   - UPDATE_SIM_TIME calls AIO_LOCK/UNLOCK multiple times per instruction
   - Creates complex lock dependency chain during calibration

2. **UPDATE_SIM_TIME Still Exists**
   - Every event operation requires locking
   - Multiple calls per instruction execution
   - Lock contention during high-frequency operations
   - Architectural complexity that doesn't fit absolute-time model

3. **Delta-Based Time Calculation**
   - `sim_vm_time = sim_vm_time + (prev_interval - sim_interval)`
   - Accumulates rounding errors
   - Requires storing "old interval" value that doesn't naturally exist with absolute times
   - Complicates reasoning about time

### What Phase 4B Fixes

1. **Eliminates UPDATE_SIM_TIME** - No more lock contention in time tracking
2. **Absolute time as source of truth** - No delta calculations, no rounding errors
3. **Lock-free time reading** - `sim_gtime()` becomes pure calculation
4. **Simpler timer calibration** - No UPDATE_SIM_TIME calls during calibration
5. **Natural model** - Absolute fire times with interpolation between events

---

## Architecture

### Current (Phase 4C)

```c
// Time tracking via UPDATE_SIM_TIME macro
#define UPDATE_SIM_TIME do {
    int32_t _x;
    AIO_LOCK;  // ← LOCK CONTENTION HERE
    if (sim_event_heap_count() == 0)
        _x = noqueue_time;
    else
        _x = sim_event_heap_get_prev_interval();
    sim_vm_time = sim_vm_time + (_x - sim_interval);  // Delta calculation
    sim_rtime = sim_rtime + ((uint32_t) (_x - sim_interval));
    if (sim_event_heap_count() == 0)
        noqueue_time = sim_interval;
    else
        sim_event_heap_set_prev_interval(sim_interval);
    AIO_UNLOCK;
} while (0)

double sim_gtime(void) {
    UPDATE_SIM_TIME;  // ← Locks on every call
    return sim_vm_time;
}
```

### Target (Phase 4B)

```c
// Global state
static uint64_t sim_vm_time_base = 0;         // Time at last event (absolute)
static int32_t sim_instructions_since_base = 0;  // Instruction countdown at last event

// Time interpolation (NO LOCKS, pure calculation)
double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        // Interpolate: base + instructions executed since base
        int32_t elapsed = sim_instructions_since_base - sim_interval;
        return (double)(sim_vm_time_base + elapsed);
    }
    // Background threads see last event time (no interpolation)
    return (double)sim_vm_time_base;
}

// When event fires, sync time to absolute fire time
void sync_time_to_event(UNIT_EVENT *evt) {
    sim_vm_time_base = evt->fire_time;  // Absolute time
    sim_vm_time = (double)evt->fire_time;  // Legacy compatibility
    sim_instructions_since_base = sim_interval;  // Save for interpolation
}
```

---

## Implementation Steps

### Step 1: Add New Time Tracking Variables

File: [src/core/scp.c](src/core/scp.c)

```c
// Add alongside existing time variables (around line 250)
static uint64_t sim_vm_time_base = 0;         // Absolute time at last event
static int32_t sim_instructions_since_base = 0;  // Saved sim_interval for interpolation
```

### Step 2: Rewrite sim_gtime()

File: [src/core/scp.c](src/core/scp.c)

Replace current implementation with:

```c
double sim_gtime (void)
{
if (AIO_MAIN_THREAD) {
    /* Main thread: interpolate between events based on instructions executed
       
       sim_interval counts DOWN from the instruction budget to 0.
       When an event fires:
         - sim_instructions_since_base = sim_interval (the starting budget)
         - sim_vm_time_base = event's absolute fire time
       
       As instructions execute, sim_interval counts down.
       Elapsed instructions = sim_instructions_since_base - sim_interval
       Current time = sim_vm_time_base + elapsed
    */
    int32_t elapsed = sim_instructions_since_base - sim_interval;
    return (double)(sim_vm_time_base + elapsed);
} else {
    /* Background threads: return time of last event (no interpolation) */
    return (double)sim_vm_time_base;
}
}
```

**Rationale:**
- NO UPDATE_SIM_TIME call
- NO locking required (pure calculation)
- Accurate instruction-level interpolation
- Background threads get stable time (no mid-instruction changes)

### Step 3: Rewrite sim_process_event_heap()

File: [src/lib/sim_event_integration.c](src/lib/sim_event_integration.c)

Current code calls UPDATE_SIM_TIME before processing. New code syncs to absolute time:

```c
t_stat sim_process_event_heap(void)
{
    UNIT_EVENT *evt;
    UNIT *uptr;
    t_stat reason;

    event_heap_lock_acquire();

    /* Process all events at the current time in one batch */
    do {
        /* Check if heap is empty */
        if (sim_event_heap_count() == 0) {
            event_heap_lock_release();
            if (sim_interval != 0) {
                sim_interval = NOQUEUE_WAIT;
                sim_instructions_since_base = NOQUEUE_WAIT;  // NEW
            }
            return SCPE_OK;
        }

        /* Peek at next event to check its fire time */
        evt = sim_event_heap_peek();
        if (evt == NULL) {
            event_heap_lock_release();
            return SCPE_IERR;
        }

        /* Check if event is ready to fire */
        if (evt->fire_time > (uint64_t)sim_gtime()) {  // Note: > not >=
            /* No more events ready, update interval and return */
            update_sim_interval_from_heap();
            event_heap_lock_release();
            return SCPE_OK;
        }

        /* Pop the event (it's ready to fire) */
        evt = sim_event_heap_pop();
        if (evt == NULL) {
            event_heap_lock_release();
            return SCPE_IERR;
        }

        uptr = evt->unit;

        /* SYNC TIME to event's absolute fire time */
        sim_vm_time_base = evt->fire_time;
        sim_vm_time = (double)evt->fire_time;  // Legacy compatibility

        /* Calculate interval to NEXT event */
        update_sim_interval_from_heap();
        
        /* Save current sim_interval for interpolation */
        sim_instructions_since_base = sim_interval;

        event_heap_lock_release();

        /* Clear compatibility fields */
        uptr->next = NULL;
        uptr->time = 0;

        /* Save usecs_remaining before freeing event, then clear it */
        double saved_usecs = uptr->usecs_remaining;
        uptr->usecs_remaining = 0.0;

        /* Free event structure */
        free(evt);

        /* Execute event action */
        sim_debug(SIM_DBG_EVENT, &sim_scp_dev, "Heap processing event for %s at time %lu\n",
                  sim_uname(uptr), (unsigned long)sim_vm_time_base);

        if (saved_usecs != 0.0) {
            if (saved_usecs < 0.0) {
                sim_debug(SIM_DBG_EVENT, &sim_scp_dev, "Clamping negative usecs_remaining %.0f to 0.0 for %s\n",
                          saved_usecs, sim_uname(uptr));
                saved_usecs = 0.0;
            }
            if (saved_usecs > 0.0) {
                sim_debug(SIM_DBG_EVENT, &sim_scp_dev, "Requeueing %s after %.0f usecs\n", sim_uname(uptr),
                          saved_usecs);
                reason = sim_timer_activate_after(uptr, saved_usecs);
            } else {
                if (uptr->action != NULL)
                    reason = uptr->action(uptr);
                else
                    reason = SCPE_OK;
            }
        } else {
            if (uptr->action != NULL)
                reason = uptr->action(uptr);
            else
                reason = SCPE_OK;
        }

        /* If action returned an error/stop, return it */
        if (reason != SCPE_OK) {
            return reason;
        }

        /* Re-acquire lock for next iteration */
        event_heap_lock_acquire();

    } while (1);
}
```

**Key changes:**
- Replace "Advance time to event fire time" with "SYNC TIME to event's absolute fire time"
- Set `sim_vm_time_base = evt->fire_time` (absolute)
- Set `sim_instructions_since_base = sim_interval` for interpolation
- No UPDATE_SIM_TIME calls

### Step 4: Remove UPDATE_SIM_TIME from wrapper functions

File: [src/core/scp.c](src/core/scp.c)

Current wrappers call UPDATE_SIM_TIME:

```c
t_stat _sim_activate (UNIT *uptr, int32_t event_time)
{
if (sim_is_active (uptr))                               /* already active? */
    return SCPE_OK;
UPDATE_SIM_TIME;  // ← REMOVE THIS
return _sim_activate_heap(uptr, event_time);
}

t_stat _sim_activate_abs (UNIT *uptr, int32_t event_time)
{
if (sim_is_active (uptr))                               /* already active? */
    return SCPE_OK;
UPDATE_SIM_TIME;  // ← REMOVE THIS
return _sim_activate_heap(uptr, event_time);
}

t_stat sim_cancel (UNIT *uptr)
{
if (uptr->next == NULL)                                 /* not active? */
    return SCPE_OK;
UPDATE_SIM_TIME;  // ← REMOVE THIS
return sim_cancel_heap(uptr);
}

t_stat sim_process_event (void)
{
if (stop_cpu) {
    stop_cpu = false;
    return SCPE_STOP;
    }
AIO_UPDATE_QUEUE;
UPDATE_SIM_TIME;  // ← REMOVE THIS
return sim_process_event_heap();
}
```

**New code (all UPDATE_SIM_TIME calls removed):**

```c
t_stat _sim_activate (UNIT *uptr, int32_t event_time)
{
if (sim_is_active (uptr))
    return SCPE_OK;
return _sim_activate_heap(uptr, event_time);
}

t_stat _sim_activate_abs (UNIT *uptr, int32_t event_time)
{
if (sim_is_active (uptr))
    return SCPE_OK;
return _sim_activate_heap(uptr, event_time);
}

t_stat sim_cancel (UNIT *uptr)
{
if (uptr->next == NULL)
    return SCPE_OK;
return sim_cancel_heap(uptr);
}

t_stat sim_process_event (void)
{
if (stop_cpu) {
    stop_cpu = false;
    return SCPE_STOP;
    }
AIO_UPDATE_QUEUE;
return sim_process_event_heap();
}
```

### Step 5: Remove UPDATE_SIM_TIME Macro Definition

File: [src/core/scp.c](src/core/scp.c)

Delete the entire UPDATE_SIM_TIME macro (currently lines ~267-285):

```c
// DELETE THIS ENTIRE BLOCK:
#define UPDATE_SIM_TIME                                         \
    do {                                                        \
        int32_t _x;                                             \
        AIO_LOCK;                                               \
        if (sim_event_heap_count() == 0)                        \
            _x = noqueue_time;                                  \
        else                                                    \
            _x = sim_event_heap_get_prev_interval();            \
        sim_vm_time = sim_vm_time + (_x - sim_interval);        \
        /* Clamp to 0 to prevent negative time during initialization */ \
        if (sim_vm_time < 0)                                    \
            sim_vm_time = 0;                                    \
        sim_rtime = sim_rtime + ((uint32_t) (_x - sim_interval)); \
        if (sim_event_heap_count() == 0)                        \
            noqueue_time = sim_interval;                        \
        else                                                    \
            sim_event_heap_set_prev_interval(sim_interval);     \
        AIO_UNLOCK;                                             \
    } while (0)
```

### Step 6: Remove prev_interval from EVENT_HEAP

File: [src/lib/sim_event_heap.c](src/lib/sim_event_heap.c)

Remove the `prev_interval` field from EVENT_HEAP structure:

```c
// Before:
typedef struct event_heap {
    UNIT_EVENT **events;
    size_t count;
    size_t capacity;
    uint64_t now;
    int32_t prev_interval;  // ← DELETE THIS
} EVENT_HEAP;

// After:
typedef struct event_heap {
    UNIT_EVENT **events;
    size_t count;
    size_t capacity;
    uint64_t now;
} EVENT_HEAP;
```

Remove accessor functions:

```c
// DELETE these functions:
int32_t sim_event_heap_get_prev_interval(void);
void sim_event_heap_set_prev_interval(int32_t interval);
```

### Step 7: Update update_sim_interval_from_heap()

File: [src/lib/sim_event_integration.c](src/lib/sim_event_integration.c)

Remove prev_interval updates:

```c
static void update_sim_interval_from_heap(void)
{
    UNIT_EVENT *next_evt;
    uint64_t now;
    int32_t delta;

    /* Must be called with lock held */

    if (sim_event_heap_count() == 0) {
        if (sim_interval != 0) {
            sim_interval = NOQUEUE_WAIT;
            // DELETE: sim_event_heap_set_prev_interval(sim_interval);
        }
        return;
    }

    next_evt = sim_event_heap_peek();
    if (next_evt == NULL) {
        if (sim_interval != 0) {
            sim_interval = NOQUEUE_WAIT;
            // DELETE: sim_event_heap_set_prev_interval(sim_interval);
        }
        return;
    }

    /* Calculate delta to next event */
    now = (uint64_t)sim_gtime();
    if (next_evt->fire_time <= now)
        delta = 0; /* overdue */
    else {
        uint64_t diff = next_evt->fire_time - now;
        delta = (diff > INT32_MAX) ? INT32_MAX : (int32_t)diff;
    }

    sim_interval = delta;
    // DELETE: sim_event_heap_set_prev_interval(sim_interval);
}
```

### Step 8: Initialize New Variables

File: [src/core/scp.c](src/core/scp.c)

Find the initialization code where `sim_vm_time` is set to 0. Add initialization of new variables:

```c
// In run_cmd() or wherever initial time is set
sim_vm_time = 0.0;
sim_vm_time_base = 0;  // NEW
sim_instructions_since_base = 0;  // NEW
sim_interval = 0;
```

---

## Testing Strategy

### Phase 1: Build and Unit Tests

1. **Build:**
   ```powershell
   cmake --build --preset windows-ninja-debug
   ```
   Expected: Clean build, no compile errors

2. **Unit Tests:**
   ```powershell
   ctest --test-dir build --parallel -R "unit.*event"
   ```
   Expected: All event unit tests pass

### Phase 2: Critical Simulators

Test the 5 simulators that were problematic:

```powershell
ctest --test-dir build --parallel -R "^(i650|3b2-400|3b2-700|pdp8|pdp9)$"
```

Expected: All 5 pass

### Phase 3: PDP-11 Clock Calibration

```powershell
ctest --test-dir build -R "^pdp11$" --output-on-failure
```

Expected: No deadlock, test completes successfully

### Phase 4: Full Regression

```powershell
ctest --test-dir build --parallel
```

Expected: All 236+ tests pass

---

## Rollback Plan

If Phase 4B fails:

1. **Revert commits:**
   ```powershell
   git reset --hard <last-phase-4c-commit>
   ```

2. **Alternative: Investigate deadlock root cause**
   - Add detailed logging to timer calibration
   - Identify exact lock sequence causing deadlock
   - Potentially make timer calibration lock-free

3. **Alternative: Make timer calibration safer**
   - Run calibration without AIO locks
   - Use lock-free time reading during calibration
   - Defer AIO operations during calibration

---

## Expected Benefits

1. **Fixes PDP-11 deadlock** - No UPDATE_SIM_TIME locking during timer calibration
2. **Simpler architecture** - Absolute time, no delta calculations
3. **Better performance** - Lock-free time reading in `sim_gtime()`
4. **More accurate timing** - No accumulating rounding errors from deltas
5. **Easier to understand** - Time flows forward, interpolate between events
6. **Aligns with heap model** - Absolute fire times throughout

---

## Risk Assessment

### Low Risk

- **Code changes are localized** - Mostly in `scp.c` and `sim_event_integration.c`
- **Heap implementation unchanged** - Only affects time tracking
- **Comprehensive test suite** - 236+ tests will catch regressions

### Medium Risk

- **Timer calibration** - Major change to how time works during calibration
- **Background threads** - Need to verify they handle non-interpolated time correctly
- **Edge cases** - Initialization, queue empty, very long intervals

### Mitigation

- **Incremental testing** - Test after each step
- **Debug logging** - Add extensive logging during development
- **Quick rollback** - Git makes reverting easy if needed

---

## Timeline

1. **Implementation:** 2-4 hours
   - Step 1-3: 1 hour (core changes)
   - Step 4-7: 1 hour (cleanup)
   - Step 8: 30 minutes (initialization)

2. **Testing:** 1-2 hours
   - Build and unit tests: 15 minutes
   - Critical simulators: 30 minutes
   - PDP-11 deadlock test: 15 minutes
   - Full regression: 30 minutes

3. **Total:** 3-6 hours

---

## Success Criteria

✅ **Build succeeds** with no warnings  
✅ **All unit tests pass**  
✅ **i650 test passes** (was failing with "FDS failed (bad ar)")  
✅ **3B2-400/700 tests pass** (were failing with negative timer delay)  
✅ **PDP-11 test completes** (no deadlock during clock calibration)  
✅ **Full test suite passes** (all 236+ tests)  
✅ **No UPDATE_SIM_TIME macro** anywhere in code  
✅ **No prev_interval in EVENT_HEAP**  
✅ **sim_gtime() is lock-free**

---

## Documentation Updates

After successful implementation, update:

1. **[EVENT_SYSTEM_COMPENDIUM.md](EVENT_SYSTEM_COMPENDIUM.md)**
   - Mark Phase 4B as COMPLETE ✅
   - Document the implementation
   - Add lessons learned

2. **Code comments**
   - Explain new time tracking variables
   - Document interpolation algorithm
   - Explain why UPDATE_SIM_TIME was removed

3. **Git commit message**
   ```
   events: Phase 4B - Remove UPDATE_SIM_TIME, use absolute time interpolation
   
   Completes event system migration by removing UPDATE_SIM_TIME macro
   and replacing delta-based time tracking with absolute time plus
   instruction-driven interpolation.
   
   Key changes:
   - Add sim_vm_time_base (absolute time at last event)
   - Add sim_instructions_since_base (for interpolation)
   - Rewrite sim_gtime() to interpolate without locking
   - Remove UPDATE_SIM_TIME macro entirely
   - Remove EVENT_HEAP.prev_interval field
   - Sync time to absolute event fire time in sim_process_event_heap()
   
   Fixes:
   - PDP-11 deadlock during clock calibration
   - Lock contention in time tracking
   - Accumulating rounding errors from delta calculations
   
   All 236+ tests pass. Phase 4 migration complete.
   ```

---

## Next Steps

Ready to implement? Confirm and I'll proceed with:
1. Making the code changes step by step
2. Building after each major step
3. Running tests to verify correctness
4. Documenting results

The implementation is straightforward - we're removing complexity (UPDATE_SIM_TIME) and replacing it with simpler absolute time tracking.
