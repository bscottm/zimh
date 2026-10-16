// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: BSD-2-Clause

// NOTE: The code in this file is adapated from FreeBSD's buf_ring.h source code:
// https://cgit.freebsd.org/src/plain/sys/sys/buf_ring.h?h=stable/15
//
// Original attribution:
// Copyright (c) 2007-2009 Kip Macy <kmacy@freebsd.org>
// All rights reserved.
// Copyright (c) 2024 Arm Ltd

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * sim_tailq_t: Lock-free SPSC (Single Producer, Single Consumer) ring buffer.
 *
 * This implementation uses atomic head pointers for producer and consumer.
 * The producer and consumer each maintain a cached copy of their head position
 * to minimize atomic operations while maintaining correctness.
 *
 * Threading model: Only one thread may call enqueue operations (producer),
 * and only one thread may call dequeue operations (consumer). These may be
 * different threads.
 *
 * Uses C11 atomics or compiler intrinsics for synchronization.
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#if !defined(SIM_TAILQ_H)
#define SIM_TAILQ_H

#include <stdbool.h>
#include "sim_platform.h"
#include "sim_atomic.h"

/* Generic pointer to items stored in the tail queue. This type exists to force the user to cast to their pointer type,
 * vs. using "void *", which often leads to interesting bugs. */
typedef struct sim_tailq_item_s {
    struct sim_tailq_item_s *__dummy_ignored_never_used;
} *sim_tailq_item_t;

/* Ring buffer slot - just holds the item pointer. */
typedef struct sim_tailq_slot_s {
    /* Data pointer. Written by producer, read by consumer. */
    sim_tailq_item_t item;
} sim_tailq_slot_t;

/* The ring buffer queue.
 *
 * Fixed-size ring buffer with power-of-2 capacity for fast modulo.
 * Producer and consumer head positions are cache-line aligned to prevent
 * false sharing between producer and consumer threads.
 */
typedef struct sim_tailq_s {
    sim_atomic_value_t prod_head;
    sim_atomic_value_t prod_tail;
    size_t prod_size;
    size_t prod_mask;
    SIM_CACHELINE_ALIGNED sim_atomic_value_t cons_head;
    sim_atomic_value_t cons_tail;
    size_t cons_size;
    size_t cons_mask;
    SIM_CACHELINE_ALIGNED sim_tailq_slot_t slots[0];
} sim_tailq_t;

/* Allocate and initialize a tail queue with the default capacity.
 * Returns NULL on allocation failure.
 */
sim_tailq_t *sim_tailq_init();

/* Allocate and initialize a tail queue with specified capacity (must be power of 2, >= 2).
 * Returns NULL on failure (capacity < 2, capacity isn't a power of 2, allocation)
 */
sim_tailq_t *sim_tailq_init_capacity(size_t capacity);

/* Destruction. Optionally frees items if free_item is provided. */
void sim_tailq_destroy(sim_tailq_t *tailq, void (*free_item)(sim_tailq_item_t));

/* Enqueue new data. Returns NULL if queue is full, tailq otherwise. */
sim_tailq_t *sim_tailq_enqueue(sim_tailq_t *tailq, sim_tailq_item_t item);

/* Dequeue from head. Returns NULL if queue is empty. */
sim_tailq_item_t sim_tailq_dequeue(sim_tailq_t *tailq);

/* Check if queue is empty (consumer-side check) */
static inline int sim_tailq_empty(const sim_tailq_t *tailq)
{
     return (sim_atomic_get(&tailq->cons_head) == sim_atomic_get(&tailq->prod_tail));
}

/* Number of elements in the queue (approximate, for monitoring only).
 * In SPSC, positions are not fully synchronized, so this is a snapshot.
 */
static inline sim_atomic_type_t sim_tailq_count(const sim_tailq_t *tailq)
{
    size_t prod_tail = (size_t)sim_atomic_get(&tailq->prod_tail);
    size_t cons_tail = (size_t)sim_atomic_get(&tailq->cons_tail);
    return ((sim_atomic_type_t)(tailq->prod_size + prod_tail - cons_tail) & tailq->prod_mask);
}

/* Number of elements allocated (capacity). */
static inline sim_atomic_type_t sim_tailq_allocated(const sim_tailq_t *tailq)
{
    return (sim_atomic_type_t)tailq->prod_size;
}

#endif /* SIM_TAILQ_H */
