// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "sim_defs.h"
#include "sim_event_queue.h"
#include "sim_threads.h"

#include <cmocka_version.h>

#if defined(CMOCKA_VERSION_MAJOR) && CMOCKA_VERSION_MAJOR >= 2
#    define sim_assert_int_in_range(val, min, max) assert_int_in_range((val), (min), (max))
#    define sim_assert_uint_in_range(val, min, max) assert_uint_in_range((val), (min), (max))
#else
#    define sim_assert_int_in_range(val, min, max) assert_in_range((val), (min), (max))
#    define sim_assert_uint_in_range(val, min, max) assert_in_range((val), (min), (max))
#endif

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Mock UNIT and activation functions for testing
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static UNIT mock_units[10];
static int activation_count = 0;
static int activation_order[100];
static int completion_check_count = 0;

/* Mock activation function that records calls */
static t_stat mock_activate(UNIT *uptr, int32_t delay)
{
    if (activation_count < 100) {
        activation_order[activation_count] = (int)(uptr - mock_units);
    }
    activation_count++;
    return SCPE_OK;
}

/* Mock completion check function */
static void mock_check_completion(UNIT *uptr)
{
    (void)uptr;
    completion_check_count++;
}

/* Reset counters before each test */
static void reset_mocks(void)
{
    activation_count = 0;
    completion_check_count = 0;
    memset(activation_order, -1, sizeof(activation_order));
    memset(mock_units, 0, sizeof(mock_units));
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * MPSC Queue Tests
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static void test_queue_init(void **state)
{
    (void)state;
    sim_event_mpsc_queue_t queue;

    sim_event_queue_init(&queue);

    /* Queue should be empty after initialization */
    assert_null(sim_atomic_ptr_get(&queue.head));
    assert_int_equal(sim_atomic_get(&queue.sequence), 0);

    sim_event_queue_destroy(&queue);
}

static void test_queue_single_enqueue(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_queue_init(&queue);

    /* Enqueue a single event */
    bool result = sim_event_enqueue(&queue, &mock_units[0], 100,
                                      (ACTIVATE_API)mock_activate, NULL);
    assert_true(result);

    /* Verify sequence number was assigned */
    sim_unit_event_t *head = (sim_unit_event_t *)sim_atomic_ptr_get(&queue.head);
    assert_non_null(head);
    assert_ptr_equal(head->unit, &mock_units[0]);
    assert_int_equal(head->event_time, 100);
    assert_int_equal(head->sequence, 0);

    /* Verify sequence counter advanced */
    assert_int_equal(sim_atomic_get(&queue.sequence), 1);

    sim_event_queue_destroy(&queue);
}

static void test_queue_multiple_enqueue(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_queue_init(&queue);

    /* Enqueue multiple events */
    for (int i = 0; i < 5; i++) {
        bool result = sim_event_enqueue(&queue, &mock_units[i], i * 10,
                                          (ACTIVATE_API)mock_activate, NULL);
        assert_true(result);
    }

    /* Verify sequence numbers are unique and monotonic */
    uint64_t seen_sequences[5];
    sim_unit_event_t *event = (sim_unit_event_t *)sim_atomic_ptr_get(&queue.head);
    int count = 0;

    while (event != NULL && count < 5) {
        seen_sequences[count] = event->sequence;
        count++;
        event = event->next;
    }

    assert_int_equal(count, 5);

    /* Check all sequences are unique and in range [0, 4] */
    for (int i = 0; i < 5; i++) {
        sim_assert_uint_in_range(seen_sequences[i], 0, 4);
        for (int j = i + 1; j < 5; j++) {
            assert_int_not_equal(seen_sequences[i], seen_sequences[j]);
        }
    }

    sim_event_queue_destroy(&queue);
}

static void test_queue_atomic_dequeue(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_queue_init(&queue);

    /* Enqueue events */
    for (int i = 0; i < 3; i++) {
        sim_event_enqueue(&queue, &mock_units[i], i * 10,
                           (ACTIVATE_API)mock_activate, NULL);
    }

    /* Atomic exchange should drain the entire queue */
    sim_unit_event_t *drained = (sim_unit_event_t *)sim_atomic_ptr_exchange(&queue.head, NULL);
    assert_non_null(drained);

    /* Queue should now be empty */
    assert_null(sim_atomic_ptr_get(&queue.head));

    /* Count drained events */
    int count = 0;
    sim_unit_event_t *event = drained;
    while (event != NULL) {
        count++;
        sim_unit_event_t *next = event->next;
        free(event);
        event = next;
    }

    assert_int_equal(count, 3);

    sim_event_queue_destroy(&queue);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Min-Heap Tests
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static void test_heap_init(void **state)
{
    (void)state;
    sim_event_heap_t heap;

    sim_event_heap_init(&heap);

    assert_true(sim_event_heap_empty(&heap));
    assert_int_equal(sim_event_heap_count(&heap), 0);
    assert_null(heap.events);
    assert_int_equal(heap.capacity, 0);

    sim_event_heap_destroy(&heap);
}

static void test_heap_insert_single(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_heap_t heap;
    sim_event_heap_init(&heap);

    sim_unit_event_t *event = malloc(sizeof(sim_unit_event_t));
    event->unit = &mock_units[0];
    event->event_time = 100;
    event->sequence = 0;
    event->activate_call = (ACTIVATE_API)mock_activate;
    event->check_completion = NULL;

    bool result = sim_event_heap_insert(&heap, event);
    assert_true(result);
    assert_false(sim_event_heap_empty(&heap));
    assert_int_equal(sim_event_heap_count(&heap), 1);

    /* Peek should return the same event */
    sim_unit_event_t *peeked = sim_event_heap_peek(&heap);
    assert_ptr_equal(peeked, event);

    sim_event_heap_destroy(&heap);
}

static void test_heap_ordering_by_time(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_heap_t heap;
    sim_event_heap_init(&heap);

    /* Insert events with different times (not in order) */
    int times[] = {50, 10, 30, 20, 40};
    for (int i = 0; i < 5; i++) {
        sim_unit_event_t *event = malloc(sizeof(sim_unit_event_t));
        event->unit = &mock_units[i];
        event->event_time = times[i];
        event->sequence = i;
        event->activate_call = (ACTIVATE_API)mock_activate;
        event->check_completion = NULL;
        sim_event_heap_insert(&heap, event);
    }

    /* Extract events - should come out in time order */
    int expected_times[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) {
        sim_unit_event_t *event = sim_event_heap_extract_min(&heap);
        assert_non_null(event);
        assert_int_equal(event->event_time, expected_times[i]);
        free(event);
    }

    assert_true(sim_event_heap_empty(&heap));
    sim_event_heap_destroy(&heap);
}

static void test_heap_ordering_by_sequence(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_heap_t heap;
    sim_event_heap_init(&heap);

    /* Insert events with SAME time but different sequences */
    for (int i = 0; i < 5; i++) {
        sim_unit_event_t *event = malloc(sizeof(sim_unit_event_t));
        event->unit = &mock_units[i];
        event->event_time = 100;  /* Same time */
        event->sequence = 4 - i;   /* Reverse sequence order */
        event->activate_call = (ACTIVATE_API)mock_activate;
        event->check_completion = NULL;
        sim_event_heap_insert(&heap, event);
    }

    /* Extract events - should come out in sequence order */
    for (int i = 0; i < 5; i++) {
        sim_unit_event_t *event = sim_event_heap_extract_min(&heap);
        assert_non_null(event);
        assert_int_equal(event->event_time, 100);
        assert_int_equal(event->sequence, i);
        free(event);
    }

    assert_true(sim_event_heap_empty(&heap));
    sim_event_heap_destroy(&heap);
}

static void test_heap_extract_empty(void **state)
{
    (void)state;
    sim_event_heap_t heap;

    sim_event_heap_init(&heap);

    /* Extracting from empty heap should return NULL */
    sim_unit_event_t *event = sim_event_heap_extract_min(&heap);
    assert_null(event);

    sim_event_heap_destroy(&heap);
}

static void test_heap_peek_no_modify(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_heap_t heap;
    sim_event_heap_init(&heap);

    /* Insert events */
    for (int i = 0; i < 3; i++) {
        sim_unit_event_t *event = malloc(sizeof(sim_unit_event_t));
        event->unit = &mock_units[i];
        event->event_time = (2 - i) * 10;  /* 20, 10, 0 */
        event->sequence = i;
        event->activate_call = (ACTIVATE_API)mock_activate;
        event->check_completion = NULL;
        sim_event_heap_insert(&heap, event);
    }

    /* Peek multiple times - should return same event without modifying heap */
    sim_unit_event_t *first_peek = sim_event_heap_peek(&heap);
    assert_non_null(first_peek);
    assert_int_equal(first_peek->event_time, 0);
    assert_int_equal(sim_event_heap_count(&heap), 3);

    sim_unit_event_t *second_peek = sim_event_heap_peek(&heap);
    assert_ptr_equal(first_peek, second_peek);
    assert_int_equal(sim_event_heap_count(&heap), 3);

    sim_event_heap_destroy(&heap);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Integration Tests (Queue + Heap)
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static void test_migrate_to_heap(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_heap_t heap;

    sim_event_queue_init(&queue);
    sim_event_heap_init(&heap);

    /* Enqueue events */
    for (int i = 0; i < 5; i++) {
        sim_event_enqueue(&queue, &mock_units[i], i * 10,
                           (ACTIVATE_API)mock_activate, NULL);
    }

    /* Migrate to heap */
    int migrated = sim_event_migrate_to_heap(&queue, &heap);
    assert_int_equal(migrated, 5);
    assert_int_equal(sim_event_heap_count(&heap), 5);

    /* Queue should be empty */
    assert_null(sim_atomic_ptr_get(&queue.head));

    /* Events should be in heap, ordered by time */
    for (int i = 0; i < 5; i++) {
        sim_unit_event_t *event = sim_event_heap_extract_min(&heap);
        assert_non_null(event);
        assert_int_equal(event->event_time, i * 10);
    }

    sim_event_heap_destroy(&heap);
    sim_event_queue_destroy(&queue);
}

static void test_process_heap_by_time(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_heap_t heap;

    sim_event_queue_init(&queue);
    sim_event_heap_init(&heap);

    /* Enqueue events with different times */
    int times[] = {100, 50, 150, 75, 200};
    for (int i = 0; i < 5; i++) {
        sim_event_enqueue(&queue, &mock_units[i], times[i],
                           (ACTIVATE_API)mock_activate, NULL);
    }

    /* Migrate to heap */
    sim_event_migrate_to_heap(&queue, &heap);

    /* Process events up to time 100 */
    int processed = sim_event_process_heap(&heap, 100);

    /* Should process events at times 50, 75, 100 = 3 events */
    assert_int_equal(processed, 3);
    assert_int_equal(activation_count, 3);

    /* Remaining events should still be in heap (150, 200) */
    assert_int_equal(sim_event_heap_count(&heap), 2);

    sim_event_heap_destroy(&heap);
    sim_event_queue_destroy(&queue);
}

static void test_process_heap_deterministic_order(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_heap_t heap;

    sim_event_queue_init(&queue);
    sim_event_heap_init(&heap);

    /* Enqueue events with SAME time but different sequence order */
    /* Enqueue in reverse order: units 4, 3, 2, 1, 0 */
    for (int i = 4; i >= 0; i--) {
        sim_event_enqueue(&queue, &mock_units[i], 100,
                           (ACTIVATE_API)mock_activate, NULL);
    }

    sim_event_migrate_to_heap(&queue, &heap);

    /* Process all events */
    int processed = sim_event_process_heap(&heap, 100);
    assert_int_equal(processed, 5);

    /* Verify activation order matches sequence order (0, 1, 2, 3, 4)
     * NOT enqueue order (4, 3, 2, 1, 0) */
    for (int i = 0; i < 5; i++) {
        /* The sequence numbers were assigned in enqueue order (4->3->2->1->0)
         * So sequences are: unit[4]=0, unit[3]=1, unit[2]=2, unit[1]=3, unit[0]=4
         * But heap should process by sequence, so: seq 0 first (unit[4]), then seq 1 (unit[3]), etc. */
        assert_int_equal(activation_order[i], 4 - i);
    }

    sim_event_heap_destroy(&heap);
    sim_event_queue_destroy(&queue);
}

static void test_completion_check_called(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_heap_t heap;

    sim_event_queue_init(&queue);
    sim_event_heap_init(&heap);

    /* Enqueue events with completion check */
    for (int i = 0; i < 3; i++) {
        sim_event_enqueue(&queue, &mock_units[i], i * 10,
                           (ACTIVATE_API)mock_activate, mock_check_completion);
    }

    sim_event_migrate_to_heap(&queue, &heap);

    /* Process all events */
    sim_event_process_heap(&heap, 100);

    /* Verify all completion checks were called */
    assert_int_equal(completion_check_count, 3);
    assert_int_equal(activation_count, 3);

    sim_event_heap_destroy(&heap);
    sim_event_queue_destroy(&queue);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Multi-threaded Tests (MPSC verification)
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#if defined(SIM_ASYNCH_IO)

typedef struct producer_thread_arg_s {
    sim_event_mpsc_queue_t *queue;
    int thread_id;
    int events_count;  /* Number of events this thread will produce */
} producer_thread_arg_t;

/* Thread function using ZIMH threading macros */
static THREAD_FUNC_DEFN(producer_thread)
{
    producer_thread_arg_t *thread_arg = (producer_thread_arg_t *)arg;

    for (int i = 0; i < thread_arg->events_count; i++) {
        int unit_idx = thread_arg->thread_id % 10;
        sim_event_enqueue(thread_arg->queue, &mock_units[unit_idx],
                           i * 10, (ACTIVATE_API)mock_activate, NULL);
    }

    return THREAD_FUNC_RETURN(0);
}

static void test_mpsc_multiple_producers(void **state)
{
    (void)state;
    reset_mocks();

    sim_event_mpsc_queue_t queue;
    sim_event_heap_t heap;

    sim_event_queue_init(&queue);
    sim_event_heap_init(&heap);

    const int num_producers = 4;
    const int events_per_producer = 25;
    sim_thread_t threads[4];
    producer_thread_arg_t args[4];

    /* Start producer threads */
    for (int i = 0; i < num_producers; i++) {
        args[i].queue = &queue;
        args[i].thread_id = i;
        args[i].events_count = events_per_producer;
        sim_thread_create(&threads[i], producer_thread, &args[i]);
    }

    /* Wait for all producers to finish */
    for (int i = 0; i < num_producers; i++) {
        sim_thread_join(threads[i], NULL);
    }

    /* Migrate to heap */
    int migrated = sim_event_migrate_to_heap(&queue, &heap);
    assert_int_equal(migrated, num_producers * events_per_producer);

    /* Verify all sequence numbers are unique */
    bool seen[100] = {false};
    for (size_t i = 0; i < sim_event_heap_count(&heap); i++) {
        sim_unit_event_t *event = sim_event_heap_extract_min(&heap);
        assert_non_null(event);
        assert_true(event->sequence < 100);
        assert_false(seen[event->sequence]); /* No duplicate sequences */
        seen[event->sequence] = true;
        free(event);
    }

    sim_event_heap_destroy(&heap);
    sim_event_queue_destroy(&queue);
}

#endif /* SIM_ASYNCH_IO */

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Test Suite
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Queue tests */
        cmocka_unit_test(test_queue_init),
        cmocka_unit_test(test_queue_single_enqueue),
        cmocka_unit_test(test_queue_multiple_enqueue),
        cmocka_unit_test(test_queue_atomic_dequeue),

        /* Heap tests */
        cmocka_unit_test(test_heap_init),
        cmocka_unit_test(test_heap_insert_single),
        cmocka_unit_test(test_heap_ordering_by_time),
        cmocka_unit_test(test_heap_ordering_by_sequence),
        cmocka_unit_test(test_heap_extract_empty),
        cmocka_unit_test(test_heap_peek_no_modify),

        /* Integration tests */
        cmocka_unit_test(test_migrate_to_heap),
        cmocka_unit_test(test_process_heap_by_time),
        cmocka_unit_test(test_process_heap_deterministic_order),
        cmocka_unit_test(test_completion_check_called),

#if defined(SIM_ASYNCH_IO)
        /* Multi-threaded tests */
        cmocka_unit_test(test_mpsc_multiple_producers),
#endif
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
