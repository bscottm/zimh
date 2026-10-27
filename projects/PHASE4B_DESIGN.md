# Phase 4B Design: Absolute-Time Event Scheduling with Instruction-Driven Time Tracking

## Overview

Phase 4A attempted to use heap-based absolute-time events while keeping delta-based time tracking (UPDATE_SIM_TIME). This failed because the two approaches are fundamentally incompatible.

Phase 4B will implement a clean **rip-and-replace** design that uses:
1. **Heap for event scheduling** (absolute fire times) - O(log n) operations
2. **Instruction-driven time tracking** (direct advancement) - no delta calculations
3. **Interpolation** for accurate `sim_gtime()` between events

## Design Principles

1. **Absolute time is the source of truth**
   - Events stored with absolute `fire_time` values
   - No relative deltas, no linked-list encoding
   
2. **Time advances with instruction execution**
   - `sim_vm_time` tracks current simulation time
   - Advances directly, not via delta calculations
   
3. **Event fire times are checkpoints**
   - When event fires, sync `sim_vm_time` to its `fire_time`
   - Corrects any accumulated drift from instruction counting

4. **Interpolation for accuracy**
   - Between events: `sim_vm_time ≈ last_event_time + instructions_since`
   - At events: `sim_vm_time = event_fire_time` (exact)

## Architecture

### Core Data Structures

```c
/* Event stored in heap - Phase 4B unchanged from Phase 2 */
typedef struct UNIT_EVENT {
    UNIT *unit;                    // Unit to service
    uint64_t fire_time;            // ABSOLUTE sim time when event fires
    t_bool is_timer_event;         // True for timer events
    size_t heap_index;             // Position in heap (for O(log n) removal)
} UNIT_EVENT;

/* Global time tracking - NEW for Phase 4B */
static uint64_t sim_vm_time_base = 0;     // Time at last event fire
static int32_t sim_instructions_since = 0; // Instructions since last event
```

### Time Tracking: New Approach

**Current (Phase 3 and earlier):**
```c
// Delta-based: calculate how much time passed
#define UPDATE_SIM_TIME do {
    int32_t _x = sim_clock_queue->time;  // OLD interval
    sim_vm_time += (_x - sim_interval);   // Add delta
} while (0)
```

**Proposed (Phase 4B):**
```c
// Direct: sim_vm_time IS the current time, no calculation needed
double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        // Interpolate: base time + instructions executed since
        return (double)(sim_vm_time_base + sim_instructions_since);
    }
    return (double)sim_vm_time_base;
}
```

### Key Changes

#### 1. Remove UPDATE_SIM_TIME Macro

The UPDATE_SIM_TIME macro will be **completely removed**. It will not be replaced with an equivalent - time tracking happens differently now.

**Rationale:** With absolute times, we don't need delta calculations. Time advances naturally as instructions execute.

#### 2. Modify sim_gtime()

**Before (with UPDATE_SIM_TIME):**
```c
double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        UPDATE_SIM_TIME;  // Calculate delta, advance sim_vm_time
    }
    return sim_vm_time;
}
```

**After (Phase 4B):**
```c
double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        // Return interpolated time
        return (double)(sim_vm_time_base + sim_instructions_since);
    }
    return (double)sim_vm_time_base;
}
```

#### 3. Track Instructions in sim_process_event()

When an event fires, we synchronize time:

```c
t_stat sim_process_event(void) {
    // ... existing checks ...
    
    do {
        UNIT_EVENT *evt = sim_event_heap_pop();
        if (evt == NULL) break;
        
        uptr = evt->unit;
        
        // SYNC TIME: Set base to this event's fire time
        sim_vm_time_base = evt->fire_time;
        sim_instructions_since = 0;
        
        // Also update old sim_vm_time for backward compatibility
        sim_vm_time = (double)evt->fire_time;
        sim_rtime = (uint32_t)evt->fire_time;
        
        // Calculate sim_interval to NEXT event
        UNIT_EVENT *next_evt = sim_event_heap_peek();
        if (next_evt != NULL) {
            int32_t next_time = (int32_t)(next_evt->fire_time - evt->fire_time);
            sim_interval = next_time;
        } else {
            sim_interval = NOQUEUE_WAIT;
        }
        
        free(evt);
        
        // Execute event...
        if (uptr->action != NULL)
            reason = uptr->action(uptr);
        
    } while (/* ... */);
    
    return reason;
}
```

#### 4. Instruction Executor Tracks Time

In each VM's `sim_instr()` loop, after decrementing `sim_interval`:

```c
while (reason == 0) {
    // Check for events
    if (sim_interval <= 0) {
        if ((reason = sim_process_event()))
            break;
    }
    
    // Fetch and execute instruction...
    
    // Decrement event timer
    sim_interval--;
    
    // NEW: Track instructions for interpolation
    sim_instructions_since++;
}
```

**Question:** Should this be in each VM, or can we intercept it centrally?

**Option A:** Modify each VM's instruction loop (18+ simulators)
- Pro: Most accurate
- Con: Touches many files

**Option B:** Intercept in sim_process_event()
- Track using `(prev_sim_interval - current_sim_interval)`
- Pro: Central, no VM changes
- Con: Less direct

**Recommendation: Option B for initial implementation**, Option A if accuracy issues arise.

#### 5. Event Scheduling Uses Absolute Time

**Already implemented in Phase 2** (unchanged):

```c
t_stat _sim_activate_heap(UNIT *uptr, int32_t event_time) {
    // ...
    evt->fire_time = (uint64_t)sim_gtime() + event_time;
    sim_event_heap_insert(evt);
    // ...
}
```

This already works correctly! The issue was never with scheduling, but with time tracking.

### Backward Compatibility

To minimize disruption, maintain the old `sim_vm_time` global variable:

```c
/* Legacy compatibility */
double sim_vm_time = 0.0;  // Keep for code that reads it directly
uint32_t sim_rtime = 0;    // Keep for relative time users
```

Update these in `sim_process_event()` to match the new tracking.

### Removing sim_clock_queue

Once time tracking is fixed, we can remove the old linked-list queue:

1. **Phase 4B-1:** Fix time tracking (this design)
2. **Phase 4B-2:** Remove `sim_clock_queue` references
3. **Phase 4B-3:** Remove `QUEUE_LIST_END` (81 occurrences)

**Do NOT remove sim_clock_queue in Phase 4B-1.** It's not used for events anymore, but removing it is a separate cleanup step.

## Implementation Plan

### Step 1: Modify sim_gtime() (Central)

File: `src/core/scp.c`

```c
// Add new tracking variables (file scope)
static uint64_t sim_vm_time_base = 0;
static int32_t sim_instructions_since_base = 0;

// Modify sim_gtime()
double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        // Interpolate based on how far sim_interval has counted down
        // Formula: base_time + (instructions executed since base)
        //        = base_time + (interval_at_base - current_interval)
        int32_t elapsed = sim_instructions_since_base - sim_interval;
        return (double)(sim_vm_time_base + elapsed);
    }
    return (double)sim_vm_time_base;
}
```

**Key insight:** We can calculate `sim_instructions_since` from `sim_interval`!
- When event fires: `sim_instructions_since_base = sim_interval` (new starting point)
- Between events: `elapsed = sim_instructions_since_base - sim_interval`
- This gives us instruction tracking **without modifying any VMs**!

### Step 2: Modify sim_process_event() (Central)

File: `src/core/scp.c`

```c
t_stat sim_process_event(void) {
    // ... existing checks ...
    
    // Remove all UPDATE_SIM_TIME calls
    
    do {
        UNIT_EVENT *evt = sim_event_heap_pop();
        if (evt == NULL) break;
        
        uptr = evt->unit;
        
        // SYNC TIME to this event's fire time
        sim_vm_time_base = evt->fire_time;
        
        // Calculate sim_interval to next event
        UNIT_EVENT *next_evt = sim_event_heap_peek();
        if (next_evt != NULL) {
            sim_interval = (int32_t)(next_evt->fire_time - evt->fire_time);
        } else {
            sim_interval = NOQUEUE_WAIT;
        }
        
        // Save new base for interpolation
        sim_instructions_since_base = sim_interval;
        
        // Update legacy variables for compatibility
        sim_vm_time = (double)evt->fire_time;
        sim_rtime = (uint32_t)evt->fire_time;
        
        free(evt);
        
        // Execute event action...
        
    } while (/* ... */);
    
    return reason;
}
```

### Step 3: Remove UPDATE_SIM_TIME Macro

File: `src/core/scp.c`

Delete the macro definition entirely. Search for all uses:

```bash
grep -n "UPDATE_SIM_TIME" src/core/scp.c
```

Remove each occurrence - they're no longer needed.

### Step 4: Test Thoroughly

1. Unit tests (17 event system tests)
2. Integration tests (all simulators)
3. Timing accuracy tests (compare old vs new `sim_gtime()` values)

## Migration Path

### Phase 4B-1: Time Tracking (This Design)
- Implement interpolation-based `sim_gtime()`
- Modify `sim_process_event()` to sync time at events
- Remove UPDATE_SIM_TIME
- Test: All simulators should work correctly

### Phase 4B-2: Queue Cleanup
- Remove `sim_clock_queue` references
- Update files that access queue directly:
  - `src/runtime/sim_timer.c`
  - `simulators/VAX/vax_cpu.c`
  - `simulators/Ibm1130/ibm1130_cpu.c`

### Phase 4B-3: Sentinel Removal
- Remove QUEUE_LIST_END (81 occurrences)
- Final cleanup

## Risk Assessment

### Risks

1. **Time tracking accuracy**
   - Mitigation: Interpolation formula preserves instruction-level accuracy
   - Validation: Compare old vs new `sim_gtime()` in test harness

2. **Backward compatibility**
   - Risk: Code that directly reads `sim_vm_time`
   - Mitigation: Keep variable, update it alongside new tracking

3. **Edge cases**
   - Negative intervals (catchup mode)
   - Empty queue scenarios
   - Async I/O events
   - Mitigation: Thorough testing with existing test suite

### Confidence Level

**High confidence** this approach will work because:
1. It aligns with the natural instruction-driven execution model
2. Heap already works (Phase 2 validated this)
3. Time tracking is simplified, not complicated
4. No circular dependencies (main cause of Phase 4A failures)

## Testing Strategy

### Unit Tests (Already Exist)
- 17 event system tests in `tests/unit/src/lib/test_sim_events.c`
- Should all pass unchanged (heap operations unchanged)

### Integration Tests (Already Exist)
- 236 simulator tests in `tests/integration/`
- Should all pass (time tracking behavior should match)

### New Tests Needed

1. **Time accuracy test:**
   ```c
   // Verify sim_gtime() interpolation is accurate
   t1 = sim_gtime();
   run_1000_instructions();
   t2 = sim_gtime();
   assert(t2 - t1 == 1000);  // Assuming 1 instruction = 1 time unit
   ```

2. **Event timing test:**
   ```c
   // Verify events fire at correct absolute times
   sim_activate(unit, 500);
   t_fire = sim_gtime();  // After event fires
   assert(t_fire == 500);
   ```

3. **Interpolation test:**
   ```c
   // Verify time advances smoothly between events
   sim_activate(unit, 1000);
   run_500_instructions();
   t_mid = sim_gtime();
   assert(t_mid == 500);  // Halfway point
   ```

## Questions for Review

1. **Interpolation formula:** Is `sim_vm_time_base + (sim_instructions_since_base - sim_interval)` the right approach?

2. **VM modifications:** Should we add `sim_instructions_since++` to each VM's loop, or calculate from `sim_interval`?
   - **Recommendation:** Calculate from `sim_interval` (no VM changes needed)

3. **Catchup mode:** How should negative intervals work with interpolation?
   - **Proposal:** Still works - negative sim_interval means we're "behind", interpolation naturally handles this

4. **AIO events:** Do async I/O events need special handling?
   - **Analysis needed:** Check how AIO_EVENT_* macros interact with time tracking

## Summary

Phase 4B replaces delta-based time tracking (UPDATE_SIM_TIME) with instruction-driven interpolation. Key changes:

1. **Remove:** UPDATE_SIM_TIME macro
2. **Modify:** `sim_gtime()` to use interpolation
3. **Modify:** `sim_process_event()` to sync time at events
4. **Add:** Base time tracking (`sim_vm_time_base`, `sim_instructions_since_base`)

**No changes needed to:**
- Heap implementation (already works)
- Event scheduling functions (already use absolute times)
- Individual VMs (time advances via existing `sim_interval--`)

**Result:** Clean separation between event scheduling (heap) and time tracking (interpolation).

Next step: Implement and test Phase 4B-1.
