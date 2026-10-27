# Bug #3: Phase 4A Time Tracking Still Broken

## Status
After fixing Bug #1 (heap initialization) and Bug #2 (using sim_gtime() instead of heap->now),
simulators STILL fail with timeouts. Multiple attempted fixes have not resolved the issue.

## Symptoms
- 3b2-400, 3b2-700: TIMEOUT 120 sec (infinite loop)
- pdp8, pdp9: TIMEOUT 120 sec  
- i650: Completes but test fails ("FDS failed (bad ar)")

These are NOT pre-existing failures - they worked before Phase 4A.

## Attempted Fixes (All Failed)
1. **Sync sim_clock_queue from heap** - maintain dummy_queue_head, update it after every heap operation
   - Result: Still times out
   
2. **Modified UPDATE_SIM_TIME to check heap count** - use sim_interval directly when heap has events
   - Result: Still times out
   
3. **Track sim_interval_prev** - save previous sim_interval value for delta calculation
   - Result: Still times out
   
4. **Make UPDATE_SIM_TIME idempotent** - only run once per cycle using sim_time_updated flag
   - Result: Still times out

## Root Cause Analysis

The fundamental problem: **Phase 4A replaced event SCHEDULING but not time TRACKING**.

### How Time Tracking Works in Old System

With linked-list events:
1. Events stored with relative `.time` field (instructions until fire)
2. `sim_clock_queue->time` = instructions until next event
3. Instruction executor decrements `sim_interval` each instruction
4. When `sim_interval <= 0`, call `sim_process_event()`
5. UPDATE_SIM_TIME uses formula: `sim_vm_time += (sim_clock_queue->time - sim_interval)`
   - This calculates how many instructions actually ran
6. Event processing updates `sim_clock_queue->time` = new interval

The key: **sim_clock_queue->time preserves the OLD interval value** so UPDATE_SIM_TIME can calculate delta.

### How Phase 4A Broke This

With heap-based events:
1. Events stored with ABSOLUTE `.fire_time` (not relative)
2. `sim_clock_queue` abandoned (set to QUEUE_LIST_END)
3. `sim_interval` calculated from heap: `next_evt->fire_time - sim_gtime()`
4. But `sim_gtime()` calls UPDATE_SIM_TIME, which reads... what?
   - Old approach: reads `sim_clock_queue->time` → but that's QUEUE_LIST_END!
   - Falls back to `noqueue_time` which is wrong

5. UPDATE_SIM_TIME breaks because:
   - Can't read sim_clock_queue (it's empty)
   - Can't use sim_interval directly (that's the NEW value, not OLD)
   - sim_gtime() gets called multiple times per cycle → not idempotent

### The Circular Dependency

```
_sim_activate_heap()
  └─> update_sim_interval_from_heap()
      └─> sim_gtime()
          └─> UPDATE_SIM_TIME
              └─> Needs OLD sim_interval value
                  └─> But we just calculated NEW sim_interval!
```

## Why All Fixes Failed

1. **Sync sim_clock_queue**: Timing is wrong - we update AFTER operations, but UPDATE_SIM_TIME needs state BEFORE
2. **Check heap count**: Doesn't solve the "old vs new sim_interval" problem
3. **Track sim_interval_prev**: Gets overwritten by multiple UPDATE_SIM_TIME calls in same cycle
4. **Make idempotent**: Prevents multiple updates but doesn't fix the wrong initial update

## The Real Solution

We need to **stop using UPDATE_SIM_TIME's relative-delta approach** and switch to **absolute time tracking**.

### Option A: Replace UPDATE_SIM_TIME Entirely (RECOMMENDED)

Don't calculate deltas. Just track absolute time:

```c
void update_sim_time_absolute(void) {
    // With heap: sim_interval is already correct (instructions until next event)
    // Time advances based on instruction execution, not event deltas
    // This happens naturally in sim_instr() loop
}
```

The insight: **With absolute fire times, we don't need UPDATE_SIM_TIME at all!**
- `sim_vm_time` should only advance in the instruction executor (`sim_instr()`)
- Event scheduling uses `sim_gtime()` which just returns current `sim_vm_time`
- No delta calculations needed

### Option B: Maintain Parallel Linked List

Keep BOTH systems running:
- Heap for O(log n) operations
- Linked list for UPDATE_SIM_TIME compatibility
- Synchronize them perfectly

This is complex and error-prone (as we've seen).

### Option C: Revert Phase 4A

Go back to linked-list event system until we can do a proper redesign.

## Recommendation

**Implement Option A**: Remove UPDATE_SIM_TIME dependency entirely.

Key changes needed:
1. Modify `sim_gtime()` to NOT call UPDATE_SIM_TIME when using heap
2. Ensure `sim_vm_time` advances correctly in instruction executor
3. Test that time tracking remains consistent

This is a bigger change but addresses the root cause instead of adding more band-aids.

## Next Steps

1. Understand how `sim_instr()` and the instruction executor work
2. Verify that `sim_vm_time` can advance without UPDATE_SIM_TIME
3. Implement clean separation: heap for events, instruction executor for time
4. Test thoroughly

## Lessons Learned

- **Retrofitting is hard**: Trying to make heap work with UPDATE_SIM_TIME is fighting the design
- **Abstractions leak**: UPDATE_SIM_TIME assumes linked-list implementation details
- **Test-driven development**: Should have caught this earlier with better integration tests
- **Design for testability**: Time tracking and event scheduling should be decoupled

---

**Current Status:** Multiple fix attempts failed. Need fundamental redesign of time tracking.
**Confidence:** Low that incremental fixes will work. Need architectural change.
