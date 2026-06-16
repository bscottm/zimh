// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * sim_tailq_t: Lock-free SPSC (Single Producer, Single Consumer) tail queue.
 *
 * Threading model: Only one thread may call enqueue operations at a time (producer),
 * and only one thread may call dequeue operations at a time (consumer). These may be
 * different threads.
 *
 * Uses C11 atomics or compiler intrinsics for synchronization.
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#if !defined(SIM_TAILQ_H)
#define SIM_TAILQ_H

#include <stdbool.h>
#include "sim_atomic.h"
#include "sim_atomic_ptr.h"

/* Generic pointer to items stored in the tail queue. This forces type casts to
 * the desired pointer type and avoids the potential bugs that can arise from
 * `void *`.
 */
typedef struct sim_tailq_item_s {
    struct sim_tailq_item_s *__dummy_ignored_never_used;
} *sim_tailq_item_t;

/* Item status indicator.
 *
 * An internal `item` status indicator that ensures that the `sim_tailq_elem_t->item`
 * member is stable. `TAILQ_ITEM_READY` indicates that the `item` pointer is stable,
 * i.e., not being modified. `TAILQ_ITEM_BUSY` indicates that the `item` pointer is
 * not stable, i.e., being modified, and a dequeue operation needs to wait until
 * the status changes to `TAILQ_ITEM_READY`.
 */
typedef enum sim_tailq_item_status_e {
    TAILQ_ITEM_READY,
    TAILQ_ITEM_BUSY
} sim_tailq_item_status_t;

/* Queue element (node) type */
typedef struct sim_tailq_elem_s {
    /* Generic element pointer. Plain pointer; ordering is enforced by the
     *  item_status release/acquire handshake surrounding all writes and reads. */
    sim_tailq_item_t item;
    /* Item status — BUSY while the producer is filling `item`, READY when done. */
    sim_atomic_value_t item_status;
    /*! Next element in the tail queue (circular ring). */
    sim_atomic_ptr_t next;
} sim_tailq_elem_t;

/* The tail queue list type.
 *
 * An atomic access-controlled circular ring queue that also tracks the number of
 * active elements and the total number of allocated ring nodes.
 *
 * SPSC invariant: the head pointer is written only by the consumer thread;
 *   the tail pointer is written only by the producer thread. Both may be read by
 *   either thread, which is why they are atomically accessed.
 *
 * The queue is initialized using `sim_tailq_init()`.
 *
 * Items are enqueued and dequeued using `sim_tailq_enqueue()` /
 * `sim_tailq_enqueue_xform()` and `sim_tailq_dequeue()`.
 *
 * `sim_tailq_destroy()` cleans the queue when it is no longer needed.
 */
typedef struct sim_tailq_s {
    sim_atomic_ptr_t head;
    sim_atomic_ptr_t tail;
    sim_atomic_value_t n_elements;
    sim_atomic_value_t n_allocated;
} sim_tailq_t;

/* Item transformer function used when enqueuing a new item.
 *
 * - item: The item. Cast it to the required type, e.g., an Ethernet packet.
 * - arg: Pointer to extra data passed by the caller.
 *
 * Typical use case: `memcpy()` or `memmove()` from a source packet passed in
 * `arg` into `item`.
 */
typedef sim_tailq_item_t (*sim_tailq_xform_t)(sim_tailq_item_t item, void *arg);

/* Initialization. */
int sim_tailq_init(sim_tailq_t *tailq);
/* Destruction.
 *
 * - free_item: A function pointer that cleans up the item's internals. Cast it
 *   to the required type.
 */
void sim_tailq_destroy(sim_tailq_t *tailq, void (*free_item)(sim_tailq_item_t));

/* Enqueue new data to the tail. */
sim_tailq_t *sim_tailq_enqueue(sim_tailq_t *tailq, sim_tailq_item_t item);

/* Enqueue new data to the tail, applies a transform function to the new item. */
sim_tailq_t *sim_tailq_enqueue_xform(sim_tailq_t *tailq,
                                     sim_tailq_xform_t xform, void *xform_arg);

/* Dequeue from the tail. */ 
sim_tailq_item_t sim_tailq_dequeue(sim_tailq_t *tailq);

/* Access the item in the node. */
static inline void *sim_tailq_item(const sim_tailq_elem_t *node)
{
    return node->item;
}

/* Number of elements in the queue (queue depth.) */
static inline sim_atomic_type_t sim_tailq_count(const sim_tailq_t *tailq)
{
    return sim_atomic_get(&tailq->n_elements);
}

/* Number of elements allocated (>= queue depth.) */
static inline sim_atomic_type_t sim_tailq_allocated(const sim_tailq_t *tailq)
{
    return sim_atomic_get(&tailq->n_allocated);
}

/* Empty predicate */
static inline int sim_tailq_empty(const sim_tailq_t *tailq)
{
    /* This is SPSC-safe because:
     *
     * - If producer sees empty and enqueues, consumer will see non-empty next
     *   check
     * - If consumer sees non-empty and dequeues, producer will see empty next
     *   check
     *
     * No data race, just a benign time-of-change to time-of-use (TOCTOU) issue.
     */
    return (sim_atomic_ptr_get(&tailq->head) == sim_atomic_ptr_get(&tailq->tail));
}

#endif /* SIM_TAILQ_H */
