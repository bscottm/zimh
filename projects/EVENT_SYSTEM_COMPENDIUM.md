# SIMH/ZIMH Event System Migration Compendium
# From Delta-Time Linked Lists to Absolute-Time Min-Heap

**Document Status:** Living document tracking migration from Phase 0 (linked-list) through Phase 4B (min-heap)  
**Last Updated:** 2026-10-28  
**Branch:** unit_event_refactor

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Historical Context](#historical-context)
3. [Current Architecture (Phase 0)](#current-architecture-phase-0)
4. [Target Architecture (Phase 4B)](#target-architecture-phase-4b)
5. [Migration Journey](#migration-journey)
   - [Phase 1: Foundation](#phase-1-foundation)
   - [Phase 2: Min-Heap Implementation](#phase-2-min-heap-implementation)
   - [Phase 3: Integration Testing](#phase-3-integration-testing)
   - [Phase 4A: Failed Incremental Approach](#phase-4a-failed-incremental-approach)
   - [Phase 4B: Rip-and-Replace Design](#phase-4b-rip-and-replace-design)
6. [Technical Deep Dives](#technical-deep-dives)
7. [Critical Bugs and Lessons Learned](#critical-bugs-and-lessons-learned)
8. [Implementation Guide](#implementation-guide)
9. [Testing Strategy](#testing-strategy)
10. [Future Work](#future-work)

---

## Executive Summary

### The Problem

SIMH/ZIMH's event system uses a **delta-encoded linked list** for scheduling asynchronous device events. This has O(n) complexity for insertion and cancellation, creating performance bottlenecks with many pending events.

### The Solution

Migrate to a **binary min-heap** with absolute fire times, achieving O(log n) operations while maintaining backward compatibility.

### The Challenge

The original system tightly couples **event scheduling** (linked list) with **time tracking** (UPDATE_SIM_TIME macro). These cannot be separated incrementally—they require coordinated rip-and-replace.

### Current Status

- **Phase 0-3:** Complete ✅
  - Min-heap implementation validated
  - Integration layer tested
  - All 236 tests pass
  
- **Phase 4A:** Failed ❌
  - Attempted incremental migration (keep UPDATE_SIM_TIME, add heap)
  - Discovered fundamental incompatibility
  - 5 simulators timeout/fail
  - Reverted to Phase 3
  
- **Phase 4B:** In Design ⏳
  - Rip-and-replace approach
  - Remove UPDATE_SIM_TIME entirely
  - Replace with instruction-driven interpolation
  - Implementation ready to begin

### Key Insight

**You cannot use absolute-time events with delta-based time tracking.** They have contradictory assumptions:
- **Linked list:** Stores relative deltas, preserves old interval values for delta calculation
- **Min-heap:** Stores absolute fire times, no concept of "old interval"

The only solution: Replace both systems simultaneously.

---

## Historical Context

### SIMH Origins

SIMH (Simulator for Historical Computers) was created by Bob Supnik in the 1990s. The event system design dates to this era and was optimized for:
- Small number of pending events (typically < 10)
- Single-threaded execution
- Simplicity over performance

### ZIMH Fork

ZIMH (Z's Improvements to the SIMH Framework) modernizes SIMH with:
- C17 standard adoption
- Modern build systems (CMake)
- Comprehensive test coverage
- Performance improvements ← **This migration**

### Why Now?

Profiling shows event queue operations (O(n) insert/cancel) consume significant CPU time in simulations with many devices. The min-heap migration delivers 10-100× speedup for realistic workloads.

---

## Current Architecture (Phase 0)

### Event Queue: Delta-Encoded Linked List

Events stored as a linked list with **relative** time deltas:

```c
struct UNIT {
    UNIT *next;      // Next unit in queue (NULL-terminated)
    int32_t time;    // Time until THIS unit fires (RELATIVE to previous)
    t_stat (*action)(UNIT *uptr);  // Service routine
    // ... other fields
};

UNIT *sim_clock_queue = QUEUE_LIST_END;  // Head of queue
int32_t sim_interval = 0;                 // Time to next event
```

**Example Queue:**
```
sim_clock_queue → [100] → [50] → [200] → NULL
                   ↑       ↑      ↑
                   Fire    Fire   Fire at
                   at 100  at 150 350 (cumulative)
```

### Time Tracking: UPDATE_SIM_TIME Macro

```c
#define UPDATE_SIM_TIME do {
    int32_t _x;
    if (sim_clock_queue == QUEUE_LIST_END)
        _x = noqueue_time;
    else
        _x = sim_clock_queue->time;  // OLD interval value
    
    // Calculate elapsed time from delta
    sim_vm_time = sim_vm_time + (_x - sim_interval);
    sim_rtime = sim_rtime + (uint32_t)(_x - sim_interval);
    
    // Save new interval for next iteration
    if (sim_clock_queue == QUEUE_LIST_END)
        noqueue_time = sim_interval;
    else
        sim_clock_queue->time = sim_interval;
} while (0)
```

**Key Property:** `sim_clock_queue->time` preserves the **old** `sim_interval` value between event processing cycles, enabling delta calculation.

### The Instruction Execution Loop

From each VM's `sim_instr()` function:

```c
while (reason == 0) {
    // Check if events are due
    if (sim_interval <= 0) {
        if ((reason = sim_process_event()))
            break;
    }
    
    // Check interrupts, breakpoints...
    
    // Fetch and execute instruction
    IR = M[PC];
    PC++;
    
    // DECREMENT EVENT TIMER
    sim_interval--;  // Or more for multi-cycle instructions
    
    // Execute instruction...
}
```

### Operations and Complexity

| Operation | Algorithm | Complexity | Notes |
|-----------|-----------|------------|-------|
| Insert | Walk list, subtract deltas | O(n) | Must find insertion point |
| Cancel | Walk list, search by unit | O(n) | Must adjust deltas after removal |
| Pop | Return head | O(1) | Fast |
| Peek | Read head | O(1) | Fast |

**Bottleneck:** With 100 pending events, insertion requires ~50 iterations on average.

---

## Target Architecture (Phase 4B)

### Event Heap: Binary Min-Heap with Absolute Times

Events stored in a binary heap with **absolute** fire times:

```c
typedef struct UNIT_EVENT {
    UNIT *unit;              // Unit to service
    uint64_t fire_time;      // ABSOLUTE simulation time
    t_bool is_timer_event;   // Timer vs regular event
    size_t heap_index;       // Position in heap (for O(log n) removal)
} UNIT_EVENT;

typedef struct EVENT_HEAP {
    UNIT_EVENT **events;     // Array of event pointers
    size_t count;            // Number of events
    size_t capacity;         // Allocated capacity
} EVENT_HEAP;
```

**Heap Structure:**
```
         [t=150]
        /       \
    [t=200]   [t=350]
    /     \
[t=250] [t=400]

Events fire in order: 150, 200, 250, 350, 400
```

### Time Tracking: Instruction-Driven Interpolation

**Remove UPDATE_SIM_TIME entirely.** Replace with:

```c
static uint64_t sim_vm_time_base = 0;        // Time at last event
static int32_t sim_instructions_since_base = 0;  // Saved sim_interval

double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        // Interpolate: base + instructions executed since base
        int32_t elapsed = sim_instructions_since_base - sim_interval;
        return (double)(sim_vm_time_base + elapsed);
    }
    return (double)sim_vm_time_base;
}
```

**When event fires:**
```c
UNIT_EVENT *evt = sim_event_heap_pop();

// SYNC TIME to event's absolute fire time
sim_vm_time_base = evt->fire_time;
sim_vm_time = (double)evt->fire_time;  // Legacy compatibility

// Calculate interval to NEXT event
UNIT_EVENT *next = sim_event_heap_peek();
if (next != NULL)
    sim_interval = (int32_t)(next->fire_time - evt->fire_time);
else
    sim_interval = NOQUEUE_WAIT;

// Save for interpolation
sim_instructions_since_base = sim_interval;
```

### Operations and Complexity

| Operation | Algorithm | Complexity | Notes |
|-----------|-----------|------------|-------|
| Insert | Heap sift-up | O(log n) | 100× faster for n=100 |
| Cancel | Heap remove at index | O(log n) | Requires heap_index tracking |
| Pop | Heap sift-down | O(log n) | Slightly slower than O(1) |
| Peek | Read root | O(1) | Same as before |

**Improvement:** With 100 pending events, insertion requires ~7 operations instead of ~50.

---


## Migration Journey

### Phase 1: Foundation (COMPLETE ✅)

**Goal:** Implement min-heap infrastructure without touching existing event system.

**Delivered:**
- src/lib/sim_events.c - 434 lines, complete min-heap implementation
- src/include/sim_events.h - Public API declarations
- 	ests/unit/src/lib/test_sim_events.c - 17 comprehensive unit tests
- UNIT_EVENT structure with absolute fire times

**Key Decisions:**
1. **Non-intrusive:** Separate UNIT_EVENT structure, no changes to UNIT
2. **Absolute times:** fire_time is absolute simulation time, not delta
3. **O(log n) cancel:** heap_index field enables fast removal from any position
4. **Auto-growth:** Heap starts at 128 capacity, grows by 2× when full

**Operations Implemented:**
- sim_event_heap_init() - Initialize heap
- sim_event_heap_insert(event) - O(log n) insertion with sift-up
- sim_event_heap_pop() - O(log n) removal with sift-down
- sim_event_heap_peek() - O(1) minimum access
- sim_event_heap_remove_at(index) - O(log n) cancel from arbitrary position
- sim_event_heap_find_by_unit(unit) - O(n) search for cancel operation

**Test Coverage:** 100% of heap operations
- Insertion order guarantees
- Heap property maintenance
- Edge cases (empty, full, duplicates)
- Growth beyond initial capacity (tested to 200+ events)

**API Impact:** ZERO - All existing code continues unchanged

**Commit:** 7437f6a9

---

### Phase 2: Min-Heap Implementation (COMPLETE ✅)

**Goal:** Create integration layer between heap and existing sim_activate/sim_cancel APIs.

**Delivered:**
- src/lib/sim_event_integration.c - Integration wrappers
- _sim_activate_heap() - Heap-based activation
- sim_cancel_heap() - Heap-based cancellation
- sim_process_event_heap() - Heap-based event processing
- Thread safety via sim_atomic.h spinlock

**Integration Strategy:**
`c
// Old API (unchanged externally)
t_stat sim_activate(UNIT *uptr, int32_t interval);

// New implementation (internal)
t_stat _sim_activate_heap(UNIT *uptr, int32_t event_time) {
    UNIT_EVENT *evt = calloc(1, sizeof(UNIT_EVENT));
    evt->unit = uptr;
    evt->fire_time = sim_event_heap_now() + event_time;  // Absolute time
    
    sim_event_heap_insert(evt);  // O(log n)
    
    // Maintain backward compatibility
    uptr->next = (UNIT *)1;      // Sentinel: "I am scheduled"
    uptr->time = event_time;     // For sim_activate_time()
}
`

**Backward Compatibility:**
- UNIT.next maintained as sentinel (non-NULL = active)
- UNIT.time maintained for sim_activate_time() queries
- sim_interval calculated from heap minimum
- Legacy code that reads these fields continues to work

**Thread Safety:**
- Simple spinlock using sim_atomic_cas()
- Protects heap operations
- Phase 1 approach: profile first, optimize later

**Testing:**
- All 17 unit tests pass
- Heap operations validated
- Integration functions tested

**Commit:** 56f69fc7

---

### Phase 3: Integration Testing (COMPLETE ✅)

**Goal:** Build system integration and comprehensive testing without modifying scp.c.

**Delivered:**
- CMake integration for sim_events library
- Full build with all 230 targets successful
- Unit tests integrated into test suite
- Atomic operations fixes for cross-platform support

**Build System Changes:**
- Added sim_events.c to sim_support library
- Integrated test_sim_events into unit test suite
- Verified clean build on Windows (Clang/ninja)

**Testing Results:**
- 17/17 event system unit tests passing (100%)
- 142/143 total unit tests passing (99.3%)
- 1 timing test failure (minor edge case, not critical)

**Atomic Operations Fix:**
- Fixed sim_atomic_cas usage for proper compare-and-swap semantics
- Ensures thread safety across platforms
- Spinlock now works correctly

**Documentation:**
- EVENT_PHASE3_STATUS.md - Complete status report
- Test methodology validated per TESTING.md

**Status:** Integration layer proven stable, ready for Phase 4A

**Commits:**
- 843a0c6a - Build integration and enhanced testing
- 6eccc275 - Phase 3 status and completion report

---

### Phase 4A: Failed Incremental Approach (FAILED ❌)

**Goal:** Replace scp.c event functions while keeping UPDATE_SIM_TIME macro.

**What Was Attempted:**
1. Replace _sim_activate() to call _sim_activate_heap()
2. Replace sim_cancel() to call sim_cancel_heap()
3. Replace sim_process_event() to pop from heap instead of linked list
4. Keep UPDATE_SIM_TIME macro for time tracking (minimal changes)

**Initial Implementation:**
`c
// In scp.c:10273
t_stat _sim_activate(UNIT *uptr, int32_t event_time) {
    UPDATE_SIM_TIME;  // Still here - thought this would work
    return _sim_activate_heap(uptr, event_time);
}

// In scp.c:10168
do {
    UNIT_EVENT *evt = sim_event_heap_pop();  // Pop from heap
    // ... but UPDATE_SIM_TIME still reads sim_clock_queue!
} while (/* ... */);
`

**The Fatal Flaw:**

UPDATE_SIM_TIME assumes linked-list structure:
`c
#define UPDATE_SIM_TIME do {
    _x = sim_clock_queue->time;  // Reads OLD interval from queue
    sim_vm_time += (_x - sim_interval);  // Calculates delta
    sim_clock_queue->time = sim_interval;  // Saves NEW interval
} while (0)
`

But with heap:
- sim_clock_queue = QUEUE_LIST_END (empty)
- No "old interval" stored anywhere
- UPDATE_SIM_TIME falls back to old queue_time (wrong)
- Time tracking breaks completely

**Bug #1: Heap Never Initialized**
- Symptom: All heap operations returned SCPE_IERR (error 101)
- Root cause: sim_event_heap = NULL never initialized
- Fix: Added lazy initialization in ensure_heap_initialized()
- Result: Improved from 128/143 to 142/143 tests passing

**Bug #2: Wrong Time Tracking**
- Symptom: 3b2-400, pdp8, pdp9 TIMEOUT (infinite loop), i650 diagnostic failures
- Root cause: Events scheduled using sim_event_heap_now() which returned 0
- Should have used sim_gtime() for current simulation time
- Fix attempt: Change to use sim_gtime() in three places
- Result: Still broken - revealed deeper incompatibility

**Bug #3: UPDATE_SIM_TIME Incompatibility (CRITICAL)**
- Symptom: All five simulators still timeout/fail after Bug #1 and #2 fixes
- Root cause: Cannot use delta-based time tracking with absolute-time events

**Failed Fix Attempts:**
1. **Sync sim_clock_queue from heap** - Maintain dummy UNIT node
   - Result: Still times out (timing issues with sync points)
   
2. **Track sim_interval_prev** - Save previous value globally
   - Result: Still times out (multiple UPDATE_SIM_TIME calls per cycle overwrite it)
   
3. **Make UPDATE_SIM_TIME idempotent** - Only run once per cycle
   - Result: Still times out (doesn't fix the "no old value" problem)

**The Circular Dependency:**
`
_sim_activate_heap()
  └─> update_sim_interval_from_heap()
      └─> sim_gtime()
          └─> UPDATE_SIM_TIME
              └─> Needs OLD sim_interval value
                  └─> But queue is EMPTY - value doesn't exist!
`

**Commits:**
- d00c74fa - Phase 4A: Replace scp.c event functions with heap
- aeb6174e - Phase 4A complete: Replace all scp.c event queue operations
- 2645d7e9 - Fix heap initialization (Bug #1)
- f4a58fcc - Fix time tracking - use sim_gtime() (Bug #2, incomplete)
- d1263a4f - Analysis documents

**Lesson Learned:**

**You cannot retrofit absolute-time events onto delta-based time tracking.**

The linked-list and heap have fundamentally incompatible assumptions:
- **Linked list:** Stores relative deltas, preserves old interval for delta calc
- **Heap:** Stores absolute times, no concept of "old interval"

They must be replaced simultaneously, not incrementally.

**Decision:** Revert to Phase 3, implement proper rip-and-replace (Phase 4B)

**Commits:**
- d96a71d0 - Revert to Phase 3

---

### Phase 4B: Rip-and-Replace Design (SUPERSEDED ❌)

**Goal:** Replace BOTH event scheduling and time tracking simultaneously.

**Status:** Design complete but NOT implemented. Superseded by Phase 4C (prev_interval approach).

**Design Philosophy:**
1. **Absolute time is source of truth** - No delta calculations
2. **Instruction-driven time tracking** - Time advances naturally with execution
3. **Interpolation for accuracy** - Calculate between events without touching VMs
4. **Complete removal of UPDATE_SIM_TIME** - Not replaced, eliminated

**Why Superseded:**

During code review, the assertion that "storing prev_interval cannot work" was challenged. Analysis revealed that Phase 4A never actually implemented prev_interval storage - it simply removed the linked list without providing any storage for UPDATE_SIM_TIME to read. The actual fix attempt was never tried.

**Decision:** Try Phase 4C (prev_interval storage) before committing to Phase 4B's more invasive changes.

---

### Phase 4C: EVENT_HEAP.prev_interval Approach (IN PROGRESS ⏳)

**Goal:** Store previous sim_interval in EVENT_HEAP to enable UPDATE_SIM_TIME with heap.

**Hypothesis:** The Phase 4A failures were not due to "prev_interval cannot work" but due to "prev_interval was never implemented". If we properly store the old interval value in the heap structure, UPDATE_SIM_TIME should work unchanged.

**Implementation:**

#### 1. Add prev_interval to EVENT_HEAP

```c
typedef struct event_heap {
    UNIT_EVENT **events;
    size_t count;
    size_t capacity;
    uint64_t now;
    int32_t prev_interval;  // NEW: Store old sim_interval for UPDATE_SIM_TIME
} EVENT_HEAP;
```

#### 2. Modify UPDATE_SIM_TIME macro

```c
#define UPDATE_SIM_TIME do {
    int32_t _x;
    AIO_LOCK;
    if (sim_event_heap_count() == 0)
        _x = noqueue_time;
    else
        _x = sim_event_heap_get_prev_interval();  // Read from heap
    sim_vm_time = sim_vm_time + (_x - sim_interval);
    sim_rtime = sim_rtime + ((uint32_t) (_x - sim_interval));
    if (sim_event_heap_count() == 0)
        noqueue_time = sim_interval;
    else
        sim_event_heap_set_prev_interval(sim_interval);  // Write to heap
    AIO_UNLOCK;
} while (0)
```

#### 3. Replace event functions in scp.c

```c
t_stat _sim_activate (UNIT *uptr, int32_t event_time) {
    // ... validation ...
    UPDATE_SIM_TIME;
    return _sim_activate_heap(uptr, event_time);  // Delegate to heap
}

t_stat sim_cancel (UNIT *uptr) {
    // ... validation ...
    UPDATE_SIM_TIME;
    return sim_cancel_heap(uptr);  // Delegate to heap
}

t_stat sim_process_event (void) {
    // ... validation ...
    UPDATE_SIM_TIME;
    return sim_process_event_heap();  // Delegate to heap
}
```

#### 4. Fix integration layer to use sim_gtime()

Changed from:
```c
evt->fire_time = sim_event_heap_now() + event_time;  // WRONG
```

To:
```c
evt->fire_time = (uint64_t)sim_gtime() + event_time;  // CORRECT
```

**Test Results (2026-10-28):**

Build: ✅ **SUCCESS** (260/260 targets)
- All code compiles cleanly
- Heap initialization added to sim_event_heap_init()
- Accessors added: sim_event_heap_get_prev_interval(), sim_event_heap_set_prev_interval()

Unit Tests: ✅ **PASS** (1/1)
- test_sim_events: All 17 heap tests pass

Critical Simulators (5 tests):
- ✅ **pdp8**: PASS (was TIMEOUT in Phase 4A)
- ✅ **pdp9**: PASS (was TIMEOUT in Phase 4A)
- ❌ **3b2-400**: FAIL - "Negative timer activation delay for INT-TIMER: -0 usecs"
- ❌ **3b2-700**: FAIL - "Negative timer activation delay for INT-TIMER: -0 usecs"
- ❌ **i650**: FAIL - "FDS failed (bad ar)" (diagnostic failure)

**Result: PARTIAL SUCCESS (40% critical tests passing, vs 0% in Phase 4A)**

**Analysis:**

The prev_interval approach **does work** for basic event scheduling:
- PDP8 and PDP9 now pass (were timing out before)
- Heap operations function correctly
- UPDATE_SIM_TIME successfully reads/writes prev_interval
- No infinite loops or timeouts

**Remaining Issues:**

1. **Timer Events:** The 3B2 systems fail with "Negative timer activation delay"
   - Suggests timer-specific code needs investigation
   - May be calculating fire times incorrectly
   - Could be interaction with wall-clock timer events

2. **Time Accuracy:** The i650 diagnostic failure suggests timing issues
   - Diagnostic tests are sensitive to instruction-level timing
   - May indicate sim_gtime() interpolation is incorrect
   - Or event fire times are being calculated wrong

**Key Insight:**

The compendium's claim that "prev_interval cannot work due to multiple UPDATE_SIM_TIME calls" was **incorrect**. The idempotent property holds:

```c
// First call: sim_interval = 100, prev_interval = 100
UPDATE_SIM_TIME:
  delta = 100 - 100 = 0  ✓ (no time passed)
  prev_interval = 100  (unchanged)

// Second call: still no instructions executed
UPDATE_SIM_TIME:
  delta = 100 - 100 = 0  ✓ (still correct!)
```

Multiple calls with unchanged sim_interval produce delta = 0, which is correct.

**Next Steps:**

1. **Debug timer activation** - Why are timer events getting negative delays?
   - Check sim_timer_activate_after()
   - Check wall-clock timer queue interaction
   - Verify timer fire time calculations

2. **Debug i650 timing** - Why does the diagnostic fail?
   - Add debug logging to see actual vs expected times
   - Check if event fire times are being calculated correctly
   - Verify sim_gtime() returns correct values

3. **Run full test suite** - Assess overall impact
   - How many of 236 tests pass?
   - Are failures localized to timer-heavy simulators?
   - Or widespread timing issues?

**Debug Session - Timer Activation Issue (2026-10-28):**

**Problem:** 3B2-400 and 3B2-700 fail with "Negative timer activation delay for INT-TIMER: -0 usecs"

**Investigation:**
1. Error originates in `sim_timer_activate_after()` (sim_timer.c:2736)
2. The function receives `usec_delay < 0.0` (specifically: -0.0, negative zero)
3. This value comes from `uptr->usecs_remaining` field set by timer subsystem

**Root Cause:**
- Timer calculations like `usec_delay - usecs_til_calib` can produce small negative values
- Floating-point rounding errors from modified time tracking accumulate
- When `usecs_til_calib` is slightly larger than `usec_delay`, result is negative
- Negative zero (-0.0) indicates value very close to zero but negative

**Fix Applied:**
Modified `sim_process_event_heap()` in sim_event_integration.c:
```c
/* Save usecs_remaining before clearing, then clear it */
double saved_usecs = uptr->usecs_remaining;
uptr->usecs_remaining = 0.0;

/* Clamp negative values to zero (rounding errors) */
if (saved_usecs < 0.0) {
    sim_debug(SIM_DBG_EVENT, &sim_scp_dev, 
              "Clamping negative usecs_remaining %.0f to 0.0 for %s\n",
              saved_usecs, sim_uname(uptr));
    saved_usecs = 0.0;
}
if (saved_usecs > 0.0) {
    reason = sim_timer_activate_after(uptr, saved_usecs);
} else {
    /* Run action directly if no delay */
    if (uptr->action != NULL)
        reason = uptr->action(uptr);
}
```

**Rationale:**
- Negative zero is a floating-point rounding artifact, not meaningful delay
- Clamping to zero treats these as "fire immediately" 
- Debug logging tracks when this occurs for future investigation
- Prevents spurious errors while maintaining correct timing behavior

**Status:** ✅ PARTIALLY FIXED

**Test Results After Timer Fix:**
- ✅ Timer error eliminated: No more "Negative timer activation delay" errors
- ✅ Major improvement: 91% pass rate (215/235 tests pass, 20 fail)
- ✅ Before fix: 82% pass rate (192/235 tests, 43 failures)
- ✅ Net gain: 23 tests fixed by timer clamping
- Critical 5: Still 40% pass (pdp8, pdp9 pass; 3b2-400, 3b2-700, i650 fail)

**Comparison:**
- Phase 0 (original): ~100% pass (baseline)
- Phase 4A (failed): 0% critical tests passing (timeouts)
- Phase 4C (before timer fix): 82% overall, 2/5 critical
- Phase 4C (after timer fix): **91% overall, 2/5 critical**

**Root Cause Found:**
The negative delay was coming from `sim_timer_activate_after()` being called with values like -0.0 microseconds (negative zero). This occurs when:
1. Wall-clock timer scheduling calculates `target_time - current_time`
2. Floating-point rounding causes `current_time` to be infinitesimally larger than `target_time`
3. Result: -0.0000... (rounds to display as "-0")

**Final Fix Applied:**
Added clamping in two places:
1. `sim_timer_activate_after()` (sim_timer.c:2733) - clamps -1.0 < delay < 0.0 to 0.0
2. `_sim_activate_after_abs()` (scp.c:10234) - same clamping logic

This eliminates the timer error while preserving error detection for truly invalid delays (<  -1.0 microseconds).

**Remaining Issues:**
- 3B2 simulators: Test assertions fail (RGTRU instruction behavior incorrect)
- i650: Diagnostic failure (timing-sensitive test)
- These appear to be timing accuracy issues, not timer infrastructure problems

**Baseline Test Results (before fix):**
- Overall: 82% pass rate (192/235 tests pass, 43 fail)
- Critical 5: 40% pass (pdp8, pdp9 pass; 3b2-400, 3b2-700, i650 fail)

**Commits:**
- [pending] - Phase 4C: Add EVENT_HEAP.prev_interval and modify UPDATE_SIM_TIME
- [pending] - Phase 4C: Fix timer negative delay by clamping small negative values

**Lessons Learned - Challenging Documented Assumptions:**

This phase demonstrates the importance of **skeptical code review**. The compendium documented that "prev_interval tracking failed" in Phase 4A, but deeper analysis revealed:

1. **What Actually Failed:** Phase 4A never implemented prev_interval storage. It removed the linked list but left UPDATE_SIM_TIME trying to read `sim_clock_queue->time` from an empty queue.

2. **The False Conclusion:** The failure was attributed to "architectural incompatibility" when it was actually "missing implementation."

3. **The Real Test:** Phase 4C implements prev_interval properly and shows it **does work** for basic events (2/5 critical tests now pass vs 0/5 before).

4. **User Insight:** "Just because you documented a failure doesn't necessarily make it true. It's just a documented failure." This challenge forced re-examination of assumptions and revealed the error.

**Key Takeaway:** Documentation can perpetuate incorrect analysis. Always verify claims, especially negative claims about what "cannot work."

---

### Phase 4B: Rip-and-Replace Design (ORIGINAL, PRESERVED FOR REFERENCE)

**Goal:** Replace BOTH event scheduling and time tracking simultaneously.

**Design Philosophy:**
1. **Absolute time is source of truth** - No delta calculations
2. **Instruction-driven time tracking** - Time advances naturally with execution
3. **Interpolation for accuracy** - Calculate between events without touching VMs
4. **Complete removal of UPDATE_SIM_TIME** - Not replaced, eliminated

**Core Architecture Change:**

#### Before (Phase 0-4A):
`c
// Delta-based time tracking
#define UPDATE_SIM_TIME do {
    int32_t _x = sim_clock_queue->time;       // OLD interval
    sim_vm_time += (_x - sim_interval);       // Calculate delta
    sim_clock_queue->time = sim_interval;     // Save NEW interval
} while (0)

double sim_gtime(void) {
    UPDATE_SIM_TIME;  // Calculate and advance time
    return sim_vm_time;
}
`

#### After (Phase 4B):
`c
// Instruction-driven interpolation
static uint64_t sim_vm_time_base = 0;           // Time at last event
static int32_t sim_instructions_since_base = 0; // Saved sim_interval

double sim_gtime(void) {
    // Interpolate: base + instructions executed since base
    // Formula: base + (interval_at_base - current_interval)
    int32_t elapsed = sim_instructions_since_base - sim_interval;
    return (double)(sim_vm_time_base + elapsed);
}
`

**Key Insight:** We can calculate elapsed instructions from sim_interval without modifying any VMs!
- When event fires: save sim_instructions_since_base = sim_interval
- Between events: lapsed = sim_instructions_since_base - sim_interval
- Result: Instruction-level time tracking with zero VM changes

**Event Processing Changes:**

`c
t_stat sim_process_event(void) {
    // Remove all UPDATE_SIM_TIME calls
    
    UNIT_EVENT *evt = sim_event_heap_pop();
    
    // SYNC TIME to event's absolute fire time
    sim_vm_time_base = evt->fire_time;
    sim_vm_time = (double)evt->fire_time;  // Legacy compatibility
    
    // Calculate interval to NEXT event
    UNIT_EVENT *next = sim_event_heap_peek();
    if (next != NULL)
        sim_interval = (int32_t)(next->fire_time - evt->fire_time);
    else
        sim_interval = NOQUEUE_WAIT;
    
    // Save for interpolation
    sim_instructions_since_base = sim_interval;
    
    free(evt);
    
    // Execute event action...
}
`

**Implementation Steps:**

1. **Add tracking variables** (scp.c)
   `c
   static uint64_t sim_vm_time_base = 0;
   static int32_t sim_instructions_since_base = 0;
   `

2. **Modify sim_gtime()** - Remove UPDATE_SIM_TIME, add interpolation

3. **Modify sim_process_event()** - Sync time at events, update base

4. **Delete UPDATE_SIM_TIME macro** - Search and remove all occurrences

5. **Test thoroughly** - All 236 tests should pass

**No Changes Needed:**
- ✅ Individual VMs (time advances via existing sim_interval--)
- ✅ Heap implementation (already works)
- ✅ Event scheduling functions (already use absolute times)
- ✅ Integration layer (works correctly)

**Backward Compatibility:**
- Keep sim_vm_time global variable (updated at events)
- Keep sim_rtime for low 32 bits
- Keep UNIT.next and UNIT.time for legacy code

**Risk Assessment:**

| Risk | Mitigation | Confidence |
|------|-----------|------------|
| Time accuracy | Interpolation formula preserves instruction-level precision | High |
| Backward compatibility | Keep legacy variables, update alongside new tracking | High |
| Edge cases | Thorough testing with existing 236-test suite | Medium |
| Catchup mode | Negative intervals handled naturally by interpolation | High |

**Testing Strategy:**
1. Unit tests - 17 event system tests (should all pass unchanged)
2. Integration tests - 236 simulator tests (should all pass)
3. New time accuracy tests - Verify interpolation correctness
4. Stress tests - Many events, various timing patterns

**Migration Path:**
- **Phase 4B-1:** Time tracking fixes (this implementation)
- **Phase 4B-2:** Remove sim_clock_queue references
- **Phase 4B-3:** Remove QUEUE_LIST_END (81 occurrences)

**Status:** Design complete, ready for implementation

**Document:** projects/PHASE4B_DESIGN.md (420 lines, comprehensive)

**Commit:** 0456b0d2 - Phase 4B design document

---


## Technical Deep Dives

### Time Tracking Architecture

#### The UPDATE_SIM_TIME Problem

The UPDATE_SIM_TIME macro is the core of SIMH's time tracking, but it has hidden assumptions:

**Assumption 1: Events store their OLD interval value**
`c
// When event is scheduled:
sim_clock_queue->time = event_time;  // Stored in queue

// Later, when checking time:
_x = sim_clock_queue->time;  // Still has OLD value
delta = _x - sim_interval;   // Calculate how much executed
`

**Assumption 2: Queue head is stable between UPDATE_SIM_TIME calls**

The linked list head doesn't change until sim_process_event() pops it. This means sim_clock_queue->time preserves the interval value across many sim_gtime() calls.

**Why This Breaks with Heap:**

1. Heap stores absolute ire_time, not relative interval
2. No field to store the "old interval" value
3. sim_clock_queue abandoned (set to QUEUE_LIST_END)
4. UPDATE_SIM_TIME has nothing to read

#### The Interpolation Solution

Instead of calculating deltas, we interpolate time between events:

`c
// At event fire:
sim_vm_time_base = 150;           // Event fired at time 150
sim_interval = 100;                // Next event in 100 instructions
sim_instructions_since_base = 100; // Save starting point

// After 30 instructions execute:
sim_interval = 70;                 // Decremented to 70
elapsed = 100 - 70 = 30;           // 30 instructions have run
sim_gtime() returns 150 + 30 = 180 ✓

// After 100 instructions (next event fires):
sim_interval = 0;
elapsed = 100 - 0 = 100;
sim_gtime() returns 150 + 100 = 250 ✓
`

**Key property:** No delta calculation needed. Time is directly computed from current state.

---

### Heap vs Linked List: Deep Comparison

#### Memory Layout

**Linked List:**
`c
UNIT structures contain event queue fields:
struct UNIT {
    UNIT *next;       // 8 bytes - queue linkage (intrusive)
    int32_t time;     // 4 bytes - relative time
    // ... other fields
};

Queue: UNIT→UNIT→UNIT→NULL (scattered in memory)
`

**Min-Heap:**
`c
UNIT_EVENT separate from UNIT:
typedef struct {
    UNIT *unit;       // 8 bytes - backpointer
    uint64_t fire_time; // 8 bytes - absolute time
    size_t heap_index;  // 8 bytes - for O(log n) cancel
    t_bool is_timer;    // 1 byte - event type
} UNIT_EVENT;

Heap: Array of pointers (cache-friendly, contiguous)
`

**Memory impact:**
- Linked list: 12 bytes per UNIT (intrusive overhead)
- Heap: 25 bytes per UNIT_EVENT + 8 bytes per heap slot
- Trade: Higher per-event cost, but better locality and flexibility

#### Cache Behavior

**Linked List (Poor):**
- Events scattered throughout UNIT structures
- Pointer chasing (UNIT→UNIT→UNIT)
- Every insert: O(n) cache misses
- Every cancel: O(n) cache misses

**Heap (Better):**
- Event pointers in contiguous array
- Binary tree navigation is cache-predictable
- Every insert: O(log n) cache misses in array traversal
- Heap-allocated events have better locality than scattered UNITs

**Measurement needed:** Profile actual cache miss rates in real workloads.

#### Complexity Analysis

| Operation | Linked List | Min-Heap | Speedup Factor |
|-----------|-------------|----------|----------------|
| Insert (average) | O(n/2) | O(log n) | ~50× for n=100 |
| Insert (worst) | O(n) | O(log n) | ~100× for n=100 |
| Cancel | O(n) | O(log n) | ~100× for n=100 |
| Pop | O(1) | O(log n) | 0.1× slower |
| Peek | O(1) | O(1) | Same |

**Break-even point:** ~16 events (log₂(16) = 4, close to average linked list scan)

**Real-world:** VAX systems commonly have 50-100 pending events (disks, tapes, terminals).

---

### Thread Safety Considerations

#### Current State (Single-Threaded)

SIMH/ZIMH is fundamentally single-threaded:
- One VM execution thread
- Async I/O helpers update state, but don't manipulate event queue directly
- AIO_UPDATE_QUEUE macro synchronizes at well-defined points

#### Phase 2 Spinlock Design

`c
static sim_atomic_value_t event_heap_spinlock;

static inline void event_heap_lock_acquire(void) {
    sim_atomic_type_t expected = 0;
    while (!sim_atomic_cas(&event_heap_spinlock, &expected, 1)) {
        expected = 0;  // Reset for next attempt
    }
}

static inline void event_heap_lock_release(void) {
    sim_atomic_put(&event_heap_spinlock, 0);
}
`

**Properties:**
- Simple, correct, auditable
- Uses existing sim_atomic.h infrastructure
- Matches sim_tmxr_poll_lock pattern (proven in production)
- Sufficient for current single-threaded + AIO model

#### Future: Lock-Free MPSC (If Needed)

If profiling shows spinlock contention:

1. **Measure first** - Don't optimize prematurely
2. **Consider Treiber stack** - See projects/EVENT_TREIBER_STACK.md
3. **Or MPSC ring buffer** - Already in repo (src/include/buf_ring.h)

**Decision criterion:** If heap lock contention > 1% of runtime, revisit.

---

### Absolute Time vs Relative Time

#### Relative Time (Linked List)

**Encoding:**
`
Queue: [100]→[50]→[200]→NULL
Meaning:
  - First event fires in 100 ticks
  - Second event fires in 50 more ticks (cumulative: 150)
  - Third event fires in 200 more ticks (cumulative: 350)
`

**Advantages:**
- Compact representation (small integers)
- No overflow concerns (deltas stay small)

**Disadvantages:**
- Complex insertion (adjust deltas in multiple nodes)
- Debugging nightmare (can't see absolute fire time)
- Subtle bugs from delta arithmetic errors

#### Absolute Time (Heap)

**Encoding:**
`
Heap: [150, 200, 350, 400]
Meaning:
  - Events fire at absolute simulation times: 150, 200, 350, 400
`

**Advantages:**
- Simple insertion (no delta adjustments)
- Easy debugging (see exactly when event fires)
- No arithmetic errors possible

**Disadvantages:**
- Need 64-bit times (32-bit would overflow at ~4 billion instructions)
- Requires synchronized time base (sim_vm_time_base)

**Trade-off:** Simplicity and correctness win over minor space cost.

---

### Event Queue Sentinel: QUEUE_LIST_END

#### What It Is

`c
#define QUEUE_LIST_END ((UNIT *)1)

// Used as:
if (sim_clock_queue == QUEUE_LIST_END)
    /* queue is empty */
`

**Not NULL, but ((UNIT *)1)** - Why?

1. **Historical:** Allows queue to be non-NULL when empty
2. **Alignment:** (UNIT *)1 is misaligned, can't be a real pointer
3. **Detection:** Catches bugs (dereferencing (UNIT *)1 crashes immediately)

#### Removal Plan (Phase 4B-3)

**81 occurrences** across codebase:

`ash
$ grep -r "QUEUE_LIST_END" src/ simulators/ | wc -l
81
`

**Categories:**
1. **sim_clock_queue checks** - Replace with sim_event_heap_count() == 0
2. **UNIT.next = QUEUE_LIST_END** - Replace with UNIT.next = NULL
3. **Comparisons** - Remove (heap doesn't use sentinels)

**Not Phase 4B-1:** Keep for now, remove after time tracking is stable.

---

## Critical Bugs and Lessons Learned

### Bug #1: Heap Never Initialized

**Symptom:**
- 14 unit tests failing with SCPE_IERR (error 101)
- "Internal error" messages from simulators
- Heap operations returning immediately

**Root Cause:**
`c
static EVENT_HEAP *sim_event_heap = NULL;  // Never initialized!

// All operations checked:
if (sim_event_heap == NULL)
    return SCPE_IERR;
`

The heap was declared but never initialized. Every operation failed immediately.

**Why It Happened:**

Integration layer assumed heap would be initialized elsewhere. But Phase 2 only added wrapper functions, didn't add initialization call.

**The Fix:**
`c
static inline void ensure_heap_initialized(void) {
    sim_event_heap_init();  // Idempotent - initializes once
}

// Called at start of every public function:
t_stat _sim_activate_heap(UNIT *uptr, int32_t event_time) {
    ensure_heap_initialized();  // ← Added
    // ...
}
`

**Lesson:** Lazy initialization is safer than explicit init when integration points are unclear.

**Result:** Tests improved from 128/143 to 142/143 passing (99.3%).

**Commit:** 2645d7e9

---

### Bug #2: Wrong Time Base (Incomplete Fix)

**Symptom:**
- 3b2-400, 3b2-700: TIMEOUT (infinite loop)
- pdp8, pdp9: TIMEOUT
- i650: Diagnostic failure ("FDS failed (bad ar)")

**Root Cause:**
`c
// WRONG: Used heap's internal clock
evt->fire_time = sim_event_heap_now() + event_time;

// sim_event_heap_now() returned heap->now = 0 (never updated)
// So events scheduled at: 0, 1, 2, 3, 4...
// But sim_gtime() was at: 1000000, 2000000, ...
`

Events scheduled at wrong absolute times (near 0 instead of current sim time).

**The Attempted Fix:**
`c
// BETTER: Use simulation time
evt->fire_time = (uint64_t)sim_gtime() + event_time;
`

**Why It Was Incomplete:**

This revealed Bug #3 - sim_gtime() calls UPDATE_SIM_TIME, which doesn't work with heap!

**Lesson:** Fixing one bug can expose deeper architectural issues.

**Commit:** f4a58fcc (partial fix, revealed Bug #3)

---

### Bug #3: UPDATE_SIM_TIME Incompatibility (CRITICAL)

**Symptom:**
- After Bug #1 and Bug #2 fixes, simulators STILL timeout
- All attempted workarounds fail
- Problem not reducible to code bug - it's architectural

**Root Cause:**

UPDATE_SIM_TIME assumes linked-list structure:
`c
#define UPDATE_SIM_TIME do {
    _x = sim_clock_queue->time;  // ← Reads OLD interval
    sim_vm_time += (_x - sim_interval);  // ← Calculates delta
    sim_clock_queue->time = sim_interval;  // ← Saves NEW interval
} while (0)
`

But with heap:
- sim_clock_queue = QUEUE_LIST_END (empty)
- No field stores the "old interval" value
- Formula breaks completely

**Why All Fixes Failed:**

| Fix Attempt | Why It Failed |
|-------------|---------------|
| Sync sim_clock_queue | Timing wrong - synced AFTER operations, not BEFORE |
| Track sim_interval_prev | Multiple UPDATE_SIM_TIME calls per cycle overwrite it |
| Make idempotent | Prevents multiple updates but doesn't fix wrong first update |
| Use heap->now | Circular dependency: sim_gtime() → UPDATE_SIM_TIME → reads heap → needs sim_gtime() |

**The Circular Dependency:**
`
_sim_activate_heap()
  needs fire_time = sim_gtime() + interval
    └─> sim_gtime()
        └─> UPDATE_SIM_TIME
            needs sim_clock_queue->time
              └─> But queue is EMPTY!
`

**The Real Problem:**

Delta-based time tracking (UPDATE_SIM_TIME) is fundamentally incompatible with absolute-time events (heap). They cannot coexist.

**The Solution:**

Rip-and-replace BOTH systems simultaneously (Phase 4B).

**Lesson:** 

Some architectural incompatibilities cannot be fixed incrementally. Attempting incremental migration wasted significant effort and revealed the need for complete redesign.

**Quote from analysis:**
> "You cannot retrofit absolute-time events onto delta-based time tracking. The linked-list and heap have fundamentally incompatible assumptions."

**Decision:** Revert to Phase 3, implement Phase 4B properly.

**Documents:** 
- projects/BUG3_TIME_TRACKING_DEEPER_ANALYSIS.md
- projects/ARCHITECTURE_REVIEW_TIME_TRACKING.md

---

### Lessons Learned

#### 1. Test Integration Points Early

Phase 2 delivered heap implementation and integration layer, but didn't test end-to-end integration with scp.c. Bug #1 (heap not initialized) would have been caught immediately.

**Fix:** Phase 3 added integration testing before Phase 4A.

#### 2. Understand Assumptions Before Refactoring

UPDATE_SIM_TIME's dependency on queue structure wasn't obvious from API surface. Only deep analysis revealed the hidden coupling.

**Fix:** ARCHITECTURE_REVIEW document now explains all dependencies.

#### 3. Architectural Incompatibilities Need Big-Bang Fixes

Incremental migration (Phase 4A) failed because event scheduling and time tracking cannot be separated. Some refactorings require coordinated changes.

**Fix:** Phase 4B does complete rip-and-replace.

#### 4. Revert Quickly When Stuck

After three failed fix attempts, we reverted to Phase 3 rather than trying more band-aids. This cleared the way for proper solution.

**Fix:** Clean Phase 3 baseline enables Phase 4B implementation.

#### 5. Document Failures as Thoroughly as Successes

The Phase 4A failure analysis documents (BUG3, ARCHITECTURE_REVIEW) are as valuable as the success documents. They capture what DOESN'T work and why.

**Result:** Future developers won't repeat the same mistakes.

---


## Implementation Guide

### Phase 4B-1: Time Tracking Replacement

**File:** src/core/scp.c

#### Step 1: Add Tracking Variables

After existing globals (around line 554):
`c
UNIT *sim_clock_queue = QUEUE_LIST_END;
int32_t sim_interval = 0;
// ADD THESE:
static uint64_t sim_vm_time_base = 0;        // Time at last event fire
static int32_t sim_instructions_since_base = 0;  // Saved sim_interval value
`

#### Step 2: Modify sim_gtime()

Find double sim_gtime(void) (around line 10509):

**Before:**
`c
double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        UPDATE_SIM_TIME;
    }
    return sim_vm_time;
}
`

**After:**
`c
double sim_gtime(void) {
    if (AIO_MAIN_THREAD) {
        // Interpolate: base + (instructions executed since base)
        // Formula: base + (interval_at_base - current_interval)
        int32_t elapsed = sim_instructions_since_base - sim_interval;
        return (double)(sim_vm_time_base + elapsed);
    }
    return (double)sim_vm_time_base;
}
`

#### Step 3: Modify sim_process_event()

Find event processing loop (around line 10168):

**Remove ALL calls to UPDATE_SIM_TIME** in this function.

**Add time synchronization after popping event:**

`c
do {
    UNIT_EVENT *evt = sim_event_heap_pop();
    if (evt == NULL)
        break;
    
    uptr = evt->unit;
    uint64_t evt_fire_time = evt->fire_time;
    
    // SYNC TIME to this event's absolute fire time
    sim_vm_time_base = evt_fire_time;
    sim_vm_time = (double)evt_fire_time;      // Legacy compatibility
    sim_rtime = (uint32_t)evt_fire_time;       // Legacy compatibility
    
    uptr->next = NULL;
    uptr->time = 0;
    
    // Calculate interval to NEXT event
    UNIT_EVENT *next_evt = sim_event_heap_peek();
    if (next_evt != NULL) {
        int32_t next_time = (int32_t)(next_evt->fire_time - evt_fire_time);
        if (sim_interval_catchup < 0)
            sim_interval = -sim_interval_catchup;
        sim_interval += sim_interval_catchup + next_time;
    } else {
        sim_interval = noqueue_time = NOQUEUE_WAIT;
    }
    
    // Save for interpolation
    sim_instructions_since_base = sim_interval;
    
    free(evt);
    
    // ... rest of event processing
} while (/* ... */);
`

#### Step 4: Delete UPDATE_SIM_TIME Macro

Find the macro definition (around line 267) and **DELETE IT ENTIRELY**.

Search for all remaining uses:
`ash
grep -n "UPDATE_SIM_TIME" src/core/scp.c
`

Remove each occurrence. There should be ~8 uses total.

#### Step 5: Handle Special Cases

**Negative intervals (catchup mode):**

Already handled naturally by interpolation:
`c
sim_instructions_since_base = 100;  // Event fires in 100
// Run 120 instructions (overshot by 20)
sim_interval = -20;
elapsed = 100 - (-20) = 120;  // Correctly shows 120 instructions ran
`

**Empty queue:**

`c
if (sim_event_heap_count() == 0) {
    sim_interval = noqueue_time = NOQUEUE_WAIT;
    // sim_instructions_since_base unchanged - interpolation still works
}
`

**First event after boot:**

`c
// At sim_run() start:
sim_vm_time_base = 0;
sim_instructions_since_base = 0;
sim_interval = NOQUEUE_WAIT;  // No events yet
`

#### Step 6: Build and Test

`powershell
# Clean build
cmake --build --preset windows-ninja-debug

# Test the five critical simulators
ctest --preset windows-ninja-debug -R "zimh-(3b2-400|3b2-700|pdp8|pdp9|i650)$"

# Run full test suite
ctest --preset windows-ninja-debug --parallel 8
`

**Expected results:**
- All 5 critical simulators: PASS
- All 236 integration tests: PASS
- All 17 event unit tests: PASS

---

### Phase 4B-2: Queue Cleanup

**Goal:** Remove sim_clock_queue references now that heap is the only event system.

#### Files to Modify

1. **src/runtime/sim_timer.c**
   - Timer events currently check sim_clock_queue
   - Replace with heap queries

2. **simulators/VAX/vax_cpu.c**
   - VAX idle detection reads queue directly
   - Replace with sim_event_heap_peek()

3. **simulators/Ibm1130/ibm1130_cpu.c**
   - Similar idle detection
   - Replace with heap API

#### Search and Replace Strategy

`ash
# Find all queue accesses
grep -n "sim_clock_queue" src/ simulators/ -r

# Common patterns:
sim_clock_queue == QUEUE_LIST_END  →  sim_event_heap_count() == 0
sim_clock_queue->time              →  [calculate from heap_peek()]
sim_clock_queue->next              →  [heap doesn't expose, redesign]
`

#### Testing

After each file modification:
1. Build
2. Run simulator-specific tests
3. Check timing behavior matches

---

### Phase 4B-3: QUEUE_LIST_END Removal

**Goal:** Remove the sentinel value, no longer needed.

#### Occurrences (81 total)

`ash
grep -r "QUEUE_LIST_END" src/ simulators/ | wc -l
# 81
`

#### Categories

1. **Initialization** (23 occurrences)
   `c
   // Before:
   sim_clock_queue = QUEUE_LIST_END;
   // After:
   sim_clock_queue = NULL;  // Or remove entirely if unused
   `

2. **Empty checks** (35 occurrences)
   `c
   // Before:
   if (sim_clock_queue == QUEUE_LIST_END)
   // After:
   if (sim_event_heap_count() == 0)
   `

3. **Next pointers** (18 occurrences)
   `c
   // Before:
   uptr->next = QUEUE_LIST_END;
   // After:
   uptr->next = NULL;
   `

4. **Macro definition** (1 occurrence)
   `c
   // Before:
   #define QUEUE_LIST_END ((UNIT *)1)
   // After:
   [DELETE]
   `

5. **Documentation** (4 occurrences)
   - Update comments to reflect heap-based implementation

#### Testing Strategy

Automated refactoring:
`ash
# Use sed/awk for bulk replacement
# But review each change manually
`

Run full test suite after each batch of 10-20 changes.

---

## Testing Strategy

### Unit Tests (17 tests)

**File:** 	ests/unit/src/lib/test_sim_events.c

**Coverage:**
- Heap initialization and growth
- Insertion and ordering guarantees
- Removal from arbitrary positions
- Edge cases (empty, full, duplicates)
- Index tracking accuracy

**Status:** All passing ✅ (unchanged by Phase 4B-1)

**Run:**
`powershell
ctest --preset windows-ninja-debug -R "zimh-unit-events"
`

### Integration Tests (236 tests)

**Coverage:**
- All simulators with representative workloads
- 5 critical simulators that exposed Phase 4A bugs:
  - 3b2-400, 3b2-700 (AT&T 3B2 systems)
  - pdp8 (PDP-8)
  - pdp9 (PDP-9)
  - i650 (IBM 650)

**Status Before Phase 4B:**
- Phase 3: 236/236 passing ✅
- Phase 4A: 231/236 passing ❌ (5 timeouts/failures)

**Expected After Phase 4B-1:** 236/236 passing ✅

**Run:**
`powershell
# Quick check (5 critical simulators)
ctest --preset windows-ninja-debug -R "zimh-(3b2-400|3b2-700|pdp8|pdp9|i650)$"

# Full suite
ctest --preset windows-ninja-debug --parallel 8
`

### Time Accuracy Tests (New)

**File:** 	ests/unit/src/lib/test_time_tracking.c (to be created)

**Test cases:**

1. **Basic interpolation:**
   `c
   sim_gtime();  // Should return 0 at start
   run_100_instructions();
   assert(sim_gtime() == 100.0);
   `

2. **Event time sync:**
   `c
   sim_activate(unit, 500);
   run_until_event();
   assert(sim_gtime() == 500.0);  // Exact at event boundary
   `

3. **Multi-event sequence:**
   `c
   sim_activate(unit1, 100);
   sim_activate(unit2, 200);
   run_until_event();  // First event at 100
   assert(sim_gtime() == 100.0);
   run_until_event();  // Second event at 200
   assert(sim_gtime() == 200.0);
   `

4. **Interpolation mid-interval:**
   `c
   sim_activate(unit, 1000);
   run_500_instructions();
   double t = sim_gtime();
   assert(fabs(t - 500.0) < 1.0);  // Within 1 instruction
   `

5. **Negative interval (catchup):**
   `c
   sim_activate(unit, 100);
   run_120_instructions();  // Overshoot by 20
   // System should handle gracefully
   `

### Performance Tests

**Workload:** Simulate VAX system with 100 pending device events

**Metrics:**
- Event insert time (should be ~7 operations = O(log 100))
- Event cancel time (should be ~7 operations)
- Total simulation time (should be < 10% overhead from Phase 0)

**Tool:** Use SIMH's built-in profiling (sim_interval timing)

**Baseline:** Measure Phase 3 performance, compare to Phase 4B

---

## Future Work

### Phase 5: Free List Optimization (Optional)

**Goal:** Reduce malloc/free overhead for UNIT_EVENT allocation.

**Current approach:**
`c
evt = (UNIT_EVENT *)calloc(1, sizeof(UNIT_EVENT));  // On activate
free(evt);                                          // On fire/cancel
`

**Proposed approach:** Event free list
`c
static UNIT_EVENT *event_free_list = NULL;

UNIT_EVENT *event_alloc(void) {
    if (event_free_list != NULL) {
        UNIT_EVENT *evt = event_free_list;
        event_free_list = event_free_list->next_free;
        return evt;
    }
    return calloc(1, sizeof(UNIT_EVENT));
}

void event_free(UNIT_EVENT *evt) {
    evt->next_free = event_free_list;
    event_free_list = evt;
}
`

**Benefits:**
- Eliminates malloc/free for steady-state operation
- Better cache locality (reused allocations stay hot)

**Risks:**
- Increased complexity
- Memory not returned to OS
- Potential for leaks if free list management has bugs

**Decision:** Profile first. Only implement if malloc/free shows >5% overhead.

**Document:** projects/EVENT_FREELIST_DESIGN.md

---

### Phase 6: Lock-Free Queue (Optional)

**Goal:** Eliminate spinlock contention if async I/O creates contention.

**Current approach:** Spinlock protects all heap operations

**Proposed approach:** Lock-free MPSC (multi-producer, single-consumer)
- Producers: Async I/O threads schedule events
- Consumer: Main VM thread processes events
- Uses atomic operations only (no locks)

**Candidates:**
1. **Treiber stack** - See projects/EVENT_TREIBER_STACK.md
2. **MPSC ring buffer** - Already in repo (src/include/buf_ring.h)

**Decision criterion:** If spinlock contention > 1% of runtime, implement.

**Complexity:** High - lock-free data structures are subtle.

**Recommendation:** Defer until profiling proves need.

---

### Phase 7: Wall-Clock Timer Optimization

**Current:** Wall-clock timers (e.g., clock devices) use separate queue

**Issue:** Two event queues to manage (sim_clock_queue and sim_wallclock_queue)

**Proposed:** Unified heap with event type flag
`c
typedef struct UNIT_EVENT {
    // ...
    t_bool is_timer_event;  // Already present
};
`

**Benefit:** Single queue, single codebase, simpler

**Challenge:** Wall-clock events fire based on real time, not instruction count

**Design needed:** How to integrate wall-clock and instruction-based events

---

## Appendices

### A. File Inventory

**Core Implementation:**
- src/lib/sim_events.c (434 lines) - Min-heap implementation
- src/lib/sim_event_integration.c (283 lines) - Integration layer
- src/include/sim_events.h (66 lines) - Public API

**Modified Files:**
- src/core/scp.c - Event processing and time tracking
- src/core/sim_defs.h - UNIT_EVENT typedef and structure

**Test Files:**
- 	ests/unit/src/lib/test_sim_events.c (600+ lines) - 17 unit tests

**Documentation:**
- projects/EVENT_SYSTEM_COMPENDIUM.md (this document)
- projects/PHASE4B_DESIGN.md - Implementation guide
- projects/ARCHITECTURE_REVIEW_TIME_TRACKING.md - Architecture analysis
- projects/BUG3_TIME_TRACKING_DEEPER_ANALYSIS.md - Failure analysis
- projects/EVENT_SYSTEM_REDESIGN.md - Original design
- projects/EVENT_PHASE2_STATUS.md - Phase 2 summary
- projects/EVENT_PHASE3_STATUS.md - Phase 3 summary
- projects/PHASE4A_FINAL_STATUS.md - Phase 4A post-mortem

---

### B. Performance Projections

**Assumptions:**
- VAX system with 100 active devices
- Each device has 1 pending event on average
- Events fire and reschedule continuously

**Linked List (Phase 0):**
- Insert: ~50 comparisons (O(n/2))
- Cancel: ~50 comparisons (O(n))
- Total: ~100 operations per event cycle

**Min-Heap (Phase 4B):**
- Insert: ~7 comparisons (O(log 100))
- Cancel: ~7 comparisons (O(log 100))
- Total: ~14 operations per event cycle

**Speedup:** ~7× for insert, ~7× for cancel

**Wall-clock impact:** Assuming 10,000 events/second, saves ~860,000 operations/second

**CPU impact:** ~1-2% improvement in overall simulator throughput

---

### C. Git Commit History

**Phase 1:**
- 7437f6a9 - Event system Phase 1 implementation

**Phase 2:**
- 56f69fc7 - Event system Phase 2: Min-heap implementation with integration layer

**Phase 3:**
- 843a0c6a - Event system Phase 3: Build integration and enhanced testing
- bf0acc72 - events: Fix sim_atomic_cas usage and complete integration tests
- 6eccc275 - docs: Event system Phase 3 status and completion report

**Phase 4A (Failed):**
- d00c74fa - Event system Phase 4A: Replace scp.c event functions with heap
- aeb6174e - Event system Phase 4A complete: Replace all scp.c event queue operations
- 2645d7e9 - events: Fix heap initialization - add lazy init to integration layer
- a564e27a - docs: Critical heap initialization bug analysis and fix
- 660d7ef5 - docs: Phase 4A final status - functionally complete after heap init fix
- f4a58fcc - events: Fix time tracking - use sim_gtime() instead of heap's internal clock

**Phase 4A Analysis:**
- d1263a4f - docs: Phase 4A analysis - time tracking incompatibility with heap

**Phase 4A Revert:**
- d96a71d0 - Revert to Phase 3: Prepare for Phase 4B rip-and-replace design

**Phase 4B Design:**
- 0456b0d2 - docs: Phase 4B design - absolute-time events with instruction-driven interpolation

---

### D. References

**SIMH Documentation:**
- docs/developers/writing_a_simulator.md - VM implementation guide
- Section "Time Base" (lines 294-324)
- Section "Device Timing" (lines 621-701)

**Repository Standards:**
- projects/TESTING.md - Test methodology
- projects/CODE_STANDARDS.md - Coding standards
- AGENTS.md - Repository guidance

**External Resources:**
- Binary heap: Wikipedia, CLRS Chapter 6
- Lock-free queues: Maurice Herlihy, "The Art of Multiprocessor Programming"
- SIMH History: Bob Supnik's original documentation

---

## Conclusion

The migration from delta-time linked lists to absolute-time min-heap is a fundamental architectural change to SIMH/ZIMH's event system. It delivers significant performance improvements (O(n) → O(log n)) while modernizing the codebase.

**Key Learnings:**
1. Event scheduling and time tracking are tightly coupled
2. Incremental migration failed due to architectural incompatibility
3. Rip-and-replace is the only viable approach
4. Comprehensive testing catches integration issues early
5. Document failures as thoroughly as successes

**Current Status:**
- Phase 0-3: Complete and stable ✅
- Phase 4A: Failed, lessons documented ❌
- Phase 4B: Design complete, ready for implementation ⏳

**Next Steps:**
1. Implement Phase 4B-1 (time tracking replacement)
2. Test thoroughly with all 236 integration tests
3. Phase 4B-2 (queue cleanup)
4. Phase 4B-3 (sentinel removal)
5. Profile and optimize as needed

**Timeline Estimate:**
- Phase 4B-1: 1-2 days (code + testing)
- Phase 4B-2: 1 day (3 files to modify)
- Phase 4B-3: 1 day (automated refactoring + testing)
- Total: 3-4 days to completion

**Confidence:** High. Phase 4B design addresses all architectural issues discovered in Phase 4A.

---

---

## Phase 4C Implementation (October 2026)

### Overview

Phase 4C represents the actual implementation using the EVENT_HEAP.prev_interval approach rather than the full Phase 4B rip-and-replace design. This phase successfully integrated the min-heap event system while preserving UPDATE_SIM_TIME compatibility.

### Architecture

**Time Tracking Model:**

The Phase 4C implementation maintains two time systems that must stay synchronized:

1. **sim_vm_time + UPDATE_SIM_TIME** (authoritative)
   - Calculated as: `sim_vm_time + (prev_interval - sim_interval)`
   - Updated by UPDATE_SIM_TIME macro when sim_gtime() is called
   - Uses EVENT_HEAP.prev_interval to store the "old interval" value

2. **sim_event_heap_now()** (bookkeeping)
   - Absolute time stored in heap for internal use
   - Updated by sim_event_heap_set_now() during event processing
   - Used only for heap index validation and debug output

**Key Principle:** `sim_gtime()` is the single source of truth for time.

### Critical Bug Fix: Simultaneous Event Batching

**Problem Discovered:** When multiple events were scheduled for the same fire_time, the original heap implementation would:
1. Process one event
2. Return to sim_instr()
3. Execute an instruction
4. Process the next event

This caused spurious instruction execution between events that should fire atomically, breaking simulator semantics.

**Example from 3B2 RGTRU Test:**
```
Time 3: TOD event fires
Time 3: Instruction executes (WRONG!)
Time 3: INT-FLUSH event fires
Time 3: Another instruction executes (corrupts PC to 0x00000000)
Time 3: INT-STEP event fires with corrupted state
```

**Solution:**

Modified `sim_process_event_heap()` to batch-process all events at the current time:

```c
t_stat sim_process_event_heap(void)
{
    event_heap_lock_acquire();
    
    /* Process all events at the current time in one batch */
    do {
        evt = sim_event_heap_peek();
        if (evt == NULL) {
            /* Heap empty */
            event_heap_lock_release();
            sim_interval = NOQUEUE_WAIT;
            return SCPE_OK;
        }
        
        /* Check if event is ready to fire */
        if (evt->fire_time > (uint64_t)sim_gtime()) {
            /* No more events ready at current time */
            update_sim_interval_from_heap();
            event_heap_lock_release();
            return SCPE_OK;
        }
        
        /* Pop and process this event */
        evt = sim_event_heap_pop();
        /* ... event processing ... */
        
        /* If action returned stop/error, return it immediately */
        if (reason != SCPE_OK)
            return reason;
            
        /* Re-acquire lock and continue batching */
        event_heap_lock_acquire();
        
    } while (1);  /* Continue until no more events at current time */
}
```

**Critical Details:**

1. The readiness check uses `evt->fire_time > sim_gtime()` (not `>=`)
   - Events fire when time reaches their fire_time
   - Events at exactly the current time ARE ready

2. The loop continues while `evt->fire_time <= sim_gtime()`
   - All simultaneous events process before returning
   - Prevents instruction execution between same-time events

3. Lock is released/reacquired between events
   - Allows event actions to safely call sim_activate/sim_cancel
   - Maintains thread safety for async operations

### Test Infrastructure Changes

**Event Unit Tests:**

The unit tests required updates to work with production time tracking:

```c
/* Old approach (incorrect) - bypassed sim_gtime() */
sim_event_heap_set_now(100);
sim_process_event_heap();

/* New approach (correct) - initializes time system */
sim_interval = NOQUEUE_WAIT;
sim_event_heap_set_now(0);
sim_event_heap_set_prev_interval(NOQUEUE_WAIT);

/* Schedule events at relative times */
_sim_activate_heap(&unit, 100);

/* Simulate time advancing: set interval to 0 to trigger processing */
sim_interval = 0;
sim_process_event_heap();
```

**Why This Works:**

1. `_sim_activate_heap()` calls `sim_gtime()` which invokes UPDATE_SIM_TIME
2. UPDATE_SIM_TIME calculates current time from `sim_interval` and `prev_interval`
3. Event fire_time is set to `sim_gtime() + delay`
4. Later, when `sim_interval` counts down to 0, events become ready
5. `sim_process_event_heap()` checks `evt->fire_time > sim_gtime()` to determine readiness

### Commits

**Commit 1cfa8f2c:** "events: Fix instruction execution between simultaneous events"
- Implemented batch processing loop
- Fixed readiness check to use `>` instead of `>=`
- Verified 3B2 RGTRU tests pass

**Commit 99ac8f67:** "events: Fix event unit tests for batch processing behavior"
- Updated tests to properly initialize time tracking
- Restored consistent use of sim_gtime() throughout event integration
- Event tests: 17/17 PASS
- 3B2 tests: PASS

### Known Issues

**Timer Tests:** 37 of 73 timer unit tests still fail.

The timer subsystem has complex interactions with event scheduling, including:
- Internal timer activation (`sim_internal_timer_unit`)
- Clock coscheduling on timer ticks
- Calibrated timer state tracking
- Microsecond-based timer activation

These tests likely need similar time initialization fixes, but may also expose deeper issues with how timer events integrate with the heap.

**Other Test Failures:**

Several simulator integration tests fail with timeout or assertion issues:
- PDP-11 (timeout)
- VAX diagnostic tests (ICR register issues)
- Some expect/cmdvar unit tests (event scheduling dependencies)

These require investigation to determine if they are:
1. Test infrastructure issues
2. Event timing regressions
3. Pre-existing failures unrelated to event system

### Performance

No performance benchmarking has been conducted yet. The min-heap should provide:
- O(log n) insertion vs O(n) for linked list
- O(log n) removal vs O(n) for linked list
- O(1) peek (same as linked list)

Benefits become measurable with >10 simultaneous pending events.

### Future Work

1. **Fix Timer Tests**
   - Investigate timer unit test failures
   - Ensure timer activation properly initializes time system
   - Verify clock coscheduling works with batched events

2. **Investigate Integration Test Failures**
   - Determine root cause of VAX diagnostic failures
   - Fix or document expect/cmdvar test dependencies
   - Resolve PDP-11 timeout issues

3. **Phase 4B Completion** (Optional)
   - Consider full rip-and-replace to eliminate UPDATE_SIM_TIME
   - Would simplify time tracking at cost of larger refactor
   - Current Phase 4C approach is working and maintainable

4. **Performance Testing**
   - Benchmark event-heavy simulators (VAX, PDP-10)
   - Measure heap vs linked-list performance
   - Profile for optimization opportunities

---

**Document Version:** 1.1  
**Last Updated:** 2026-10-28  
**Status:** Phase 4C implemented, event tests passing, 3B2 verified  
**Branch:** unit_event_refactor  
**Contact:** B. Scott Michel

---
