# AIO/DIO Unification

## Problem

`SIM_ASYNCH_IO` is a compile-time macro that decides, once and for all at
build time, whether a simulator binary has the asynchronous I/O (AIO)
machinery at all. When it is undefined, every `AIO_*` macro in
`src/core/sim_defs.h:1210-1227` collapses to nothing, the `UNIT` struct
loses its `a_*` field block (`src/core/sim_defs.h:417-432`), and every
device's `_a`-suffixed entry point (`sim_disk_rdsect_a`,
`sim_tape_rdrecf_a`, etc.) degrades to calling the synchronous routine
inline and immediately invoking the caller's callback
(`src/runtime/sim_disk.c:398-403`).

In practice this dichotomy is close to fictional on our two supported
platforms. `cmake/os-features.cmake:354-355` defines `SIM_ASYNCH_IO`
(and `USE_READER_THREAD`) on the `aio_support` target whenever a pthreads
or C11-threads target is found, which is unconditionally true on recent
POSIX and recent Windows. The DIO branch exists for a build environment
we don't actually ship for. Meanwhile `sim_asynch_enabled`
(`src/core/scp.c:351`/`:417`) is already a *runtime* bool, toggled by
`SET ASYNCH`/`SET NOASYNCH` (`sim_set_asynch`, `src/core/scp.c:4388-4452`)
— so we already pay for a second compile-time universe to get a
behavior that's otherwise runtime-switchable anyway.

We also already have a working runtime CPU-affinity partitioner
(`sim_os_get_cpu_partition`, `src/lib/sim_threads.c:241-315`) that
degrades gracefully from 3 CPUs down to 1, and a portable CAS/atomics
layer (`src/include/sim_atomic.h`, `src/include/sim_atomic_ptr.h`) that
the AIO queue code does not use — it hand-rolls its own
`InterlockedCompareExchangePointer` shim instead
(`src/core/sim_defs.h:1107-1121`). The pieces needed to make AIO-vs-DIO
a runtime decision, rather than a build decision, already exist; they
just aren't wired together.

Goal: eliminate the `SIM_ASYNCH_IO` compile-time branch. One binary,
always built with the threading/atomics machinery present, with the
AIO/DIO choice made at runtime — explicitly via `SET NOASYNCH`, or
because a new feasibility flag, `sim_async_preference`, forbids async
entirely when `sim_os_get_cpu_partition` can't find enough CPUs to give
the I/O threads their own affinity. DIO is not dead code to be deleted;
it is the code path that runs whenever async isn't both requested and
feasible.

## Blast Radius

`SIM_ASYNCH_IO` is referenced in 30 files, ~80 occurrences:

| Area | Files | Occurrences |
|---|---|---|
| Core (`src/core`, `src/runtime`, `cmake`) | 12 | ~49 |
| Device/peripheral emulation (`simulators/`) | 11 | ~14 |
| Tests | 3 | ~9 |
| Docs/projects | 4 | ~10 (incl. `docs/history/0readmeAsynchIO.txt`, 8 refs) |

Core files carrying real logic (not just a register-table entry):
`src/core/sim_defs.h`, `src/core/scp.c`, `src/core/scp.h`,
`src/runtime/sim_console.c`, `src/runtime/sim_disk.c`,
`src/runtime/sim_tape.c`, `src/runtime/sim_timer.c`,
`src/runtime/sim_tmxr.c`/`.h`, `src/runtime/sim_ether.c`.

Device files are mostly a single conditional register-table block per
file (9 identical copies across `simulators/VAX/vax*_stddev.c`,
e.g. `vax_stddev.c:228`, exposing `ASYNCH`/`LATENCY` registers) plus
`simulators/PDP11/pdp11_xq.h:90-92`, which gates a polling-interval
constant on `defined(SIM_ASYNCH_IO) && defined(USE_READER_THREAD)`.
None of the device files implement their own threading; they only read
`sim_asynch_enabled`/`sim_asynch_latency` or call the shared `AIO_CALL`
macros defined once per I/O library (`sim_disk.c`, `sim_tape.c`).

Test exposure is small and mechanical: `tests/unit/src/runtime/test_sim_tape.c`
has 4 blocks gated on the macro, and
`tests/unit/simulators/VAX/CMakeLists.txt:178` injects the define for a
VAX unit-test target. `projects/SYM_TAPE_FIXES.md:5` already flags "the
real asynchronous path under `SIM_ASYNCH_IO`" as the largest coverage
gap in `sim_tape.c` — that gap is a direct consequence of the path being
reachable only in a different compiled binary than the one the rest of
the suite exercises.

**Conclusion: the removal's blast radius is contained.** No simulator
device implements its own locking or thread lifecycle; that is all
centralized in `sim_disk.c`, `sim_tape.c`, `sim_console.c`,
`sim_timer.c`, `sim_tmxr.c`, and `sim_ether.c`. Device files only need
their `#if defined(SIM_ASYNCH_IO)` register-table/help-text guards
dropped. The real work is in `sim_defs.h` and the six core files above.

## Current Architecture (as it stands)

- **Macro layer** — `src/core/sim_defs.h:991-1227`. Under
  `SIM_ASYNCH_IO`: pthread primitives, `AIO_TLS`, `AIO_QUEUE_CHECK`,
  `AIO_LOCK`/`AIO_UNLOCK`, two alternate queue implementations selected
  by `USE_AIO_INTRINSICS` (lock-free CAS vs. mutex-guarded list head),
  `AIO_ACTIVATE`, `AIO_VALIDATE`, `AIO_CHECK_EVENT`. Under `#else`, all
  fourteen of those collapse to no-ops or trivial constants
  (`sim_defs.h:1210-1227`).
- **UNIT struct** — the synchronous event-queue link `UNIT.next`
  (`sim_defs.h:384`) is unconditional. The AIO-only tail
  (`sim_defs.h:417-432`) adds a *second*, parallel intrusive link,
  `a_next`, plus `a_check_completion`, `a_is_active`, `a_event_time`,
  `a_activate_call`, polling-thread bookkeeping, and duplicate
  due-time fields (`a_due_time`, `a_due_gtime`, `a_usec_delay`) that
  shadow information the synchronous timer path tracks differently.
- **Queue drain/dispatch** — `sim_aio_update_queue()`
  (`src/core/scp.c:356-391`) atomically swaps `sim_asynch_queue` for
  `QUEUE_LIST_END`, walks the drained list, and re-dispatches each unit
  onto the real `sim_clock_queue` via `uptr->a_activate_call`.
  `sim_aio_activate()` (`scp.c:393-415`) is the enqueue half, called by
  `AIO_ACTIVATE` whenever `sim_activate*` is invoked off the main
  thread.
- **Per-device worker threads** — `sim_disk.c` (`_disk_io`,
  `sim_disk.c:292`), `sim_tape.c` (equivalent `_tape_io`), and
  `sim_console.c` (`_console_poll`, `sim_console.c:3419-3488`) each
  define their *own* private context struct (`struct disk_context`,
  `struct tape_context`) carrying `io_lock`/`io_cond`/`asynch_io`/
  `callback`/staged-operation fields, and each defines its own
  `AIO_CALLSETUP`/`AIO_CALL` macro pair
  (`sim_disk.c:254-284`/`:398-403`, similarly in `sim_tape.c`). This is
  the same lock/condvar/worker-loop pattern copy-pasted three times.
- **Build wiring** — `cmake/os-features.cmake:354-355` defines
  `SIM_ASYNCH_IO` on the `aio_support` target whenever pthreads/C11
  threads are available — i.e., always, on both supported platforms.
- **Runtime CPU partitioning (already exists, unused by AIO)** —
  `sim_os_get_cpu_partition()` / `sim_os_compute_cpu_partition()`
  (`src/lib/sim_threads.c:241-315`) already computes a main/I-O/SDL CPU
  split from `sim_os_get_process_affinity()`, degrading cleanly from 3
  CPUs down to 1 (`sim_threads.c:259-289`). It's already called from
  `scp.c:2696-2701`, `sim_console.c:3426-3431`, `sim_disk.c:299-304`,
  `sim_timer.c:2291-2296`, `sim_tmxr.c:3997-4002`, and
  `src/simnetwork/eth_threads.c:70-75,257-262` to pin worker threads —
  but nothing currently asks it "do I even have enough CPUs to bother
  with threads," which is exactly the resource-based auto-disable the
  unification should add.

## Design Issues Found

These are pre-existing problems independent of the AIO/DIO split, but
removing the split is the natural point to fix them because both touch
the same `UNIT`/queue code:

1. **`QUEUE_LIST_END` sentinel (`(UNIT *)1`) and dual intrusive links.**
   `sim_defs.h:267-273` picks a non-NULL, non-valid-pointer sentinel so
   that a unit's `next`/`a_next` field can double as "am I queued at
   all" (`NULL` = not queued, `QUEUE_LIST_END` = queued and last,
   anything else = queued with a successor). A `UNIT` can be linked
   into `sim_clock_queue` (via `next`) and `sim_asynch_queue` (via
   `a_next`) using two completely separate pointer fields embedded in
   the public `UNIT` struct that every device sees. This conflates
   "is a unit scheduled" with "does this struct have a spare pointer
   field," and it hard-codes an alignment assumption (address `1` is
   never a valid pointer) into application logic rather than isolating
   it behind a queue API.
2. **O(n) insertion-sorted linked list for the event queue.**
   `sim_activate`'s insertion loop (`scp.c:10253-10261`, `prvptr`/`accum`
   walking the list to find a delta-time-ordered insertion point) is
   O(n) per activation. A min-heap keyed on absolute event time is
   O(log n) insert/extract and removes the need for the delta-time
   "accum" rewalk entirely.
3. **Per-device duplicated threading plumbing.** `disk_context` and
   `tape_context` each hand-roll an `io_lock`/`io_cond`/worker-thread
   loop that is structurally identical. This should be one shared
   "worker queue" abstraction that device libraries configure with a
   callback, not three independent copies.
4. **Reimplemented CAS instead of `sim_atomic_ptr.h`.** The lock-free
   queue path (`USE_AIO_INTRINSICS`, `sim_defs.h:1085-1128`) defines its
   own `InterlockedCompareExchangePointer` macro for non-Windows GCC/
   Clang rather than using `sim_atomic_ptr_cas()`
   (`src/include/sim_atomic_ptr.h:152-178`), which already wraps this
   portably with memory-order control. There is no reason for two CAS
   implementations in the tree.

## Proposed Direction

Replace the compile-time branch with:

- `SIM_ASYNCH_IO` deleted. The pthread/atomics-based worker-thread
  machinery is simply part of the runtime, unconditionally, matching
  what `cmake/os-features.cmake` already does in effect.
- Two runtime flags replace the single compile-time branch:
  - `sim_async_preference` — a feasibility gate, computed once at
    startup from `sim_os_get_cpu_partition()`. It is `false` whenever
    the partitioner falls back to the `n == 1` case
    (`sim_os_compute_cpu_partition`, `sim_threads.c:278-282`/
    `:286-288`) — i.e., there's only one usable CPU, so the simulator's
    instruction-loop thread would dominate scheduling and could starve
    an I/O worker thread rather than overlap with it. On any host
    where the partitioner can isolate a separate I/O CPU,
    `sim_async_preference` is `true`. This flag is a ceiling, not a
    live switch: it says whether AIO is *possible* on this host, not
    whether it's currently in use.
  - `sim_asynch_enabled` — the existing live flag, unchanged in spirit
    (`SET ASYNCH`/`SET NOASYNCH` still toggle it, `sim_set_asynch`,
    `scp.c:4388-4452`), except it can now only become `true` when
    `sim_async_preference` is also `true`. `sim_set_asynch` rejects (or
    warns and no-ops) an attempt to enable async while
    `sim_async_preference` is `false`, instead of silently appearing to
    succeed on a host where it would starve the main thread.
  - This means the DIO code path is **not deleted** — it is exactly
    what runs whenever `sim_asynch_enabled` is `false`, whether that's
    because the user asked for it or because `sim_async_preference`
    forbids async entirely. What goes away is the *compile-time*
    fork, not the synchronous behavior itself: today's `#else` branch
    becomes the runtime behavior of "`sim_asynch_enabled == false`",
    reachable in every build instead of only in a `!SIM_ASYNCH_IO`
    build.
- `UNIT.a_next`/`a_event_time`/etc. go away in favor of a *separate*
  event-queue entry structure (not embedded in `UNIT`), queued in a
  min-heap keyed on absolute event time. A unit's membership in the
  heap is tracked by an opaque handle/index, not a pointer field on
  `UNIT` doing double duty as both link and "is-queued" flag. This
  removes `QUEUE_LIST_END` entirely — heap "empty" is just size `0`.
- Worker-thread enqueue/drain (today's `sim_aio_activate`/
  `sim_aio_update_queue`) becomes the *only* path into the event queue;
  there is no second "synchronous" insertion routine to keep in sync,
  because DIO mode is just AIO mode with the worker threads disabled
  and callbacks invoked inline on the calling thread instead of handed
  to a queue.
- `sim_disk.c`/`sim_tape.c` lose their private `AIO_CALLSETUP`/
  `AIO_CALL` macro pairs in favor of one shared worker-queue helper
  (new, in `sim_threads.c` or a sibling file) that both call.
- The lock-free queue path's hand-rolled CAS is replaced with
  `sim_atomic_ptr_t`/`sim_atomic_ptr_cas()`.
- `UNIT` gains a dedicated worker-thread teardown hook, analogous to
  the existing `cancel`/`io_flush` function-pointer fields
  (`sim_defs.h:396,412`): `t_stat (*a_clear_async)(UNIT *uptr);`.
  `sim_set_asynch` calls it, uniformly, on every attached unit when
  transitioning `sim_asynch_enabled` from `true` to `false` — instead
  of today's workaround of overloading `io_flush` to also mean "stop
  my worker thread" (`sim_disk.c:1085`, `sim_tape.c:565-567`, both
  calling their own `_clr_async` from inside the device's `io_flush`
  callback). See "New UNIT teardown hook" below.

### New UNIT teardown hook

Today, stopping a worker thread when async is turned off is not a
single, uniform operation:

- `sim_disk.c`/`sim_tape.c` each do the real work in their own
  `*_clr_async` (`sim_disk_clr_async`, `sim_disk.c:728-755`;
  `sim_tape_clr_async`, `sim_tape.c:529-...`): lock, clear the
  context's `asynch_io` flag, signal the condvar, `pthread_join` the
  worker, destroy the lock/condvars. But `sim_set_asynch`
  (`scp.c:4388-4435`) never calls either of these directly — it only
  calls `uptr->io_flush`, and it is only because `_sim_disk_io_flush`
  (`sim_disk.c:1082-1088`) and `_sim_tape_io_flush` (`sim_tape.c:560-568`)
  *also* happen to call `sim_disk_clr_async`/`sim_tape_clr_async` before
  doing the real flush that the mechanism works at all. A device that
  forgot to fold the `_clr_async` call into its `io_flush` would leak a
  running worker thread on `SET NOASYNCH` with no error.
- `sim_console.c`'s poll thread has its own stop function
  (`console_poll_stop`, `sim_console.c:3492-3504`), called from
  `sim_ttcmd` — a completely different trigger than `sim_set_asynch`,
  so console thread teardown is not even reachable from `SET NOASYNCH`
  today; it only happens when the simulator drops back to command mode.
- `sim_ether.c` has **no** teardown path at all — its reader/writer
  threads (`sim_ether.c:1780,1794`, joined only at device detach) only
  read `sim_asynch_enabled` once, at attach time (`sim_ether.c:1846`).
  This is exactly why `sim_set_asynch` has to hard-refuse the whole
  operation whenever any Ethernet device is attached
  (`scp.c:4402-4405`, `SCPE_ALATT`) — there is no way to tell an
  attached Ethernet device's threads to stop, so the command path
  avoids the situation instead of handling it.

This is a real correctness gap for the unification: once
`sim_asynch_enabled` is the only thing standing between "worker threads
running" and "worker threads not running" (rather than a build-time
fact), `SET NOASYNCH` must be able to reliably stop every worker thread
for every attached unit, uniformly, or the `SCPE_ALATT` refusal has to
stay forever and Ethernet never gets a working NOASYNCH path.

Add a new `UNIT` function-pointer field, next to `cancel`
(`sim_defs.h:412`) and `io_flush` (`sim_defs.h:396`):

```c
t_stat (*a_clear_async)(UNIT *uptr); /* stop this unit's AIO worker
                                         thread(s), if any; NULL if
                                         the unit has none */
```

`NULL` (not a shared NOP stub) is the right "no worker thread" value
here. `UDATA(act, fl, cap)` (`sim_defs.h:727`) expands to 15 positional
values ending at `UNIT.buf`; the handful of device files that append a
trailing positional initializer after `UDATA(...)` (e.g.
`UDATA(&tti_svc, TT_MODE_KSR, 0), KBD_POLL_WAIT`) only reach as far as
`UNIT.wait`, field 16. The `a_*` block is at the very end of the
struct, well past every field any static initializer in the tree
touches, so C's aggregate-initialization rule already zero-initializes
`a_clear_async` to `NULL` in every existing `UNIT` declaration with no
edits anywhere. Defaulting to a shared NOP function instead would
require either touching every statically-initialized `UNIT` in the
tree (hundreds of files) to assign it explicitly, or adding a runtime
"backfill NULL with NOP" pass during device registration — pure
overhead that still needs a NULL check somewhere to know whether to do
the backfill. `a_clear_async` is the same shape as the already-optional
`cancel`/`io_flush`/`a_check_completion`/`a_is_active` fields, each
guarded at its call site (`scp.c:10384`, `scp.c:7624`,
`sim_defs.h:1045`); `sim_set_asynch` guards `a_clear_async` the same
way: `if (uptr->a_clear_async) uptr->a_clear_async(uptr);`.

`sim_disk_set_async`/`sim_tape_set_async` set `uptr->a_clear_async =
sim_disk_clr_async` / `sim_tape_clr_async` respectively when they start
the worker thread, mirroring how `io_flush` is already assigned at the
same call sites (`sim_disk.c` around `set_async`, `sim_tape.c:935`).
`sim_console.c`'s `console_poll_stop` and a new
`sim_ether_clr_async`-style function (to be added for the reader/writer
threads) plug into the same field, which finally gives Ethernet a real
teardown path and lets the `SCPE_ALATT` refusal in
`scp.c:4402-4405` be dropped once this lands.

This hook is additive and does not depend on the `SIM_ASYNCH_IO`
removal — it's useful (and should be added) even if slice 1 below is
the only thing that ships. It becomes mandatory once worker threads
can be started and stopped purely at runtime, because there is no
longer a build that lacks them to fall back on.

### Open design decisions

Two choices need a decision before implementation, each with a
recommended "safest" option:

**Event-queue replacement scope.** Options:
- Do the min-heap replacement in the same effort as the
  `SIM_ASYNCH_IO` removal, since both touch `UNIT.next`/`a_next` and
  `sim_clock_queue` insertion.
- Do them as two sequential efforts: first collapse AIO/DIO onto a
  single code path while *keeping* the existing linked-list queue and
  `QUEUE_LIST_END` sentinel (just drop the second `a_next` link since
  there's only one queue now), then replace the queue representation
  in a follow-on project.

  The second option is safer: it isolates the correctness-critical
  "always-threaded, runtime-disabled" behavior change from the
  independent algorithmic change to queue representation, and gives
  each change its own characterization-test pass per
  `projects/TESTING.md`. **Recommended: do the AIO/DIO collapse first;
  track the min-heap/queue-structure replacement as a follow-on slice
  in this same document (see Commit Slices below), gated on the
  collapse landing cleanly.**

**Worker-thread lifecycle when `sim_async_preference` is false.** When
`sim_os_get_cpu_partition` can't isolate an I/O CPU, options are:
- Don't start the per-device worker threads at all; every `_a` entry
  point runs its body inline on the calling thread (closest to today's
  DIO behavior).
- Still start the threads, but don't bother with the affinity pinning
  call (let the OS scheduler share the single CPU).

  The first option matches today's actual DIO semantics (no extra
  thread, no synchronization overhead) and avoids introducing thread
  contention on a single-CPU host that the current DIO path never has
  — which is the whole point of `sim_async_preference` existing.
  **Recommended: don't start the worker threads when
  `sim_async_preference` is false; `sim_asynch_enabled == false`
  should mean "no AIO threads exist," not "AIO threads exist but are
  quiesced."**

## Relationship to Existing Work

- **`projects/ASYNC_IDENTIFIER_RENAME.md`** proposes *renaming*
  `SIM_ASYNCH_IO`/`asynch_io`/etc. to the `async` spelling while keeping
  the conditional-compilation structure intact. This document proposes
  *deleting* that macro and the fields it renames, which makes most of
  that rename's "Core/build macro cleanup" slice moot — there would be
  nothing left to rename. If both are pursued, this unification should
  go first (there's no point renaming a macro about to be deleted); if
  the rename is wanted independently as a smaller near-term step, do it
  first and treat "delete `SIM_ASYNC_IO`" as this document's slice 2
  instead. Either way, whoever picks this up should explicitly choose
  the order rather than running both in parallel.
- **`projects/SYM_TAPE_FIXES.md`** names "the real asynchronous path
  under `SIM_ASYNCH_IO`" as the largest coverage gap in `sim_tape.c`,
  because that path only compiles in a separate binary. After this
  unification there is exactly one binary and one code path, so that
  gap description needs to be rewritten (not just closed) once this
  lands — update `SYM_TAPE_FIXES.md` as part of the follow-on test work.
- **`projects/SIM_TIMER_FIXES.md`** lists "async-clock and
  timer-thread logic" as its largest remaining coverage gap and asks
  that new cleanup opportunities discovered while touching
  `sim_timer.c` be logged there. This document's timer-thread changes
  should be cross-linked from `SIM_TIMER_FIXES.md` rather than
  duplicated into it.
- **`projects/TIME_TESTING.md`** explicitly deferred async-clock
  redesign ("must not make that code harder to separate and test
  later"). This document is the deferred redesign; it does not change
  simulator-time vs. host-time semantics, only who runs the clock
  thread and when.
- **`projects/EVENT_QUEUE_DELAY_TYPES.md`** covers the `int32` delay
  representation on the activation surface, not the queue's storage
  structure. The min-heap replacement proposed here is a different,
  complementary change; neither document should block the other, but
  they should land with awareness of each other's touches to
  `sim_activate*`.

## Proposed Commit Slices

1. **Introduce `sim_async_preference`.** Compute it once at startup
   from `sim_os_get_cpu_partition()`'s `n == 1` fallback case; make
   `sim_set_asynch` refuse to set `sim_asynch_enabled = true` when
   `sim_async_preference` is `false`. `sim_asynch_enabled` keeps
   today's semantics otherwise. No macro removal yet — this is
   additive and testable on its own against the existing
   `SIM_ASYNCH_IO` build.
2. **Add the `UNIT.a_clear_async` hook.** Add the field, wire
   `sim_disk_set_async`/`sim_tape_set_async` to assign it, make
   `sim_set_asynch` call it on every attached unit when disabling
   async (instead of relying on `io_flush` side effects), add an
   Ethernet teardown function and wire it the same way, then drop the
   `SCPE_ALATT` Ethernet-attached refusal in `scp.c:4402-4405`. Also
   additive and independently testable — this closes a real thread-leak
   gap in the *current* `SIM_ASYNCH_IO` build, not only in the unified
   one.
3. **Collapse `SIM_ASYNCH_IO` to always-on.** Remove the `#else`
   branch of every `AIO_*` macro in `sim_defs.h`; remove the
   `#ifdef SIM_ASYNCH_IO` from the `UNIT` struct (the `a_*` fields are
   now unconditional); remove the macro from `cmake/os-features.cmake`
   (the `aio_support` target definitions become unconditional). Replace
   the hand-rolled CAS in the lock-free queue path with
   `sim_atomic_ptr_cas()`.
4. **Make `sim_asynch_enabled == false` suppress thread creation**, not
   just queue usage — `_disk_io`/`_tape_io`/`_console_poll` worker
   threads should not be started at all when disabled, matching the
   "Open design decisions" recommendation above. This is the same code
   path whether disabled by `sim_async_preference == false` or by an
   explicit `SET NOASYNCH`, and relies on slice 2's `a_clear_async`
   hook to actually join the threads before restarting or exiting.
5. **Strip the now-dead device-file guards.** Remove the
   `#if defined(SIM_ASYNCH_IO)` register-table and help-text blocks
   from the nine `simulators/VAX/*_stddev.c` files and
   `simulators/PDP11/pdp11_xq.h`; those registers/help text become
   unconditional.
6. **Deduplicate per-device worker-thread plumbing.** Extract the
   shared `io_lock`/`io_cond`/worker-loop pattern out of
   `disk_context`/`tape_context` into one worker-queue helper that
   `sim_disk.c` and `sim_tape.c` both configure; replace their private
   `AIO_CALLSETUP`/`AIO_CALL` macro pairs with calls into it.
7. **(Follow-on) Replace the event queue representation.** Introduce a
   min-heap keyed on absolute event time with its own entry structure;
   remove `UNIT.next`/`a_next` and `QUEUE_LIST_END` in favor of an
   opaque per-unit queue handle. This is the largest and riskiest
   slice and should only start once 1-6 are stable and covered.
8. **Documentation.** Rewrite `docs/history/0readmeAsynchIO.txt` (or
   fold its still-relevant content into `docs/developers/writing_a_simulator.md`)
   to describe the unified runtime-switched model instead of a
   build-time either/or; update `projects/SYM_TAPE_FIXES.md`'s coverage-gap
   description per the note above.

## Verification / Acceptance Criteria

- `rg SIM_ASYNCH_IO` returns no hits outside this document and
  `docs/history/` (kept as historical record of the old design).
- `UNIT` has exactly one event-queue linkage mechanism (not two
  parallel `next`/`a_next` fields) after slice 7.
- `SET NOASYNCH` joins every worker thread for every attached unit,
  verified by a test that attaches a disk, tape, console session, and
  (once slice 2 lands) an Ethernet device, issues `SET NOASYNCH`, and
  confirms no AIO threads remain running — including the Ethernet case
  that `scp.c:4402-4405` currently refuses outright.
- A build with `sim_os_get_process_affinity()` reporting a single CPU
  starts no AIO worker threads and exercises the same code paths as
  today's `!SIM_ASYNCH_IO` build, verified by a unit test that fakes a
  1-CPU affinity mask.
- `tests/unit/src/runtime/test_sim_tape.c`'s `#if defined(SIM_ASYNCH_IO)`
  blocks compile and run unconditionally (slice 3), then get real
  coverage of the worker-thread dispatch path rather than only the
  degraded inline path (addressing the `SYM_TAPE_FIXES.md` gap).
- Full test suite (`ctest --parallel`) passes on both supported
  platforms with no `SIM_ASYNCH_IO`/`!SIM_ASYNCH_IO` build matrix left
  to test against — one build configuration instead of two.
