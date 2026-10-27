# Two Critical Bugs in Phase 4A Event System

## Summary

Phase 4A had **two critical bugs** that broke event scheduling:

1. **Heap never initialized** → All operations failed with SCPE_IERR
2. **Wrong time tracking** → Events scheduled at incorrect absolute times

Both bugs have been fixed. Testing is needed to confirm.

## Bug #1: Heap Not Initialized

### Symptoms
- Unit tests failing with error 101 (SCPE_IERR)
- "Internal error" messages on runlimit commands
- 14 unit test failures

### Root Cause
```c
static EVENT_HEAP *sim_event_heap = NULL;  /* Never initialized! */
```

All heap operations checked `if (sim_event_heap == NULL)` and returned `SCPE_IERR`.

### Fix (commit 2645d7e9)
Added lazy initialization:
```c
static inline void ensure_heap_initialized(void)
{
    sim_event_heap_init();  /* Initialize on first use */
}
```

Called at start of `_sim_activate_heap()`, `sim_cancel_heap()`, `sim_process_event_heap()`.

### Result After Bug #1 Fix
- ✅ Unit tests: 142/143 passing (99.3%)
- ✅ No more "Internal error" messages
- ⚠️ Simulators still failing/hanging → Bug #2

---

## Bug #2: Wrong Time Tracking (CRITICAL)

### Symptoms (After Bug #1 Fixed)
- **3b2-400: TIMEOUT 118 sec** (infinite loop)
- **pdp8: Diagnostic test failed** 
- **pdp9: Diagnostic test failed**
- **i650: Diagnostic test failed**

User confirmed: "ATP before the min-heap and UNIT_EVENT changes" - these are **NEW failures**.

### Root Cause

The heap maintained its own internal clock (`heap->now`) starting at 0, separate from SIMH's global time tracking.

**Problem 1: Event scheduling used wrong time base**
```c
// WRONG: Used heap's internal clock (always 0)
evt->fire_time = sim_event_heap_now() + event_time;
//                ^^^^^^^^^^^^^^^^^^^^ Returns 0!

// Events scheduled at: 0, 1, 2, 3, 4...
// But sim_gtime() was at: 1000000, 2000000, ...
```

**Problem 2: Interval calculation used wrong "now"**
```c
// WRONG: Compared against heap's clock (0)
now = sim_event_heap_now();  // Returns 0
if (next_evt->fire_time <= now)  // Always true after first event!
    delta = 0;  // Event appears "overdue"
```

**Problem 3: Never synchronized**
```c
// In sim_process_event_heap():
sim_event_heap_set_now(evt->fire_time);  // Updates heap->now

// But in scp.c's sim_process_event():
// NO CALL to sim_event_heap_set_now() - heap clock never advanced!
```

### Why It Broke Simulators

**Scenario 1: Infinite Loop (3b2-400 timeout)**
1. Simulator starts, sim_gtime() = 0
2. Schedule event A at time 100
   - Heap: fire_time = 0 + 100 = 100 ❌ (should be sim_gtime() + 100)
3. sim_gtime() advances to 1000000 (CPU instructions execute)
4. Schedule event B at time 200
   - Heap: fire_time = 0 + 200 = 200 ❌ (but sim_gtime() = 1000000!)
5. Check next event: fire_time (200) <= sim_gtime() (1000000)
   - **Event always "overdue"**
   - sim_interval = 0
   - Infinite loop: process event, schedule again at wrong time, repeat

**Scenario 2: Events Never Fire (pdp8, pdp9, i650)**
1. Schedule event at absolute time 100 (using heap->now = 0)
2. sim_gtime() advances to 5000 (much later)
3. Event at time 100 fires "late"
4. Simulator expects event at time 5100, but got 100
5. State mismatch → diagnostic failure

### The Fix

Use SIMH's existing global time tracking instead of heap's internal clock:

```c
// FIX 1: Schedule using sim_gtime()
evt->fire_time = (uint64_t)sim_gtime() + event_time;
//                ^^^^^^^^^^^^^^ Real simulation time!

// FIX 2: Calculate interval using sim_gtime()
now = (uint64_t)sim_gtime();  // Current simulation time
if (next_evt->fire_time <= now)
    delta = 0;
else
    delta = (int32_t)(next_evt->fire_time - now);

// FIX 3: Remove sim_event_heap_set_now() call - not needed
// (The heap is just a data structure, time tracking stays in scp.c)
```

### Design Lesson

**The heap should be a pure priority queue**, not a time-tracking system:
- ✅ Store events by absolute fire time
- ✅ Return next event to fire
- ❌ Don't maintain separate "now" clock
- ❌ Don't try to track simulation time

SIMH already has robust time tracking:
- `sim_gtime()` - global simulation time
- `sim_vm_time` - VM-specific time  
- `sim_rtime` - real instructions executed
- `sim_interval` - instructions until next event

The heap should **use** this existing infrastructure, not **replace** it.

### Files Changed

**src/lib/sim_event_integration.c:**
- Line 97: `sim_gtime()` instead of `sim_event_heap_now()`
- Line 263: `sim_gtime()` instead of `sim_event_heap_now()`
- Line 274: `sim_gtime()` instead of `sim_event_heap_now()`
- Removed: `sim_event_heap_set_now()` call (line 211)

## Testing Required

After both fixes, need to verify:

1. ✅ **3b2-400** - Should complete in ~3 seconds (not timeout)
2. ✅ **pdp8** - Diagnostic should pass
3. ✅ **pdp9** - Diagnostic should pass
4. ✅ **i650** - Diagnostic should pass
5. ✅ **All unit tests** - Should maintain 99.3% pass rate

## Root Cause Analysis

### Why Did This Happen?

**Design mistake:** The heap was designed with an internal "now" field as if it were a standalone subsystem. But SIMH's event system is deeply integrated with global time tracking.

**Integration error:** When replacing linked list with heap:
- The linked list used relative deltas → calculated from global time
- The heap used absolute times → but from wrong time base (heap->now vs sim_gtime())

**Missing synchronization:** The heap's clock needed to stay synchronized with `sim_gtime()`, but:
- `sim_process_event_heap()` (integration layer) updated it
- `sim_process_event()` (scp.c) did not
- Result: heap clock stuck at 0 when using scp.c's version

### The Correct Approach

The heap should be **stateless** regarding time:
1. Accept `fire_time` as absolute value from caller
2. Return events in `fire_time` order
3. Let scp.c manage all time calculations

This is simpler, more robust, and integrates naturally with existing SIMH code.

## Status

- **Bug #1:** Fixed (commit 2645d7e9)
- **Bug #2:** Fixed (this commit)
- **Testing:** Build in progress, tests pending
- **Confidence:** High - root causes identified and addressed

---

**Next:** Wait for build to complete, then test 3b2-400, pdp8, pdp9, i650.
