# Time Tracking and Event Scheduling: Architecture Review

## Executive Summary

I've analyzed the SIMH/ZIMH clock/timer update cycle and its relationship to UNIT_EVENT scheduling. The current Phase 4A implementation has a fundamental architectural conflict between:
1. **Time tracking** (how `sim_vm_time` advances based on instruction execution)
2. **Event scheduling** (when device service routines fire)

This document explains the architecture and recommends a path forward.

## How Time Tracking Works

### The Core Loop (from `sim_instr()` in each VM)

```c
while (reason == 0) {
    // 1. Check if events are due
    if (sim_interval <= 0) {
        if ((reason = sim_process_event()))
            break;
    }
    
    // 2. Check interrupts, breakpoints, etc.
    
    // 3. Fetch and execute one instruction
    IR = M[PC];
    PC++;
    
    // 4. DECREMENT THE TIMER ← KEY STEP
    sim_interval = sim_interval - 1;  // or more for multi-cycle instructions
    
    // 5. Execute the instruction...
}
```

### Key Variables

1. **`sim_interval`** (int32_t)
   - Instructions until next event fires
   - Decremented by instruction executor (usually by 1 per instruction)
   - When reaches 0, `sim_process_event()` is called

2. **`sim_vm_time`** (double)
   - Total simulation time elapsed since last RUN/BOOT
   - Updated by `UPDATE_SIM_TIME` macro
   - Returned by `sim_gtime()`

3. **`sim_rtime`** (uint32_t)
   - Low 32 bits of `sim_vm_time`
   - Used for relative time calculations

### The UPDATE_SIM_TIME Formula (OLD Linked-List System)

```c
#define UPDATE_SIM_TIME do {
    int32_t _x;
    if (sim_clock_queue == QUEUE_LIST_END)
        _x = noqueue_time;  // No events pending
    else
        _x = sim_clock_queue->time;  // Time to next event
    
    // Calculate how many instructions actually executed
    sim_vm_time = sim_vm_time + (_x - sim_interval);
    sim_rtime = sim_rtime + (uint32_t)(_x - sim_interval);
    
    // Save current sim_interval for next cycle
    if (sim_clock_queue == QUEUE_LIST_END)
        noqueue_time = sim_interval;
    else
        sim_clock_queue->time = sim_interval;
} while (0)
```

**The Formula:**
- `_x` = OLD interval value (preserved in queue)
- `sim_interval` = NEW interval value (after event processing)
- `(_x - sim_interval)` = instructions that actually executed

**Example:**
1. `sim_interval = 1000` (1000 instructions until next event)
2. Executor runs 1000 instructions, decrements to 0
3. Calls `sim_process_event()`
4. UPDATE_SIM_TIME: `_x = 1000` (from queue), `sim_interval = 0` (current)
5. `sim_vm_time += (1000 - 0) = 1000` ← Time advances by 1000

## Event Scheduling

### Old Linked-List Approach (Phase 3 and earlier)

Events stored with **RELATIVE** times:

```c
struct UNIT {
    UNIT *next;      // Next unit in queue (or NULL)
    int32_t time;    // Instructions until THIS event fires (RELATIVE)
    // ...
};
```

Queue organized as delta-encoded linked list:
```
HEAD → [100] → [50] → [200] → NULL
       ↑       ↑      ↑
       Fire    Fire   Fire at
       at 100  at 150 350
```

When scheduling:
1. Calculate wait time (e.g., 175 instructions)
2. Walk linked list, subtracting each node's time
3. Insert at correct position with adjusted relative time

**Pros:** Events stored naturally for time tracking
**Cons:** O(n) insertion, O(n) cancellation

### New Heap Approach (Phase 4A)

Events stored with **ABSOLUTE** fire times:

```c
typedef struct UNIT_EVENT {
    UNIT *unit;
    uint64_t fire_time;  // ABSOLUTE sim time when event fires
    // ...
} UNIT_EVENT;
```

Heap organized by absolute fire_time:
```
       [150]
       /    \
    [200]  [350]
    
Fire times: 150, 200, 350 (absolute)
```

When scheduling:
1. Calculate fire_time = `sim_gtime()` + wait_time
2. Insert into heap (O(log n))

**Pros:** O(log n) insertion, O(log n) cancellation
**Cons:** **INCOMPATIBLE WITH UPDATE_SIM_TIME**

## The Fundamental Conflict

### Why UPDATE_SIM_TIME Breaks with Heap

UPDATE_SIM_TIME relies on `sim_clock_queue->time` preserving the OLD `sim_interval` value:

**With Linked List:**
- Queue stores relative times
- `sim_clock_queue->time` = instructions until next event
- This value is STABLE between event processing cycles
- UPDATE_SIM_TIME reads it as `_x` to calculate delta

**With Heap:**
- Events have absolute `fire_time`, not relative
- `sim_clock_queue` abandoned (set to QUEUE_LIST_END)
- UPDATE_SIM_TIME can't read the old value → **BREAKS**

### The Circular Dependency

```
sim_process_event()
  └─> UPDATE_SIM_TIME (needs OLD sim_interval)
      └─> Reads sim_clock_queue->time
          └─> But queue is EMPTY with heap!

_sim_activate_heap()
  └─> Needs sim_gtime() to calculate fire_time
      └─> sim_gtime() calls UPDATE_SIM_TIME
          └─> UPDATE_SIM_TIME needs sim_clock_queue->time
              └─> Which doesn't exist!
```

## Attempted Fixes (All Failed)

### Fix Attempt #1: Sync sim_clock_queue from heap
- Maintain dummy UNIT node with `time = sim_interval`
- Update after every heap operation
- **Failed:** Timing of updates is wrong, UPDATE_SIM_TIME sees stale data

### Fix Attempt #2: Track sim_interval_prev
- Save previous `sim_interval` value globally
- Use in UPDATE_SIM_TIME formula
- **Failed:** Multiple UPDATE_SIM_TIME calls per cycle overwrite it

### Fix Attempt #3: Make UPDATE_SIM_TIME idempotent
- Only run UPDATE_SIM_TIME once per cycle
- Use flag to prevent re-entry
- **Failed:** Doesn't fix the fundamental "no old value" problem

## The Real Question: Do We Need UPDATE_SIM_TIME?

### What UPDATE_SIM_TIME Actually Does

It advances `sim_vm_time` based on how many instructions executed. But with absolute fire times, **we don't need delta calculations**!

### Alternative: Direct Time Tracking

```c
// In sim_instr() loop (each VM):
sim_interval--;         // Decrement as usual

// When event fires:
sim_process_event() {
    // Pop event with absolute fire_time
    UNIT_EVENT *evt = heap_pop();
    
    // Update sim_vm_time to match event fire time
    sim_vm_time = (double)evt->fire_time;
    
    // Calculate new sim_interval to NEXT event
    UNIT_EVENT *next = heap_peek();
    if (next)
        sim_interval = (int32_t)(next->fire_time - evt->fire_time);
    else
        sim_interval = NOQUEUE_WAIT;
}
```

**Key insight:** With absolute times, `sim_vm_time` should **equal** the current event's fire time, not be calculated via deltas!

## Coexistence Analysis

### Can Two Event Systems Coexist?

**Short answer: NO, not cleanly.**

The linked-list system and heap system have fundamentally incompatible assumptions:
- **Linked list:** Relative times, delta-based time tracking
- **Heap:** Absolute times, direct time tracking

Trying to maintain both means:
1. Double the memory (two data structures)
2. Perfect synchronization required
3. UPDATE_SIM_TIME still broken (which system does it trust?)
4. High maintenance burden

### Verdict: Rip-and-Replace Needed

**Recommendation:** Complete migration to heap-based absolute-time system.

## Proposed Rip-and-Replace Strategy

### Phase 1: Replace UPDATE_SIM_TIME

Create new time tracking that works with absolute times:

```c
// New approach: sync sim_vm_time to event fire times
static inline void update_sim_time_absolute(void) {
    // With absolute fire times, sim_vm_time tracks naturally
    // No delta calculation needed!
}
```

**Question for you:** How does `sim_vm_time` actually need to advance?
- Option A: Only when events fire (event-driven)
- Option B: Continuously as instructions execute (instruction-driven)
- Option C: Hybrid (sync at event boundaries)

### Phase 2: Remove sim_clock_queue

Once UPDATE_SIM_TIME is replaced:
1. Remove all references to `sim_clock_queue`
2. Remove `QUEUE_LIST_END` (81 occurrences)
3. Heap becomes the single source of truth

### Phase 3: Test and Validate

Run full test suite, including:
- Unit tests for event scheduling
- Integration tests for all simulators
- Timing accuracy tests

## Questions for Your Guidance

### Q1: How should sim_vm_time advance with absolute times?

Currently, `sim_vm_time` advances via delta calculation in UPDATE_SIM_TIME. With absolute fire times:

**Option A: Event-driven (simplest)**
```c
// sim_vm_time only advances when events fire
sim_vm_time = (double)current_event->fire_time;
```
Pro: Simple, correct
Con: `sim_gtime()` called mid-instruction returns slightly stale value

**Option B: Instruction-driven**
```c
// Advance sim_vm_time on every instruction
sim_vm_time++;  // In sim_instr() loop
```
Pro: Always accurate
Con: Have to touch every VM's instruction loop

**Option C: Hybrid**
```c
// Advance at event boundaries, interpolate between
sim_vm_time_base = current_event->fire_time;
sim_vm_time = sim_vm_time_base + (prev_interval - sim_interval);
```
Pro: Accurate without touching VMs
Con: More complex

**Which approach do you prefer?**

### Q2: Risk tolerance for rip-and-replace?

Phase 4A tried incremental migration (replace functions, keep UPDATE_SIM_TIME). This failed.

True rip-and-replace means:
- Removing UPDATE_SIM_TIME entirely
- Changing how `sim_gtime()` works
- Potentially touching multiple VMs

**Risk assessment:**
- **High:** Changes core time tracking used by ALL simulators
- **Mitigated by:** Comprehensive test suite (236 tests)
- **Rollback:** Can revert to Phase 3 if needed

**Are you comfortable with this level of change?**

### Q3: Should we fix Phase 4A or start fresh?

**Option A: Fix Phase 4A (band-aid approach)**
- Try more sophisticated sync mechanisms
- Keep both systems running
- Low confidence this will work

**Option B: Revert to Phase 3, redesign from scratch**
- Go back to working linked-list system
- Design clean absolute-time architecture
- Implement in new Phase 4B
- Higher confidence, but more work

**Option C: Push forward with rip-and-replace**
- We're already 90% there (heap works, just time tracking broken)
- Fix time tracking properly
- Medium confidence, medium effort

**Which path do you prefer?**

## Summary

The clock/timer update cycle is:
1. **Instruction executor** decrements `sim_interval`
2. When `sim_interval <= 0`, call `sim_process_event()`
3. Process event, calculate new `sim_interval` to next event
4. **UPDATE_SIM_TIME** advances `sim_vm_time` based on elapsed instructions

The relationship to UNIT_EVENT:
- **Old:** UNIT_EVENT implicitly in UNIT.next/UNIT.time (relative)
- **New:** UNIT_EVENT explicit structure with absolute fire_time

The conflict:
- UPDATE_SIM_TIME assumes relative times
- Heap uses absolute times
- **They cannot coexist cleanly**

**Recommendation:** Rip-and-replace UPDATE_SIM_TIME with absolute-time-aware version.

**Next step:** Your guidance on Q1-Q3 above.
