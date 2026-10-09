# UNIT.capac/pos Type Conflation

## Problem

`UNIT.capac` (device/file capacity) and `UNIT.pos` (file position) are
both declared `t_addr` (`src/core/sim_defs.h:394-395`):

```c
t_addr capac;               /* capacity */
t_addr pos;                 /* file position */
```

`t_addr` is a guest-address-width type (`src/core/sim_defs.h:120-126`):

```c
#if defined(USE_INT64) && defined(USE_ADDR64) /* 64b address */
typedef uint64_t t_addr;
#else                                         /* 32b address */
typedef uint32_t t_addr;
#endif
```

`capac` and `pos` are not guest addresses. They are host-file-size and
host-file-offset concepts: how many bytes a disk/tape image holds, and
where the simulator's read/write cursor currently sits in that host
file. By `projects/MACHINE_WORD_TYPES.md`'s own stated taxonomy (lines
25-28), that puts them in the "host indexes, counts, lengths, and
sizes should use appropriate host count types" bucket, not the "guest
addresses should use address-sized unsigned types" bucket — yet they
are typed with the guest-address type. This surfaced while assessing
whether `sim_fsize()` (`uint32_t`-returning) could be collapsed into
`sim_fsize_ex()` (`sim_off_t`-returning, see `src/runtime/sim_fio.c`):
for the ~24 call sites that assign the result straight into
`uptr->capac`, widening the source doesn't change anything observable,
because the destination is `t_addr`, which is `uint32_t` on every
build in this tree except the three that define `USE_ADDR64`
(`FEATURE_FULL64`: `simulators/VAX`, `simulators/alpha`,
`simulators/3B2` — see `cmake/add_simulator.cmake:109`). The type
conflation, not the narrow return type of `sim_fsize()`, is what's
actually capping file-size/position handling at 32 bits almost
everywhere in the tree.

This is a real, if narrow, functional ceiling. `sim_fio.c`'s own
header comment (`sim_fio.c:49-50`) states the design intent plainly:
*"sim_fsize is always a 32b routine (it is used only with small
capacity random access devices like fixed head disks and
DECtapes)."* That intent is only true as long as `capac`/`pos` stay
32-bit on those devices' builds — which today is guaranteed not by any
device-specific decision but by `t_addr`'s width, a type those devices
have no control over and no reason to be coupled to.

## Scope

`uptr->capac` is assigned or read at roughly 531 sites across 131
files; `uptr->pos` at roughly 520 sites across 87 files (counts from
`rg -c '\->capac\b'` / `'\->pos\b'`, repo-wide). This is extensive by
any measure — per `MACHINE_WORD_TYPES.md`'s own process (line 78-79),
"when the audit finds an area where a type change would be extensive,
create a subsystem-specific project note before doing the refactor,"
which is what this document is.

Note: `MACHINE_WORD_TYPES.md:67` points to `projects/UB_PROCESS.md`
for the audit methodology; that file does not currently exist in this
tree. Whoever picks up this project should either locate/restore it or
follow the methodology actually described inline in
`MACHINE_WORD_TYPES.md` (tests first, classify meaning before
retyping, update helper interfaces and consumers together, keep
signed/guest interpretation explicit and local, run focused tests
before broader suites).

## Why This Isn't Just "Widen t_addr"

`t_addr` is deliberately narrow-by-default because it sizes guest
address space, which has real cost implications (register/table sizes,
address arithmetic width) for every simulator in the tree, most of
which have no use for 64-bit addressing. `capac`/`pos` have nothing to
do with that trade-off — they size host files, which can be large
regardless of how wide the simulated machine's address bus is (a PDP-8
has an 12-bit address space but its attached disk image file can still
be gigabytes). Coupling file size/position to guest address width means
every simulator either:

- stays stuck at a 4GiB file-size/position ceiling it has no reason to
  have (the status quo), or
- would have to turn on 64-bit guest addressing it doesn't otherwise
  need, just to get a wider `capac`/`pos` (not a real option).

The fix is to decouple the two concepts, not to widen `t_addr` itself.

## Proposed Direction

Introduce a host-file-size/offset type for this purpose — `sim_off_t`
(`src/include/sim_platform.h:197-210`, already 64-bit on both
supported platforms) is the natural candidate, since it's already
used for this exact purpose by `sim_fsize_ex()`, `sim_ftell()`, and
`sim_fseeko()`. Retype `UNIT.capac` and `UNIT.pos` to `sim_off_t`,
decoupling them from `t_addr` entirely.

This is not a mechanical retype. Each of the ~1000 combined call sites
needs the same per-site judgment `MACHINE_WORD_TYPES.md` and
`VAX_WORD_TYPES.md` already call for elsewhere in the tree:

- Many sites do arithmetic that mixes `capac`/`pos` with genuinely
  guest-address-typed values (sector/track/cylinder computations,
  `t_lba` conversions) — those sites need the guest-address and
  host-file-size domains kept visibly separate, not silently merged by
  matching types.
- Some sites print `capac`/`pos` with address-oriented format macros
  (`PRIuADDR`-style); those need the `sim_off_t` format macro
  (`PRIsim_off_t`) instead.
- Some device-specific capacity constants are defined relative to
  `T_ADDR_W`/address width and would need re-examining once `capac`
  no longer shares a type with addresses.
- A handful of devices may have genuine reasons their capacity really
  is bounded by addressable guest memory (e.g., core-image or
  memory-mapped devices rather than disk/tape backing files); those
  should keep using an address-typed field, or a clearly-named
  separate field, rather than being swept into this change.

## Relationship to Existing Work

- Supersedes the practical motivation (though not the mechanics) for
  widening `sim_fsize()` to `sim_fsize_ex()` at most disk/tape call
  sites: once `capac` is `sim_off_t`, assigning `sim_fsize_ex()`'s
  result directly into it stops truncating, which is the actual
  functional goal that swapping `sim_fsize` for `sim_fsize_ex` without
  this retype would not achieve on non-`FEATURE_FULL64` builds.
- Is a direct instance of the pattern `projects/MACHINE_WORD_TYPES.md`
  and `projects/VAX_WORD_TYPES.md` both describe (classify the value's
  real domain before changing its type; keep host/guest and
  address/size domains from being conflated by a shared type), scoped
  to one specific, widespread conflation those documents hadn't yet
  named.
- Should be read before any further `sim_fsize`/`sim_fsize_ex`
  consolidation work in disk/tape drivers, since it changes what the
  "right" destination type for those call sites will eventually be.

## Suggested Process

1. Add characterization tests at a few representative `capac`/`pos`
   boundaries (disk attach/detach, tape attach, position tracking)
   before changing anything, per `MACHINE_WORD_TYPES.md`'s process.
2. Pick one subsystem (`sim_disk.c`/`sim_tape.c` and their direct
   device callers are the highest-value and most self-contained
   starting point, since they already route file size through
   `sim_fsize_ex()`/`sim_off_t` internally) and retype `capac`/`pos`
   there first, fixing format specifiers and adjacent arithmetic as
   found.
3. Expand outward one device family at a time, the same way the
   `sim_fsize_name` migration was split into subsystem-grouped commits
   (`projects/AIO_DIO_UNIFICATION.md`'s sibling work documents that
   recipe in more detail).
4. Leave devices whose "capacity" is genuinely guest-address-bounded
   (not host-file-size-bounded) on `t_addr`, documenting why per site.
