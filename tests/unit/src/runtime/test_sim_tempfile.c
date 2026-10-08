/* test_sim_tempfile.c: tests for simulator temporary file helpers */

#include <errno.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_host_path_internal.h"
#include "sim_tempfile.h"
#include "sim_win32_compat.h"
#include "string_compat.h"
#include "test_cmocka.h"

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static char test_temp_dir[128];

#if defined(_WIN32)
static unsigned long test_win32_temp_path(unsigned long size, char *buf)
{
    size_t need = strlen(test_temp_dir) + 2;

    if (need > size)
        return (unsigned long)need;

    strlcpy(buf, test_temp_dir, size);
    strlcat(buf, "\\", size);
    return (unsigned long)(need - 1);
}
#endif

static int test_process_id(void)
{
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

static int test_mkdir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static FILE *test_fdopen(int fd, const char *mode)
{
#if defined(_WIN32)
    return _fdopen(fd, mode);
#else
    return fdopen(fd, mode);
#endif
}

static const char *test_base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');

    if (slash == NULL)
        return backslash == NULL ? path : backslash + 1;
    if (backslash == NULL)
        return slash + 1;
    return slash > backslash ? slash + 1 : backslash + 1;
}

static int test_has_suffix(const char *text, const char *suffix)
{
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);

    if (text_len < suffix_len)
        return 0;
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

static int setup_temp_dir(void **state)
{
    (void)state;

    snprintf(test_temp_dir, sizeof(test_temp_dir), "zimh-tempfile-test-%d",
             test_process_id());
    assert_int_equal(test_mkdir(test_temp_dir), 0);

#if defined(_WIN32)
    sim_host_path_set_test_win32_temp_hooks(test_win32_temp_path,
                                            test_win32_temp_path);
#else
    assert_int_equal(setenv("TMPDIR", test_temp_dir, 1), 0);
#endif

    return 0;
}

static int teardown_temp_dir(void **state)
{
    (void)state;

#if defined(_WIN32)
    sim_host_path_reset_test_hooks();
#else
    unsetenv("TMPDIR");
#endif

    assert_int_equal(rmdir(test_temp_dir), 0);
    return 0;
}

static void test_sim_tempfile_open_creates_read_write_file(void **state)
{
    char path[512];
    char buffer[8] = {0};
    const char *base;
    FILE *stream;
    int fd;

    (void)state;

    fd = sim_tempfile_open(path, sizeof(path), "zimh-temp-", ".dat");
    assert_true(fd >= 0);

    base = test_base_name(path);
    assert_ptr_not_equal(base, path);
    assert_int_equal(strncmp(base, "zimh-temp-", strlen("zimh-temp-")), 0);
    assert_true(test_has_suffix(base, ".dat"));
    assert_int_equal(access(path, F_OK), 0);

    stream = test_fdopen(fd, "w+b");
    assert_non_null(stream);
    assert_int_equal(fwrite("abc", 1, 3, stream), 3);
    assert_int_equal(fseek(stream, 0, SEEK_SET), 0);
    assert_int_equal(fread(buffer, 1, 3, stream), 3);
    assert_string_equal(buffer, "abc");
    assert_int_equal(fclose(stream), 0);

    assert_int_equal(unlink(path), 0);
}

static void test_sim_tempfile_open_accepts_default_affixes(void **state)
{
    char path[512];
    int fd;

    (void)state;

    fd = sim_tempfile_open(path, sizeof(path), NULL, NULL);
    assert_true(fd >= 0);
    assert_ptr_not_equal(test_base_name(path), path);
    assert_int_equal(close(fd), 0);
    assert_int_equal(unlink(path), 0);
}

static void test_sim_tempfile_open_stream_creates_read_write_file(void **state)
{
    char path[512];
    char buffer[8] = {0};
    const char *base;
    FILE *stream;

    (void)state;

    stream = sim_tempfile_open_stream(path, sizeof(path), "zimh-stream-",
                                      ".txt", "w+b");
    assert_non_null(stream);

    base = test_base_name(path);
    assert_ptr_not_equal(base, path);
    assert_int_equal(strncmp(base, "zimh-stream-", strlen("zimh-stream-")), 0);
    assert_true(test_has_suffix(base, ".txt"));
    assert_int_equal(access(path, F_OK), 0);

    assert_int_equal(fwrite("def", 1, 3, stream), 3);
    assert_int_equal(fseek(stream, 0, SEEK_SET), 0);
    assert_int_equal(fread(buffer, 1, 3, stream), 3);
    assert_string_equal(buffer, "def");
    assert_int_equal(fclose(stream), 0);

    assert_int_equal(unlink(path), 0);
}

static void test_sim_tmpfile_creates_read_write_stream(void **state)
{
    char buffer[8] = {0};
    FILE *stream;

    (void)state;

    stream = sim_tmpfile();
    assert_non_null(stream);

    assert_int_equal(fwrite("ghi", 1, 3, stream), 3);
    assert_int_equal(fseek(stream, 0, SEEK_SET), 0);
    assert_int_equal(fread(buffer, 1, 3, stream), 3);
    assert_string_equal(buffer, "ghi");
    assert_int_equal(fclose(stream), 0);
}

/*
 * A tight loop of sim_tempfile_open() calls from the same process, at
 * the same stack address, within the same clock second previously
 * started every attempt from an identical first candidate name,
 * relying entirely on EEXIST retries to find a free one. Mix in a
 * per-process counter so repeated calls land on distinct names up
 * front instead of colliding every time.
 */
static void test_sim_tempfile_open_generates_distinct_names_in_a_loop(
    void **state)
{
    enum { COUNT = 64 };
    char paths[COUNT][512];
    int fds[COUNT];
    int i, j;

    (void)state;

    for (i = 0; i < COUNT; i++) {
        fds[i] = sim_tempfile_open(paths[i], sizeof(paths[i]), "zimh-dup-",
                                   NULL);
        assert_true(fds[i] >= 0);
    }

    for (i = 0; i < COUNT; i++)
        for (j = i + 1; j < COUNT; j++)
            assert_string_not_equal(paths[i], paths[j]);

    for (i = 0; i < COUNT; i++) {
        assert_int_equal(close(fds[i]), 0);
        assert_int_equal(unlink(paths[i]), 0);
    }
}

static void test_sim_tempfile_open_rejects_invalid_arguments(void **state)
{
    char path[512];

    (void)state;

    errno = 0;
    assert_int_equal(sim_tempfile_open(NULL, sizeof(path), "zimh-", NULL), -1);
    assert_int_equal(errno, EINVAL);

    errno = 0;
    assert_int_equal(sim_tempfile_open(path, 0, "zimh-", NULL), -1);
    assert_int_equal(errno, EINVAL);
}

static void test_sim_tempfile_open_stream_rejects_invalid_mode(void **state)
{
    char path[512];

    (void)state;

    errno = 0;
    assert_null(
        sim_tempfile_open_stream(path, sizeof(path), "zimh-", NULL, NULL));
    assert_int_equal(errno, EINVAL);
}

static void test_sim_tempfile_open_rejects_short_buffer(void **state)
{
    char path[4];

    (void)state;

    errno = 0;
    assert_int_equal(sim_tempfile_open(path, sizeof(path), "zimh-", NULL), -1);
    assert_int_equal(errno, ENAMETOOLONG);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_sim_tempfile_open_creates_read_write_file),
        cmocka_unit_test(test_sim_tempfile_open_accepts_default_affixes),
        cmocka_unit_test(test_sim_tempfile_open_stream_creates_read_write_file),
        cmocka_unit_test(test_sim_tmpfile_creates_read_write_stream),
        cmocka_unit_test(
            test_sim_tempfile_open_generates_distinct_names_in_a_loop),
        cmocka_unit_test(test_sim_tempfile_open_rejects_invalid_arguments),
        cmocka_unit_test(
            test_sim_tempfile_open_stream_rejects_invalid_mode),
        cmocka_unit_test(test_sim_tempfile_open_rejects_short_buffer),
    };

    return cmocka_run_group_tests(tests, setup_temp_dir, teardown_temp_dir);
}
