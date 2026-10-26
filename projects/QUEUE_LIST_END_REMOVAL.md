# QUEUE_LIST_END Sentinel Removal

## Current State

The `QUEUE_LIST_END` sentinel is a peculiar design artifact from the intrusive
linked list implementation:

```c
/* sim_defs.h:275 */
#define QUEUE_LIST_END ((UNIT *)1)
```

This exists to distinguish two states:
- `UNIT.next == NULL` → Unit not in any queue
- `UNIT.next == QUEUE_LIST_END` → Unit is last in queue

## Why It's Weird

1. **Invalid pointer masquerading as valid:** `(UNIT *)1` is not a valid
   pointer, but it's tested with `!=` comparisons throughout scp.c

2. **Relies on alignment assumption:** The comment notes this works because
   `1` is not a valid pointer due to alignment (typically 4 or 8 bytes).
   This is fragile.

3. **Type system abuse:** Casting integer `1` to `UNIT *` violates type safety

## Current Usage (grep analysis)

```
src/core/scp.c:      27 occurrences
src/core/scp.h:       1 declaration  
src/core/sim_defs.h:  1 definition
```

**Representative examples from scp.c:**

```c
/* Line 10266: Linked list traversal */
for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
    if (event_time < (accum + cptr->time))
        break;
    // ...
}

/* Line 10144: Empty queue check */
if (sim_clock_queue == QUEUE_LIST_END) {
    sim_interval = noqueue_time;
    return SCPE_STOP;
}

/* Line 10403: Initialize next pointer */
nptr = QUEUE_LIST_END;
```

## Why Min-Heap Eliminates It

### No Pointer Chaining

**Old (linked list):**
```c
for (cptr = queue; cptr != QUEUE_LIST_END; cptr = cptr->next)
    // Walk pointer chain, need sentinel to stop
```

**New (min-heap):**
```c
for (size_t i = 0; i < heap->count; i++)
    // Array indexing, count is natural terminator
```

### Natural Empty Detection

**Old:**
```c
if (sim_clock_queue == QUEUE_LIST_END)  /* empty? */
```

**New:**
```c
if (sim_event_heap_count() == 0)  /* empty? */
```

### Natural "Not Scheduled" Test

**Old:**
```c
/* UNIT.next == NULL means "not scheduled"
   UNIT.next == QUEUE_LIST_END means "last in queue"
   UNIT.next == <other> means "scheduled with next event"
   
   Three states in one pointer! */
```

**New:**
```c
/* Simple: */
if (uptr->event == NULL)  /* not scheduled */
if (uptr->event != NULL)  /* scheduled */

/* No sentinel needed */
```

## Removal Plan (Phase 2)

### Step 1: Audit External Usage

```bash
# Check if simulator code uses QUEUE_LIST_END
grep -r "QUEUE_LIST_END" simulators/
```

**Expected:** Zero matches (it's internal to scp.c)

### Step 2: Replace All scp.c References

**Pattern 1: Queue traversal**
```c
// Before:
for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next)

// After:
for (size_t i = 0; i < sim_event_heap_count(); i++)
    UNIT_EVENT *evt = sim_event_heap->events[i];
```

**Pattern 2: Empty check**
```c
// Before:
if (sim_clock_queue == QUEUE_LIST_END)

// After:
if (sim_event_heap_count() == 0)
```

**Pattern 3: Initialization**
```c
// Before:
sim_clock_queue = QUEUE_LIST_END;

// After:
sim_event_heap_cleanup();
sim_event_heap_init();
```

### Step 3: Remove Definition

```c
/* Delete from sim_defs.h:269-275 */
// #define QUEUE_LIST_END ((UNIT *)1)
```

### Step 4: Verify

```bash
# Should find zero references:
grep -r "QUEUE_LIST_END" src/

# Run full test suite:
ctest --parallel
```

## Benefit: Cleaner Code

### Before (scp.c:10252)
```c
t_stat _sim_activate (UNIT *uptr, int32_t event_time)
{
    UNIT *cptr, *prvptr;
    int32_t accum;
    
    prvptr = NULL;
    accum = 0;
    for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
        if (event_time < (accum + cptr->time))
            break;
        accum = accum + cptr->time;
        prvptr = cptr;
    }
    if (prvptr == NULL) {                       /* insert at head */
        cptr = uptr->next = sim_clock_queue;
        sim_clock_queue = uptr;
    }
    else {
        cptr = uptr->next = prvptr->next;       /* insert at prvptr */
        prvptr->next = uptr;
    }
    uptr->time = event_time - accum;
    if (cptr != QUEUE_LIST_END)                 /* SENTINEL CHECK */
        cptr->time = cptr->time - uptr->time;
    sim_interval = sim_clock_queue->time;
    return SCPE_OK;
}
```

### After (Phase 2)
```c
t_stat _sim_activate (UNIT *uptr, int32_t event_time)
{
    if (uptr->event != NULL)
        return SCPE_OK;  /* already scheduled */
    
    UNIT_EVENT *evt = malloc(sizeof(UNIT_EVENT));
    evt->unit = uptr;
    evt->fire_time = sim_event_heap_now() + event_time;
    evt->is_timer_event = false;
    
    heap_insert(sim_event_heap, evt);
    uptr->event = evt;
    
    return SCPE_OK;
}
```

**No more:**
- Sentinel checks
- Pointer chasing
- Delta time arithmetic
- Special case for "insert at head"

## Related Removals

Once `QUEUE_LIST_END` is gone, these also become vestigial:

1. **`UNIT.next`** - No more linked list, no next pointer needed
2. **Delta time encoding** - Absolute `fire_time` replaces relative `UNIT.time`
3. **`sim_clock_queue` global** - Replaced by `sim_event_heap`

All three disappear in Phase 2, along with the sentinel.

## Testing Strategy

Per TESTING.md, removal must be verified:

1. **Unit tests:** Existing event tests cover heap operations
2. **Integration tests:** Full simulator regression suite
3. **Grep verification:** Zero `QUEUE_LIST_END` references after Phase 2
4. **Compiler check:** No warnings about undefined `QUEUE_LIST_END`

## Summary

**Yes, `QUEUE_LIST_END` becomes completely vestigial after Phase 2.**

The min-heap's natural termination (array bounds) and NULL checking eliminate
the need for this weird sentinel. Its removal simplifies the code and removes
a source of potential undefined behavior (invalid pointer arithmetic).

**Timeline:**
- Phase 1: Sentinel still exists (heap not yet integrated)
- Phase 2: Sentinel removed when linked list code replaced
- Phase 3+: Remains absent

The sentinel was clever for its time (SIMH dates to 1993), but modern
data structures eliminate the need for such tricks.
