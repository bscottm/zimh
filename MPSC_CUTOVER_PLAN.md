# MPSC Queue Cutover Plan

## Overview

This document outlines the complete cutover plan for replacing the unsafe intrusive `sim_asynch_queue` with the new MPSC queue + min-heap implementation.

## Current Status

✅ **Phase 1: Prototype Complete**
- Lock-free MPSC queue implemented
- Min-heap priority queue implemented
- Atomic operations with proper memory ordering
- Dual-mode support (old + new running in parallel)
- Comprehensive unit tests written

## Cutover Phases

### Phase 2: Testing & Validation (1-2 weeks)

#### 2.1 Build Integration

**Files to Update:**
- `CMakeLists.txt` or build system
  - Add `src/lib/sim_event_queue.c` to build
  - Add `tests/unit/src/lib/test_event_queue.c` to test suite

**Actions:**
```bash
# Build with tests
cmake -B build -DBUILD_TESTING=ON
cmake --build build

# Run unit tests
ctest --test-dir build --output-on-failure -R test_event_queue
```

#### 2.2 Unit Test Verification

**Run Tests:**
```bash
# Basic functionality
./build/tests/unit/test_event_queue

# With sanitizers
cmake -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
./build-asan/tests/unit/test_event_queue

# With ThreadSanitizer
cmake -B build-tsan -DCMAKE_C_FLAGS="-fsanitize=thread"
./build-tsan/tests/unit/test_event_queue
```

**Expected Results:**
- ✅ All 14+ tests pass
- ✅ No memory leaks (ASan)
- ✅ No undefined behavior (UBSan)
- ✅ No data races (TSan)
- ✅ MPSC test with 4 threads × 25 events succeeds

#### 2.3 Integration Testing

**Simulators to Test:**
- VAX simulator with async I/O (network cards, serial)
- PDP-11 with async disk I/O
- Any simulator using `sim_asynch_queue`

**Test Scenarios:**
1. **Serial I/O**: Connect telnet, send/receive data
2. **Network I/O**: Ethernet device traffic
3. **Disk I/O**: File system operations
4. **Stress Test**: High concurrent I/O load
5. **Timing Accuracy**: Events fire at correct simulator times

**Actions:**
```bash
# Run VAX with async I/O
./vax
sim> set cpu async
sim> boot
# Perform I/O operations, verify no crashes/hangs

# Stress test: generate heavy I/O
sim> test device <device_name> 10000
```

**Validation Criteria:**
- No crashes or assertion failures
- Events process in correct order
- Timing accuracy maintained
- Performance comparable or better than old implementation

#### 2.4 Performance Benchmarking

**Benchmark Scenarios:**

1. **Throughput**: Events/second with varying producer counts
2. **Latency**: Time from enqueue to activation
3. **Scalability**: Performance with 1, 2, 4, 8 producer threads
4. **Memory**: Heap growth under load

**Metrics to Collect:**
```c
// Add instrumentation to sim_event_queue.c
static struct {
    uint64_t enqueues;
    uint64_t dequeues;
    uint64_t migrations;
    uint64_t heap_inserts;
    uint64_t heap_extracts;
    uint64_t heap_max_size;
} event_queue_stats;
```

**Acceptance Criteria:**
- Enqueue latency: < 1 μs (lock-free CAS)
- Migration latency: < 100 μs for 100 events
- Heap operations: < 10 μs per insert/extract
- Memory overhead: < 10% increase vs old queue

---

### Phase 3: Code Cleanup & Transition (1 week)

#### 3.1 Remove Dual-Mode Support

**File: `src/lib/sim_aio.c`**

**Changes:**
1. Remove old queue path from `sim_aio_update_queue()`
2. Remove `UNIT *volatile sim_asynch_queue` global
3. Simplify `sim_aio_activate()` to only use new queue

**Before:**
```c
int sim_aio_update_queue(void) {
    int migrated = 0;
    
    // OLD CODE PATH
    if (SIM_LIKELY(AIO_QUEUE_VAL != QUEUE_LIST_END)) {
        // ... old queue processing ...
    }
    
    // NEW CODE PATH
    int migrated_new = sim_event_migrate_to_heap(&sim_event_queue, &sim_event_heap);
    return migrated + migrated_new;
}
```

**After:**
```c
int sim_aio_update_queue(void) {
    return sim_event_migrate_to_heap(&sim_event_queue, &sim_event_heap);
}
```

#### 3.2 Update sim_process_event()

**File: `src/core/scp.c`**

**Current Code:**
```c
t_stat sim_process_event(void)
{
    // ... existing code ...
    AIO_UPDATE_QUEUE;  // Line 9986
    UPDATE_SIM_TIME;
    // ... process sim_clock_queue ...
}
```

**New Code:**
```c
t_stat sim_process_event(void)
{
    // ... existing code ...
    
    /* Migrate async events from MPSC queue to heap */
    AIO_UPDATE_QUEUE;
    
    /* Process heap events that are ready (time <= current time) */
    if (aio_enabled_and_active()) {
        int32_t current_time = (int32_t)sim_gtime();
        int processed = sim_aio_process_heap(current_time);
        
        /* Log if events were processed */
        if (processed > 0) {
            sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev,
                      "Processed %d async events from heap\n", processed);
        }
    }
    
    UPDATE_SIM_TIME;
    // ... rest of existing code ...
}
```

**Key Decision: Where to call `sim_aio_process_heap()`?**

Looking at the current flow:
1. `AIO_UPDATE_QUEUE` - migrates events to heap
2. `UPDATE_SIM_TIME` - updates simulator time
3. `sim_interval` check - see if any timed events ready
4. `sim_clock_queue` processing - process regular events

**Options:**

**A) Process heap BEFORE sim_clock_queue** (Recommended)
- Events go: async queue → heap → activated into sim_clock_queue
- Heap events become regular timed events
- Existing event processing handles everything

**B) Process heap AFTER sim_clock_queue**
- Heap events processed separately
- Requires careful timing coordination

**Recommendation: Option A** - Let heap events activate into `sim_clock_queue`, then existing code processes them.

#### 3.3 Deprecate UNIT->a_next Field

**File: `src/core/sim_defs.h`**

**Mark field as deprecated:**
```c
typedef struct unit {
    // ... existing fields ...
    
    /* DEPRECATED: Used by old async queue, will be removed */
    UNIT *a_next __attribute__((deprecated("Use new MPSC queue instead")));
    int32_t a_event_time __attribute__((deprecated("Use new MPSC queue instead")));
    
    // ... rest of fields ...
} UNIT;
```

**Update is_unit_aio_active() in `src/include/sim_aio.h`:**

**Before:**
```c
static inline bool is_unit_aio_active(const UNIT *unit) {
    return ((unit->a_is_active != NULL ? unit->a_is_active(unit) : false) || 
            unit->a_next != NULL);
}
```

**After:**
```c
static inline bool is_unit_aio_active(const UNIT *unit) {
    return (unit->a_is_active != NULL ? unit->a_is_active(unit) : false);
}
```

**Audit for other `a_next` checks:**
```bash
# Find all uses of a_next
grep -r "a_next" src/ --include="*.c" --include="*.h"

# Common patterns to fix:
# - if (uptr->a_next) { ... }
# - uptr->a_next = NULL;
# - for (uptr = queue; uptr != NULL; uptr = uptr->a_next)
```

#### 3.4 Remove Old AIO Macros

**File: `src/include/sim_aio.h`**

**Remove:**
```c
// These macros are no longer needed
#undef AIO_QUEUE_VAL
#undef AIO_QUEUE_SET
#undef AIO_ILOCK
#undef AIO_IUNLOCK

// Remove from sim_defs.h:
#undef QUEUE_LIST_END
```

**Keep:**
```c
// Still needed
#define AIO_UPDATE_QUEUE sim_aio_update_queue()
#define AIO_ACTIVATE(caller, uptr, event_time) // Now uses sim_event_enqueue()
```

---

### Phase 4: Final Removal (1 week)

#### 4.1 Remove UNIT Fields

**File: `src/core/sim_defs.h`**

**Remove from UNIT structure:**
```c
typedef struct unit {
    // REMOVE these lines:
    // UNIT *a_next;
    // int32_t a_event_time;
    // ACTIVATE_API a_activate_call;
    
    // KEEP:
    void (*a_check_completion)(struct unit *);
    // ... other fields ...
} UNIT;
```

**Impact Analysis:**
```bash
# Ensure no code still references removed fields
git grep "a_next\|a_event_time\|a_activate_call" src/
# Should only find definitions, no uses
```

#### 4.2 Remove Old Queue Code

**Files to Update:**
- `src/lib/sim_aio.c` - Remove `aio_queue_check()` if unused
- `src/include/sim_aio.h` - Remove old declarations
- `src/core/sim_defs.h` - Remove old macros

#### 4.3 Update Documentation

**Files:**
- `docs/async_io.md` - Document new MPSC queue architecture
- `CHANGELOG.md` - Note breaking change
- `MPSC_QUEUE_PROTOTYPE.md` - Update status to "PRODUCTION"

**Example CHANGELOG Entry:**
```markdown
## [Unreleased]

### Changed
- **BREAKING**: Replaced unsafe intrusive async event queue with lock-free MPSC queue + min-heap
  - Fixes memory ordering issues on ARM/RISC-V platforms
  - Provides deterministic event ordering via sequence numbers
  - Eliminates `UNIT->a_next` field pollution
  - Performance: Comparable or better than old implementation
  
### Migration Guide
- No API changes for simulator implementations
- Internal async I/O now uses proper atomic operations
- Events with same time are ordered by sequence number (deterministic)
```

---

## Rollback Plan

If critical issues are discovered during cutover:

### Quick Rollback (Minutes)

```bash
# Revert commits
git revert <cutover-commit-range>
git push

# Rebuild
cmake --build build
```

### Partial Rollback (Keep New Code, Disable It)

**Add runtime flag:**
```c
// In sim_aio.h
extern bool use_new_event_queue;  // Default: true

// In sim_aio_activate()
if (use_new_event_queue) {
    sim_event_enqueue(...);
} else {
    // Old code path
}
```

**CLI command:**
```
sim> set async old-queue   # Revert to old queue
sim> set async new-queue   # Use new queue (default)
```

---

## Validation Checklist

Before declaring cutover complete, verify:

### Functional Requirements
- [ ] All unit tests pass (14+ tests)
- [ ] No memory leaks (Valgrind/ASan)
- [ ] No data races (ThreadSanitizer)
- [ ] No undefined behavior (UBSan)
- [ ] Integration tests pass on all target simulators
- [ ] Stress tests complete without errors

### Performance Requirements
- [ ] Throughput: ≥ old implementation
- [ ] Latency: Enqueue < 1 μs, Process < 100 μs
- [ ] Memory: Overhead < 10%
- [ ] Scalability: Linear with producer count

### Code Quality
- [ ] All compiler warnings addressed
- [ ] Static analysis clean (cppcheck, clang-tidy)
- [ ] Code coverage ≥ 90% for new code
- [ ] Documentation updated
- [ ] CHANGELOG.md updated

### Platform Verification
- [ ] x86-64 Linux (strong memory model)
- [ ] ARM64 Linux (weak memory model)
- [ ] Windows x64 (MSVC compiler)
- [ ] macOS ARM64 (Apple Silicon)

---

## Timeline Summary

| Phase | Duration | Tasks | Milestone |
|-------|----------|-------|-----------|
| Phase 2: Testing | 1-2 weeks | Unit tests, integration tests, benchmarks | All tests pass, performance validated |
| Phase 3: Cleanup | 1 week | Remove dual-mode, deprecate fields, update callers | Single code path, deprecation warnings |
| Phase 4: Removal | 1 week | Remove UNIT fields, clean up old code, docs | Production-ready, old code gone |
| **Total** | **3-4 weeks** | | **Cutover Complete** |

---

## Key Files Modified Summary

### Phase 2 (Testing)
- `CMakeLists.txt` - Add test to build
- `tests/unit/src/lib/test_event_queue.c` - Run tests

### Phase 3 (Cleanup)
- `src/lib/sim_aio.c` - Remove dual-mode, simplify
- `src/core/scp.c` - Call `sim_aio_process_heap()`
- `src/include/sim_aio.h` - Update `is_unit_aio_active()`
- `src/core/sim_defs.h` - Deprecate UNIT fields

### Phase 4 (Removal)
- `src/core/sim_defs.h` - Remove UNIT.a_next, UNIT.a_event_time
- `src/include/sim_aio.h` - Remove old macros
- `docs/async_io.md` - Document new architecture
- `CHANGELOG.md` - Note breaking change

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Performance regression | Low | High | Benchmark before/after, keep old code until validated |
| Data races on weak memory | Low | High | ThreadSanitizer testing on ARM |
| Heap memory exhaustion | Very Low | Medium | Monitor heap growth, set limits |
| Timing accuracy issues | Low | Medium | Integration tests verify event timing |
| Compatibility with 3rd party simulators | Medium | Medium | Provide migration guide, deprecation period |

---

## Success Criteria

Cutover is successful when:

1. ✅ All tests pass on all platforms
2. ✅ No performance regression (≤5% slower acceptable)
3. ✅ No crashes or hangs in 24-hour stress test
4. ✅ ThreadSanitizer reports no data races
5. ✅ Production simulators run without issues for 1 week
6. ✅ Old code completely removed
7. ✅ Documentation updated

---

**Cutover Owner**: TBD  
**Target Completion**: 3-4 weeks from Phase 2 start  
**Status**: Phase 1 Complete ✅
