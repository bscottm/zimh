// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#include <cmocka.h>

#include "sim_defs.h"
#include "sim_threads.h"

/* Platform-specific includes for verification */
#if defined(_WIN32) || defined(_WIN64)
#    include <windows.h>
#    if defined(HAVE_SETTHREADDESCRIPTION)
#        include <processthreadsapi.h>
#    endif
#elif defined(HAVE_PTHREADS)
#    include <pthread.h>
#    if defined(__linux__)
#        include <sys/prctl.h>
#    elif defined(__FreeBSD__) || defined(__OpenBSD__)
#        include <pthread_np.h>
#    endif
#endif

/* Test: NULL name should not crash */
static void test_set_thread_name_null(void **state)
{
    (void)state; /* unused */

    /* Should handle NULL gracefully */
    sim_set_thread_name(NULL);

    /* If we get here, test passes */
    assert_true(1);
}

/* Test: Empty string should not crash */
static void test_set_thread_name_empty(void **state)
{
    (void)state; /* unused */

    /* Should handle empty string gracefully */
    sim_set_thread_name("");

    /* If we get here, test passes */
    assert_true(1);
}

/* Test: Normal thread name */
static void test_set_thread_name_normal(void **state)
{
    (void)state; /* unused */

    const char *test_name = "TestThread";

    /* Should succeed without crashing */
    sim_set_thread_name(test_name);

    /* Platform-specific verification where possible */
#if defined(__linux__) && defined(HAVE_PRCTL_SET_NAME)
    char retrieved_name[16] = {0};
    prctl(PR_GET_NAME, retrieved_name, 0, 0, 0);
    assert_string_equal(retrieved_name, test_name);
#elif defined(__APPLE__) && defined(__MACH__)
    char retrieved_name[64] = {0};
    pthread_getname_np(pthread_self(), retrieved_name, sizeof(retrieved_name));
    assert_string_equal(retrieved_name, test_name);
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    /* FreeBSD and NetBSD also support pthread_getname_np */
    char retrieved_name[64] = {0};
    pthread_getname_np(pthread_self(), retrieved_name, sizeof(retrieved_name));
    assert_string_equal(retrieved_name, test_name);
#else
    /* On platforms without verification, just ensure no crash */
    assert_true(1);
#endif
}

/* Test: Long thread name (should truncate) */
static void test_set_thread_name_long(void **state)
{
    (void)state; /* unused */

    /* Create a name longer than any platform's limit */
    const char *long_name = "ThisIsAVeryLongThreadNameThatExceedsThePlatformMaximumLength123456789";

    /* Should handle truncation gracefully */
    sim_set_thread_name(long_name);

    /* Platform-specific verification */
#if defined(__linux__) && defined(HAVE_PRCTL_SET_NAME)
    char retrieved_name[16] = {0};
    prctl(PR_GET_NAME, retrieved_name, 0, 0, 0);

    /* Linux truncates to 15 chars + null terminator */
    assert_int_equal(strlen(retrieved_name), 15);
    assert_memory_equal(retrieved_name, long_name, 15);

#elif defined(__APPLE__) && defined(__MACH__)
    char retrieved_name[64] = {0};
    pthread_getname_np(pthread_self(), retrieved_name, sizeof(retrieved_name));

    /* macOS should have the full name (within 64 char limit) */
    size_t expected_len = strlen(long_name) < 63 ? strlen(long_name) : 63;
    assert_int_equal(strlen(retrieved_name), expected_len);

#else
    /* Just ensure no crash on other platforms */
    assert_true(1);
#endif
}

/* Test: Special characters in thread name */
static void test_set_thread_name_special_chars(void **state)
{
    (void)state; /* unused */

    const char *special_name = "Test-Thread_123";

    /* Should handle special characters */
    sim_set_thread_name(special_name);

#if defined(__linux__) && defined(HAVE_PRCTL_SET_NAME)
    char retrieved_name[16] = {0};
    prctl(PR_GET_NAME, retrieved_name, 0, 0, 0);
    assert_string_equal(retrieved_name, special_name);
#else
    /* Just ensure no crash */
    assert_true(1);
#endif
}

/* Test: Unicode/UTF-8 characters (if supported) */
static void test_set_thread_name_unicode(void **state)
{
    (void)state; /* unused */

    /* UTF-8 encoded string with emoji */
    const char *unicode_name = "Test🧵Thread";

    /* Should handle UTF-8 gracefully (may truncate or transliterate) */
    sim_set_thread_name(unicode_name);

    /* We can't reliably verify Unicode handling across platforms,
     * but we can ensure it doesn't crash */
    assert_true(1);
}

/* Test: Repeated calls (should be idempotent) */
static void test_set_thread_name_repeated(void **state)
{
    (void)state; /* unused */

    const char *name1 = "FirstName";
    const char *name2 = "SecondName";

    /* Set name multiple times */
    sim_set_thread_name(name1);
    sim_set_thread_name(name2);
    sim_set_thread_name(name1);

    /* Verify final name */
#if defined(__linux__) && defined(HAVE_PRCTL_SET_NAME)
    char retrieved_name[16] = {0};
    prctl(PR_GET_NAME, retrieved_name, 0, 0, 0);
    assert_string_equal(retrieved_name, name1);
#else
    assert_true(1);
#endif
}

/* Test: Whitespace in thread name */
static void test_set_thread_name_whitespace(void **state)
{
    (void)state; /* unused */

    const char *name_with_space = "Test Thread";

    sim_set_thread_name(name_with_space);

#if defined(__linux__) && defined(HAVE_PRCTL_SET_NAME)
    char retrieved_name[16] = {0};
    prctl(PR_GET_NAME, retrieved_name, 0, 0, 0);
    assert_string_equal(retrieved_name, name_with_space);
#else
    assert_true(1);
#endif
}

/* Test group setup/teardown (if needed) */
static int group_setup(void **state)
{
    (void)state; /* unused */
    return 0;
}

static int group_teardown(void **state)
{
    (void)state; /* unused */
    return 0;
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_set_thread_name_null),          cmocka_unit_test(test_set_thread_name_empty),
        cmocka_unit_test(test_set_thread_name_normal),        cmocka_unit_test(test_set_thread_name_long),
        cmocka_unit_test(test_set_thread_name_special_chars), cmocka_unit_test(test_set_thread_name_unicode),
        cmocka_unit_test(test_set_thread_name_repeated),      cmocka_unit_test(test_set_thread_name_whitespace),
    };

    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
