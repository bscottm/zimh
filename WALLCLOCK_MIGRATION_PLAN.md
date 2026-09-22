# MPSC Queue Migration Plan: Wallclock Timers & Display

## Overview

Two issues to fix:
1. **Wallclock timer queue** still uses intrusive `a_next` pointers
2. **Queue display** tries to walk obsolete `sim_asynch_queue`

---

## Issue 1: Wallclock Timer Queue (sim_wallclock_queue)

### Current Architecture

**Location:** `src/runtime/sim_timer.c`

**Threading Model:** MPSC (Multiple Producer, Single Consumer)
- **Producer:** Simulator thread scheduling timer events
- **Consumer:** Timer thread (`_timer_thread`) processing due events
- **Protection:** Mutex (`sim_timer_lock`)

**Current Implementation:**
- Intrusive linked list using `UNIT->a_next`
- Sorted by `a_due_time` (insertion sort on enqueue)
- Mutex-protected operations

### Problems

1. ✅ **Already has proper synchronization** (mutex-based, not broken like async queue)
2. ❌ **Uses intrusive pointers** preventing field removal
3. ❌ **Insertion is O(n)** - walks queue to find insertion point

### Migration Options

#### Option A: Use Existing MPSC Queue + Heap (RECOMMENDED)

**Pros:**
- ✅ Reuse tested infrastructure
- ✅ O(log n) insertion via heap
- ✅ Non-intrusive
- ✅ Already handles (time, sequence) ordering

**Cons:**
- ⚠️ Need separate queue/heap instance (not shared with async I/O)
- ⚠️ Events stored by simulation time vs wallclock time (need mapping)

**Implementation:**
```c
// In sim_timer.c - new static structures
static sim_event_mpsc_queue_t sim_wallclock_mpsc_queue;
static sim_event_heap_t sim_wallclock_heap;

// Event wrapper
typedef struct {
    UNIT *unit;
    double due_time;        // Wallclock time
    uint64_t sequence;      // For deterministic ordering
} sim_wallclock_event_t;
```

#### Option B: Keep Mutex-Based Queue, Remove Intrusive Pointers

**Pros:**
- ✅ Minimal changes
- ✅ Mutex already provides synchronization

**Cons:**
- ❌ Still O(n) insertion
- ❌ Need separate event structure anyway
- ❌ Doesn't leverage new infrastructure

---

## Issue 2: Queue Display (scp.c lines 5535-5550)

### Current Code

```c
fprintf(st, "asynchronous pending event queue\n");
if (sim_asynch_queue == QUEUE_LIST_END)
    fprintf(st, "  Empty\n");
else {
    /* FIXME!! Iterate through the min-heap */
    for (uptr = sim_asynch_queue; uptr != QUEUE_LIST_END; uptr = uptr->a_next) {
        // Display event info...
        fprintf(st, " event delay %d\n", uptr->a_event_time);
    }
}
```

### Problem
- Tries to walk `sim_asynch_queue` which is now empty (deprecated)
- Should display contents of **MPSC heap** instead

### Solution: Display Heap Contents

Need to add heap iteration function:

```c
// In sim_event_queue.h
size_t sim_event_heap_count(const sim_event_heap_t *heap);
sim_unit_event_t* sim_event_heap_peek(const sim_event_heap_t *heap);
sim_unit_event_t* sim_event_heap_get_at(const sim_event_heap_t *heap, size_t index);
```

**Display implementation:**
```c
fprintf(st, "asynchronous pending event queue\n");
size_t count = sim_event_heap_count(&sim_event_heap);
if (count == 0) {
    fprintf(st, "  Empty\n");
} else {
    for (size_t i = 0; i < count; i++) {
        sim_unit_event_t *event = sim_event_heap_get_at(&sim_event_heap, i);
        UNIT *uptr = event->unit;
        // Display event info...
        fprintf(st, " event delay %d (seq %llu)\n", 
                event->event_time, event->sequence);
    }
}
```

---

## Implementation Steps

### Phase 1: Fix Queue Display (Low Risk)

1. ✅ Add heap accessor functions to `sim_event_queue.h/c`:
   - `sim_event_heap_count()` - already exists (inline)
   - `sim_event_heap_get_at()` - NEW: array access

2. ✅ Update `scp.c` queue display to iterate heap

3. ✅ Test with existing async I/O events

**Estimated effort:** 30 minutes
**Risk:** Low - read-only operations

### Phase 2: Migrate Wallclock Timer Queue (Medium Risk)

#### Option A: Use MPSC Queue + Heap

1. ✅ Create separate queue/heap for wallclock timers
2. ✅ Create `sim_wallclock_event_t` wrapper structure
3. ✅ Replace queue operations in `sim_timer.c`:
   - Enqueue: Use `sim_event_enqueue()`
   - Dequeue: Use `sim_event_migrate_to_heap()` + `sim_event_extract_min()`
4. ✅ Update timer thread to process heap
5. ✅ Remove mutex protection (MPSC queue is lock-free)

**Estimated effort:** 2-3 hours
**Risk:** Medium - threading changes

#### Option B: Simple Non-Intrusive Queue

1. ✅ Create `sim_wallclock_event_t` structure
2. ✅ Change from intrusive to pointer-based queue
3. ✅ Keep existing mutex protection
4. ✅ Keep existing O(n) insertion

**Estimated effort:** 1-2 hours
**Risk:** Low - minimal changes

### Phase 3: Remove UNIT Fields (Breaking Change)

Only after Phase 1 and Phase 2 complete:

1. ✅ Remove `UNIT->a_next`
2. ✅ Remove `UNIT->a_event_time`
3. ✅ Remove `UNIT->a_activate_call`
4. ✅ Remove `sim_asynch_queue` global
5. ✅ Update documentation

**Risk:** HIGH - ABI breaking change

---

## Recommendation

### Immediate Action
**Phase 1 only** - Fix the display issue:
- Low risk
- Visible benefit (correct display)
- Unblocks testing/validation

### Next Steps
**Phase 2 Option B** first (simple non-intrusive):
- Lower risk than Option A
- Unblocks field removal
- Can upgrade to Option A later if needed

**Phase 2 Option A** later (MPSC infrastructure):
- Better performance
- Leverages existing code
- More invasive changes to timer system

### When to Remove Fields
After **both** Phase 1 and Phase 2 complete:
- All users migrated off intrusive fields
- ABI break acceptable (major version bump)
- Extended testing period

---

## Questions for You

1. **Which approach for wallclock queue?**
   - Option A: MPSC queue + heap (better performance, more changes)
   - Option B: Simple non-intrusive (less risk, keeps mutex)

2. **Priority?**
   - Fix display first? (30 min, low risk)
   - Fix wallclock queue first? (2-3 hours, medium risk)
   - Do both together?

3. **Timeline for field removal?**
   - After next migration
   - After extended soak period
   - Specific version target?
