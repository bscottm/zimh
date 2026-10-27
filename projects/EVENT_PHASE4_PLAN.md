# Event System Phase 4: Complete Integration Plan

## Overview

Phase 4 involves replacing the linked-list event queue in scp.c with the min-heap implementation, while maintaining full backward compatibility and handling the complex async I/O integration.

## Key Findings

### Current Architecture

SIMH has **THREE** separate event queues:

1. **`sim_clock_queue`** (scp.c:545) - Main instruction-based event queue
   - Linked list using UNIT.next pointers
   - Uses relative delta times between events
   - O(n) insertion via list traversal
   - O(n) cancellation via list search

2. **`sim_asynch_queue`** (sim_defs.h) - Async I/O event queue  
   - Separate queue for events from async I/O threads
   - Uses UNIT.a_next pointers (not UNIT.next)
   - Periodically drained into sim_clock_queue via `AIO_UPDATE_QUEUE`
   - Protected by mutex or lock-free intrinsics

3. **`sim_wallclock_queue`** (sim_timer.c:212) - Wall-clock timer queue
   - For real-time events (calibrated timers)
   - Uses UNIT.a_next for chaining
   - Managed by sim_timer.c

### Files That Need Updates

#### Critical (Core Event System):

1. **src/core/scp.c**
   - `_sim_activate()` (line 10252) - Replace O(n) list insertion
   - `sim_cancel()` (line 10382) - Replace O(n) list search
   - `sim_process_event()` (line 10135) - Replace list pop
   - `sim_clock_queue` global (line 545) - May become vestigial
   - All `QUEUE_LIST_END` references (81 occurrences)

2. **src/runtime/sim_timer.c**
   - `sim_wallclock_queue` management (line 212)
   - Multiple `QUEUE_LIST_END` checks (lines 469, 483, 878, etc.)
   - Timer queue iteration and cancellation

#### Medium Priority (Direct Queue Access):

3. **simulators/VAX/vax_cpu.c** (line 2693)
   - Direct `sim_clock_queue` access for VAX timing
   - Checks `sim_clock_queue != QUEUE_LIST_END`

4. **simulators/Ibm1130/ibm1130_cpu.c** (lines 485, 563, 625)
   - Iterates over `sim_clock_queue` directly
   - Checks `sim_clock_queue != QUEUE_LIST_END`
   - Uses `uptr->next` to traverse queue

### Async I/O Integration Complexity

The AIO system adds significant complexity:

```c
// From sim_defs.h:1142-1203
#define AIO_ACTIVATE(caller, uptr, event_time) \
    if (!sim_thread_equal(sim_thread_self(), sim_asynch_main_threadid)) { \
        sim_aio_activate((ACTIVATE_API)caller, uptr, event_time); \
        return SCPE_OK; \
    } else (void)0
```

**Key insight:** When called from async I/O thread, `_sim_activate()` redirects to `sim_aio_activate()`, which queues the event in `sim_asynch_queue`. The main thread periodically calls `AIO_UPDATE_QUEUE` to drain async events into `sim_clock_queue`.

**This means:** Our heap integration must handle both:
- Direct activation from main thread → heap insert
- Async activation from I/O thread → still use async queue, drain into heap

## Migration Strategy

### Phase 4A: Internal Replacement (scp.c only)

**Goal:** Replace linked list with heap in scp.c while maintaining all external interfaces.

**Changes:**

1. **_sim_activate()** becomes a wrapper:
```c
t_stat _sim_activate(UNIT *uptr, int32_t event_time)
{
    AIO_ACTIVATE(_sim_activate, uptr, event_time);
    if (sim_is_active(uptr))
        return SCPE_OK;
    UPDATE_SIM_TIME;
    
    // NEW: Use heap instead of linked list
    return _sim_activate_heap(uptr, event_time);
}
```

2. **sim_cancel()** becomes a wrapper:
```c
t_stat sim_cancel(UNIT *uptr)
{
    UNIT *nptr;
    t_stat cancel_status;
    
    AIO_VALIDATE(uptr);
    if ((uptr->cancel) && uptr->cancel(uptr))
        return SCPE_OK;
    if (uptr->dynflags & UNIT_TMR_UNIT) {
        cancel_status = sim_timer_cancel(uptr);
        if (cancel_status != SCPE_OK)
            return cancel_status;
    }
    AIO_CANCEL(uptr);
    AIO_UPDATE_QUEUE;
    
    if (!sim_is_active(uptr))
        return SCPE_OK;
    UPDATE_SIM_TIME;
    
    // NEW: Use heap instead of linked list
    return sim_cancel_heap(uptr);
}
```

3. **sim_process_event()** becomes a wrapper:
```c
t_stat sim_process_event(void)
{
    // ... existing async update logic ...
    AIO_UPDATE_QUEUE;
    
    // NEW: Use heap instead of linked list
    return sim_process_event_heap();
}
```

4. **Keep `sim_clock_queue` for backward compatibility:**
```c
// Option A: Make it always point to first scheduled unit (or QUEUE_LIST_END)
// Option B: Make it vestigial and update code that reads it
// Option C: Phase it out gradually
```

### Phase 4B: External Queue Access

**Goal:** Update files that directly access `sim_clock_queue`.

**Files to update:**

1. **src/runtime/sim_timer.c**
   - Replace `while (sim_clock_queue != QUEUE_LIST_END)` loops
   - Add heap-based timer queue iterator
   - Update wall-clock queue management

2. **simulators/VAX/vax_cpu.c**
   - Replace `sim_clock_queue != QUEUE_LIST_END` check
   - Use `sim_event_heap_count() > 0` instead

3. **simulators/Ibm1130/ibm1130_cpu.c**
   - Replace queue iteration with heap query API
   - Add `sim_event_heap_get_next_unit()` helper if needed

### Phase 4C: QUEUE_LIST_END Removal

**Goal:** Remove all 81 occurrences of QUEUE_LIST_END sentinel.

See `projects/QUEUE_LIST_END_REMOVAL.md` for full list.

**Key changes:**
- Replace `!= QUEUE_LIST_END` with `!= NULL` or heap count checks
- Remove `#define QUEUE_LIST_END ((UNIT *)1)` from sim_defs.h
- Update all initialization to use NULL

### Phase 4D: Thread Safety Profiling

**Goal:** Measure spinlock contention and determine if Treiber stack upgrade is needed.

**Metrics to collect:**
1. Spinlock acquisition time (average, max, p99)
2. Contention frequency (spin iterations)
3. Event insertion rate during async I/O bursts
4. Impact on simulator performance

**Profiling approach:**
```c
// Add instrumentation to event_heap_lock_acquire():
static inline void event_heap_lock_acquire(void)
{
    sim_atomic_type_t expected = 0;
    int spin_count = 0;
    uint64_t start_time = sim_os_msec();
    
    event_heap_spinlock_init();
    while (!sim_atomic_cas(&event_heap_spinlock, &expected, 1)) {
        spin_count++;
        expected = 0;
    }
    
    uint64_t acquire_time = sim_os_msec() - start_time;
    sim_atomic_inc(&lock_acquisitions);
    sim_atomic_add(&total_spin_count, spin_count);
    if (acquire_time > max_acquire_time)
        max_acquire_time = acquire_time;
}
```

**Decision criteria:**
- Contention < 5% → Keep spinlock (simple, fast)
- Contention 5-15% → Consider Treiber stack
- Contention > 15% → Implement Treiber stack (see EVENT_TREIBER_STACK.md)

## Stability Improvements Expected

### 1. Elimination of O(n) Pathologies

**Current worst case:** Insert event at end of 1000-event queue
- Cost: 1000 list traversals = O(n)
- With heap: log₂(1000) ≈ 10 operations = O(log n)
- **Speedup: 100×**

**Real-world impact:**
- Reduces scheduling jitter
- More predictable timing for high-frequency devices
- Better real-time responsiveness

### 2. Elimination of Linked List Corruption Bugs

**Current risks:**
- `uptr->next` pointer corruption crashes simulator
- Race conditions in async I/O can corrupt list
- `QUEUE_LIST_END` sentinel confusion

**With heap:**
- Array-based storage (no pointer chasing)
- Heap index validation catches corruption early
- Clear separation between scheduled/unscheduled

### 3. Better Thread Safety

**Current approach:**
- Separate async queue + periodic drain
- Complex locking in AIO_ACTIVATE macro
- Potential for lock contention

**With heap + spinlock:**
- Single atomic spinlock (Phase 1)
- Can upgrade to lock-free Treiber stack (Phase 4D)
- Simpler reasoning about thread safety

### 4. Improved Debuggability

**Current challenges:**
- Relative delta times hard to interpret
- Pointer chains hard to inspect
- `QUEUE_LIST_END` sentinel confusing

**With heap:**
- Absolute fire times (easier debugging)
- Array layout (easy to dump in debugger)
- `sim_event_heap_validate()` catches corruption

## Implementation Order

### Week 1: Phase 4A - Core Replacement
1. Update `_sim_activate()` in scp.c to call `_sim_activate_heap()`
2. Update `sim_cancel()` in scp.c to call `sim_cancel_heap()`
3. Update `sim_process_event()` in scp.c to call `sim_process_event_heap()`
4. Handle `sim_clock_queue` backward compatibility
5. Run basic simulator tests (PDP-11, VAX)

### Week 2: Phase 4B - External Access
6. Update sim_timer.c to use heap queries instead of queue traversal
7. Update VAX/vax_cpu.c to check heap count instead of queue pointer
8. Update Ibm1130/ibm1130_cpu.c to use heap iterator API
9. Run full simulator suite

### Week 3: Phase 4C - Cleanup
10. Remove all QUEUE_LIST_END references (81 occurrences)
11. Update all initialization code
12. Run full regression tests

### Week 4: Phase 4D - Profiling & Optimization
13. Add spinlock instrumentation
14. Run async I/O stress tests (tmxr, network)
15. Collect contention metrics
16. Decide on Treiber stack upgrade
17. Document performance improvements

## Testing Strategy

### Unit Tests (Already Complete)
- ✅ 17/17 tests passing
- ✅ Heap operations verified
- ✅ Integration layer verified

### Integration Tests (TODO)
- [ ] Run PDP-11 diagnostics
- [ ] Run VAX VMS boot
- [ ] Run IBM 1130 diagnostics
- [ ] Stress test with tmxr (multiple serial lines)
- [ ] Network I/O stress test

### Regression Tests (TODO)
- [ ] Full simulator test suite
- [ ] All architectures build cleanly
- [ ] No timing regressions

### Performance Tests (TODO)
- [ ] Benchmark event scheduling overhead
- [ ] Measure async I/O latency
- [ ] Profile spinlock contention
- [ ] Compare before/after timing accuracy

## Risk Mitigation

### Risk 1: Breaking Async I/O
**Mitigation:** 
- Keep AIO_ACTIVATE/AIO_CANCEL macros unchanged
- Async queue still drains into main queue
- Only replace main queue implementation

### Risk 2: Timing Changes
**Mitigation:**
- Maintain exact same event firing times
- Use absolute times (no rounding errors)
- Verify with diagnostic programs

### Risk 3: sim_clock_queue Dependencies
**Mitigation:**
- Identify all direct accesses (done above)
- Create heap query APIs for external use
- Gradual migration, not big bang

### Risk 4: Performance Regression
**Mitigation:**
- Profile before and after
- If heap is slower, investigate (shouldn't be!)
- Can roll back if catastrophic

## Success Criteria

1. ✅ All 17 unit tests pass
2. [ ] All simulator builds complete
3. [ ] All simulator regression tests pass
4. [ ] PDP-11/VAX/IBM1130 boot and run diagnostics
5. [ ] Async I/O devices work (serial, network)
6. [ ] Event scheduling is O(log n) confirmed
7. [ ] No timing accuracy regressions
8. [ ] Spinlock contention < 5%
9. [ ] No memory leaks detected
10. [ ] Code coverage > 95%

## Next Steps

1. **Start with Phase 4A** - Replace scp.c functions
2. **Test incrementally** - Don't break working code
3. **Profile carefully** - Measure don't guess
4. **Document everything** - Future maintainers will thank us

---

**Status:** Ready to Begin Phase 4A
**Last Updated:** 2026-10-27
**Estimated Completion:** 4 weeks
