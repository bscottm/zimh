// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <cmocka_version.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "sim_platform.h"
#include "sim_tailq.h"
#include "sim_threads.h"


#if defined(CMOCKA_VERSION_MAJOR) && CMOCKA_VERSION_MAJOR >= 2
#    define sim_assert_int_in_range(val, min, max) assert_int_in_range((val), (min), (max))
#    define sim_assert_uint_in_range(val, min, max) assert_uint_in_range((val), (min), (max))
#else
#    define sim_assert_int_in_range(val, min, max) assert_in_range((val), (min), (max))
#    define sim_assert_uint_in_range(val, min, max) assert_in_range((val), (min), (max))
#endif

const uintmax_t MAX_ITERATIONS = 10000000;

/* Test data structure */
typedef struct test_item_s {
    int value;
    uint32_t checksum; /* For data integrity validation */
    char name[32];
} test_item_t;

/* Helper: compute checksum for integrity validation */
static uint32_t compute_checksum(const test_item_t *item)
{
    uint32_t sum = (uint32_t)item->value;
    for (size_t i = 0; i < sizeof(item->name); i++) {
        sum = sum * 31 + (uint32_t)item->name[i];
    }
    return sum;
}

/* Helper: create test item with checksum */
static test_item_t *create_test_item(int value, const char *name)
{
    test_item_t *item = malloc(sizeof(test_item_t));
    assert_non_null(item);
    item->value = value;
    strncpy(item->name, name, sizeof(item->name) - 1);
    item->name[sizeof(item->name) - 1] = '\0';
    item->checksum = compute_checksum(item);
    return item;
}

/* Helper: validate test item integrity */
static void validate_test_item(const test_item_t *item)
{
    assert_non_null(item);
    if (item->checksum != compute_checksum(item)) {
        printf("item checksum mismatch: %s\n", item->name);
        fflush(stdout);
    }
    
    assert_int_equal(item->checksum, compute_checksum(item));
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Basic Functionality Tests
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static void test_tailq_init(void **state)
{
    (void)state;
    sim_tailq_t *queue;

    queue = sim_tailq_init();
    assert_non_null(queue);

    /* Queue should be empty after initialization */
    assert_int_equal(sim_tailq_count(queue), 0);
    assert_int_equal(sim_tailq_empty(queue), 1);

    /* Should have allocated initial nodes */
    assert_true(sim_tailq_allocated(queue) > 0);

    sim_tailq_destroy(queue, NULL);
}

static void test_tailq_single_enqueue_dequeue(void **state)
{
    (void)state;
    sim_tailq_t *queue;

    queue = sim_tailq_init();
    assert_non_null(queue);

    test_item_t *item = create_test_item(42, "test_item");

    assert_non_null(sim_tailq_enqueue(queue, (sim_tailq_item_t)item));
    assert_int_equal(sim_tailq_count(queue), 1);
    assert_int_equal(sim_tailq_empty(queue), 0);

    test_item_t *retrieved = (test_item_t *)sim_tailq_dequeue(queue);
    assert_non_null(retrieved);
    assert_ptr_equal(retrieved, item);
    assert_int_equal(retrieved->value, 42);
    assert_string_equal(retrieved->name, "test_item");
    validate_test_item(retrieved);

    assert_int_equal(sim_tailq_count(queue), 0);
    assert_int_equal(sim_tailq_empty(queue), 1);

    free(item);
    sim_tailq_destroy(queue, NULL);
}

static void test_tailq_multiple_enqueue_dequeue(void **state)
{
    (void)state;
    sim_tailq_t *queue;
    test_item_t *items[10];

    queue = sim_tailq_init();
    assert_non_null(queue);

    /* Enqueue 10 items */
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof(name), "item_%d", i);
        items[i] = create_test_item(i * 100, name);
        assert_non_null(sim_tailq_enqueue(queue, (sim_tailq_item_t)items[i]));
    }

    assert_int_equal(sim_tailq_count(queue), 10);

    /* Dequeue and verify FIFO order */
    for (int i = 0; i < 10; i++) {
        test_item_t *retrieved = (test_item_t *)sim_tailq_dequeue(queue);
        assert_non_null(retrieved);
        assert_ptr_equal(retrieved, items[i]);
        assert_int_equal(retrieved->value, i * 100);
        validate_test_item(retrieved);
        free(retrieved);
    }

    assert_int_equal(sim_tailq_count(queue), 0);
    assert_int_equal(sim_tailq_empty(queue), 1);

    sim_tailq_destroy(queue, NULL);
}

static void test_tailq_dequeue_empty(void **state)
{
    (void)state;
    sim_tailq_t *queue;

    queue = sim_tailq_init();
    assert_non_null(queue);

    /* Dequeue from empty queue should return NULL */
    assert_null(sim_tailq_dequeue(queue));

    /* Multiple dequeues should all return NULL */
    for (int i = 0; i < 5; i++) {
        assert_null(sim_tailq_dequeue(queue));
    }

    sim_tailq_destroy(queue, NULL);
}

/* Transform function that creates a new item */
SIM_UNUSED_FUNC
static sim_tailq_item_t create_item(sim_tailq_item_t ignored, void *arg)
{
    (void)ignored;
    int value = *(int *)arg;
    char name[32];
    snprintf(name, sizeof(name), "xform_%d", value);
    return (sim_tailq_item_t)create_test_item(value, name);
}

static int freed_count = 0;

static void count_free(sim_tailq_item_t item)
{
    if (item != NULL) {
        free(item);
        freed_count++;
    }
}

static void test_tailq_destroy_with_callback(void **state)
{
    (void)state;
    sim_tailq_t *queue;

    queue = sim_tailq_init();
    assert_non_null(queue);

    /* Enqueue some items */
    for (int i = 0; i < 5; i++) {
        test_item_t *item = create_test_item(i, "test");
        sim_tailq_enqueue(queue, (sim_tailq_item_t)item);
    }

    /* Destroy should call callback for each item */
    freed_count = 0;
    sim_tailq_destroy(queue, count_free);

    /* Should have freed at least the 5 items we enqueued */
    assert_true(freed_count >= 5);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * SPSC Concurrent Tests
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

typedef struct producer_args_s {
    sim_tailq_t *queue;
    int start_value;
    int count;
    sim_atomic_value_t *start_flag;
    sim_atomic_value_t *producer_done;
} producer_args_t;

typedef struct consumer_args_s {
    sim_tailq_t *queue;
    int expected_count;
    int *received_values;
    int *received_count;
    sim_atomic_value_t *start_flag;
    sim_atomic_value_t *producer_done;
} consumer_args_t;

THREAD_FUNC_DEFN(producer_thread)
{
    producer_args_t *args = (producer_args_t *)arg;

    sim_set_thread_name("TestProducer");

    /* Wait for start signal */
    while (sim_atomic_get(args->start_flag) == 0) {
        sim_thread_yield();
    }

    uintmax_t spin_count = 0;

    print_message("producer_thread starting\n");
    fflush(stdout);
    for (int i = 0; i < args->count; i++) {
        /* Allocate item dynamically */
        char name[32];
        snprintf(name, sizeof(name), "prod_%d", i);
        test_item_t *item = create_test_item(args->start_value + i, name);

        /* Debug: verify item before enqueue */
        if (i < 10 || i == args->count - 1) {
            printf("producer: about to enqueue item %d, value=%d, ptr=%p\n", i, item->value, (void*)item);
            fflush(stdout);
        }

        while (spin_count < MAX_ITERATIONS) {
            sim_tailq_t *result = sim_tailq_enqueue(args->queue, (sim_tailq_item_t)item);
            /* Might be temporarily full... */
            if (result == NULL) {
                if (++spin_count % 1000 == 0) {
                    /* Let consumer make progress. */
                    printf("producer spin: count = %" PRIuMAX "\n", spin_count);
                    fflush(stdout);
                }

                sim_thread_yield();
            } else {
                spin_count = 0;
                if (i < 10 || i == args->count - 1) {
                    printf("producer: enqueued item %d successfully\n", i);
                    fflush(stdout);
                }
                break;
            }
        }

        assert_true(spin_count < MAX_ITERATIONS);
    }

    /* Signal completion to consumer with RELEASE semantics to ensure all enqueues are visible */
    sim_atomic_put(args->producer_done, 1);
    print_message("producer_thread exiting\n");
    fflush(stdout);
    return THREAD_FUNC_RETURN(0);
}

THREAD_FUNC_DEFN(consumer_thread)
{
    consumer_args_t *args = (consumer_args_t *)arg;
    int count = 0;
    int spin_count = 0;

    sim_set_thread_name("TestConsumer");

    /* Wait for start signal */
    while (sim_atomic_get(args->start_flag) == 0) {
        sim_thread_yield();
    }

    print_message("consumer_thread starting\n");
    fflush(stdout);
    while (count < args->expected_count) {
        test_item_t *item = (test_item_t *)sim_tailq_dequeue(args->queue);
        if (item != NULL) {
            /* Debug: print first few and problem area */
            if (count < 10 || (count >= 170 && count <= 180) || count == args->expected_count - 1) {
                printf("consumer: dequeued item at position %d, value=%d, ptr=%p\n", count, item->value, (void*)item);
                fflush(stdout);
            }
            /* Validate, copy data, then free the item */
            validate_test_item(item);
            args->received_values[count] = item->value;
            free(item);  /* Free the dynamically allocated item */
            count++;
            spin_count = 0; /* Reset spin counter on success */
            /* Periodic progress indicator */
            if (count % 1000 == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "consumer: processed %d items\n", count);
                print_message(msg);
                fflush(stdout);
            }
        } else {
            spin_count++;
            if (spin_count % 1000 == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "consumer spin %d, count=%d/%d, done=%ld\n",
                         spin_count, count, args->expected_count, sim_atomic_get(args->producer_done));
                print_message(msg);
                fflush(stdout);
            }
            /* Detect infinite spin - consumer should not spin more than a reasonable amount */
            if (spin_count > 10000000 && sim_atomic_get(args->producer_done)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "ERROR: Consumer spinning forever after processing %d/%d items, producer is done\n",
                         count, args->expected_count);
                print_message(msg);
                fflush(stdout);
                abort();
            }

            /* Allow produce to make progress. */
            sim_thread_yield();
        }
    }

    *args->received_count = count;
    print_message("consumer_thread exiting\n");
    fflush(stdout);
    print_message("consumer_thread: about to return\n");
    fflush(stdout);
    return THREAD_FUNC_RETURN(0);
}

static void test_tailq_single_producer_single_consumer(void **state)
{
    (void)state;
    sim_tailq_t *queue;
    sim_thread_t prod_thread, cons_thread;

    const int ITEM_COUNT = 1000;

    queue = sim_tailq_init();
    assert_non_null(queue);

    sim_atomic_value_t start_flag;
    sim_atomic_init(&start_flag);

    sim_atomic_value_t producer_done;
    sim_atomic_init(&producer_done);

    producer_args_t prod_args = {.queue = queue, .start_value = 0, .count = ITEM_COUNT,
                                 .start_flag = &start_flag, .producer_done = &producer_done};

    int *received_values = calloc(ITEM_COUNT, sizeof(int));
    int received_count = 0;

    consumer_args_t cons_args = {.queue = queue,
                                 .expected_count = ITEM_COUNT,
                                 .received_values = received_values,
                                 .received_count = &received_count,
                                 .start_flag = &start_flag,
                                 .producer_done = &producer_done};

    print_message("Creating threads\n");
    fflush(stdout);
    /* Create threads */
    assert_int_equal(sim_thread_create(&cons_thread, consumer_thread, &cons_args), 0);
    assert_int_equal(sim_thread_create(&prod_thread, producer_thread, &prod_args), 0);

    /* Start both threads simultaneously */
    print_message("Starting threads\n");
    fflush(stdout);
    sim_atomic_put(&start_flag, 1);

    print_message("Waiting for producer\n");
    fflush(stdout);
    sim_thread_join(prod_thread, NULL);
    print_message("Waiting for consumer\n");
    fflush(stdout);
    sim_thread_join(cons_thread, NULL);

    print_message("Threads joined\n");
    fflush(stdout);

    /* Verify all items received */
    assert_int_equal(received_count, ITEM_COUNT);

    /* Verify FIFO order */
    for (int i = 0; i < ITEM_COUNT; i++) {
        assert_int_equal(received_values[i], i);
    }

    assert_int_equal(sim_tailq_empty(queue), 1);

    free(received_values);
    sim_tailq_destroy(queue, NULL);
}

static void test_tailq_stress_test(void **state)
{
    (void)state;
    sim_tailq_t *queue;
    sim_thread_t prod_thread, cons_thread;

    const int ITEM_COUNT = 10000;

    print_message("stress: Starting test\n");
    fflush(stdout);
    queue = sim_tailq_init();
    assert_non_null(queue);
    print_message("stress: Queue initialized\n");
    fflush(stdout);

    sim_atomic_value_t start_flag;
    sim_atomic_init(&start_flag);
    sim_atomic_put(&start_flag, 0);
    print_message("stress: start_flag initialized\n");
    fflush(stdout);

    sim_atomic_value_t producer_done;
    sim_atomic_init(&producer_done);
    sim_atomic_put(&producer_done, 0);
    print_message("stress: producer_done initialized\n");
    fflush(stdout);

    producer_args_t prod_args = {.queue = queue, .start_value = 0, .count = ITEM_COUNT,
                                 .start_flag = &start_flag, .producer_done = &producer_done};

    int *received_values = calloc(ITEM_COUNT, sizeof(int));
    print_message("stress: received_values allocated\n");
    fflush(stdout);

    int received_count = 0;

    consumer_args_t cons_args = {.queue = queue,
                                 .expected_count = ITEM_COUNT,
                                 .received_values = received_values,
                                 .received_count = &received_count,
                                 .start_flag = &start_flag,
                                 .producer_done = &producer_done};

    print_message("stress: Creating threads\n");
    fflush(stdout);
    assert_int_equal(sim_thread_create(&prod_thread, producer_thread, &prod_args), 0);
    assert_int_equal(sim_thread_create(&cons_thread, consumer_thread, &cons_args), 0);

    print_message("stress: Starting threads\n");
    fflush(stdout);
    sim_atomic_put(&start_flag, 1);

    print_message("stress: Waiting for producer\n");
    fflush(stdout);
    sim_thread_join(prod_thread, NULL);
    print_message("stress: Producer joined\n");
    fflush(stdout);
    print_message("stress: Waiting for consumer\n");
    fflush(stdout);
    sim_thread_join(cons_thread, NULL);
    print_message("stress: Consumer joined\n");
    fflush(stdout);

    print_message("stress: Threads joined\n");
    fflush(stdout);

    assert_int_equal(received_count, ITEM_COUNT);

    /* Check for duplicates and missing values */
    int *seen = calloc(ITEM_COUNT, sizeof(int));
    for (int i = 0; i < ITEM_COUNT; i++) {
        int val = received_values[i];
        sim_assert_int_in_range(val, 0, ITEM_COUNT - 1);
        assert_int_equal(seen[val], 0); /* No duplicates */
        seen[val] = 1;
    }

    /* Verify all values received */
    for (int i = 0; i < ITEM_COUNT; i++) {
        assert_int_equal(seen[i], 1);
    }

    free(seen);
    free(received_values);
    sim_tailq_destroy(queue, NULL);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Edge Case Tests
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static void test_tailq_alternating_enqueue_dequeue(void **state)
{
    (void)state;
    sim_tailq_t *queue;

    queue = sim_tailq_init();
    assert_non_null(queue);

    /* Alternate enqueue/dequeue to test ring reuse */
    for (int i = 0; i < 100; i++) {
        test_item_t *item = create_test_item(i, "alternate");
        assert_non_null(sim_tailq_enqueue(queue, (sim_tailq_item_t)item));

        test_item_t *retrieved = (test_item_t *)sim_tailq_dequeue(queue);
        assert_non_null(retrieved);
        assert_int_equal(retrieved->value, i);
        validate_test_item(retrieved);
        free(retrieved);

        assert_int_equal(sim_tailq_empty(queue), 1);
    }

    sim_tailq_destroy(queue, NULL);
}

static void test_tailq_grow_beyond_initial_allocation(void **state)
{
    (void)state;
    sim_tailq_t *queue;
#define LARGE_COUNT 100 /* More than INITIAL_TAILQ_NODES (17) */

    queue = sim_tailq_init();
    assert_non_null(queue);

    test_item_t *items[LARGE_COUNT];
    for (int i = 0; i < LARGE_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "large_%d", i);
        items[i] = create_test_item(i, name);
        assert_non_null(sim_tailq_enqueue(queue, (sim_tailq_item_t)items[i]));
    }

    assert_int_equal(sim_tailq_count(queue), LARGE_COUNT);
    assert_true(sim_tailq_allocated(queue) >= LARGE_COUNT);

    for (int i = 0; i < LARGE_COUNT; i++) {
        test_item_t *retrieved = (test_item_t *)sim_tailq_dequeue(queue);
        assert_non_null(retrieved);
        assert_int_equal(retrieved->value, i);
        validate_test_item(retrieved);
        free(retrieved);
    }

    sim_tailq_destroy(queue, NULL);
#undef LARGE_COUNT
}

static void test_tailq_ring_wraparound(void **state)
{
    (void)state;
    sim_tailq_t *queue;
    const int CYCLES = 5;
    const int ITEMS_PER_CYCLE = 20;

    queue = sim_tailq_init();
    assert_non_null(queue);

    for (int cycle = 0; cycle < CYCLES; cycle++) {
        /* Fill */
        for (int i = 0; i < ITEMS_PER_CYCLE; i++) {
            int value = cycle * ITEMS_PER_CYCLE + i;
            test_item_t *item = create_test_item(value, "wrap");
            assert_non_null(sim_tailq_enqueue(queue, (sim_tailq_item_t)item));
        }

        assert_int_equal(sim_tailq_count(queue), ITEMS_PER_CYCLE);

        /* Drain */
        for (int i = 0; i < ITEMS_PER_CYCLE; i++) {
            test_item_t *item = (test_item_t *)sim_tailq_dequeue(queue);
            int expected_value = cycle * ITEMS_PER_CYCLE + i;
            assert_non_null(item);
            assert_int_equal(item->value, expected_value);
            validate_test_item(item);
            free(item);
        }

        assert_int_equal(sim_tailq_empty(queue), 1);
    }

    sim_tailq_destroy(queue, NULL);
}

static void test_tailq_null_item(void **state)
{
    (void)state;
    sim_tailq_t *queue;

    queue = sim_tailq_init();

    /* Enqueue NULL item */
    assert_non_null(sim_tailq_enqueue(queue, NULL));
    assert_int_equal(sim_tailq_count(queue), 1);

    /* Dequeue should return NULL */
    sim_tailq_item_t item = sim_tailq_dequeue(queue);
    assert_null(item);

    assert_int_equal(sim_tailq_empty(queue), 1);

    sim_tailq_destroy(queue, NULL);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Main Test Runner
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Basic functionality */
        cmocka_unit_test(test_tailq_init),
        cmocka_unit_test(test_tailq_single_enqueue_dequeue),
        cmocka_unit_test(test_tailq_multiple_enqueue_dequeue),
        cmocka_unit_test(test_tailq_dequeue_empty),
        cmocka_unit_test(test_tailq_destroy_with_callback),

        /* SPSC concurrent tests */
        cmocka_unit_test(test_tailq_single_producer_single_consumer),
        cmocka_unit_test(test_tailq_stress_test),

        /* Edge cases */
        cmocka_unit_test(test_tailq_alternating_enqueue_dequeue),
        cmocka_unit_test(test_tailq_grow_beyond_initial_allocation),
        cmocka_unit_test(test_tailq_ring_wraparound),
        cmocka_unit_test(test_tailq_null_item),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
