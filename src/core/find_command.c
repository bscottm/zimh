
/*
 * find_command.c
 *
 * char *find_command_on_path(const char *command);
 *
 * Returns:
 *   - malloc()'d absolute path to the resolved command on success
 *   - NULL if the command cannot be found, is not executable, or allocation fails
 *
 * Caller must free() a non-NULL result.
 *
 * On Windows:
 *   - Searches using SearchPathA().
 *   - Tries PATHEXT for extensionless command names.
 *
 * On POSIX/Linux:
 *   - Searches PATH manually.
 *   - Requires a regular file with execute permission.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int has_path_separator(const char *s)
{
    return strchr(s, '\\') != NULL || strchr(s, '/') != NULL;
}

static int has_extension(const char *path)
{
    const char *base = path;

    for (const char *p = path; *p; ++p) {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }

    return strchr(base, '.') != NULL;
}

static int is_non_directory(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);

    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static char *duplicate_absolute_path(const char *path)
{
    DWORD needed;
    char *result;

    /*
     * GetFullPathNameA() returns the required length excluding the NUL when
     * the supplied buffer is too small.
     */
    needed = GetFullPathNameA(path, 0, NULL, NULL);
    if (needed == 0)
        return NULL;

    result = malloc((size_t)needed + 1);
    if (result == NULL)
        return NULL;

    if (GetFullPathNameA(path, needed + 1, result, NULL) == 0) {
        free(result);
        return NULL;
    }

    return result;
}

/*
 * SearchPathA has an awkward buffer-size convention:
 *
 * - Return 0: not found / error
 * - Return < buffer size: success, excluding terminating NUL
 * - Return >= buffer size: buffer was too small; return is required size
 */
static char *search_path(const char *command, const char *extension)
{
    DWORD needed;
    char *result;

    needed = SearchPathA(NULL, command, extension, 0, NULL, NULL);
    if (needed == 0)
        return NULL;

    result = malloc((size_t)needed + 1);
    if (result == NULL)
        return NULL;

    if (SearchPathA(NULL, command, extension, needed + 1, result, NULL) == 0) {
        free(result);
        return NULL;
    }

    if (!is_non_directory(result)) {
        free(result);
        return NULL;
    }

    /*
     * SearchPathA normally produces an absolute path, but canonicalize it
     * explicitly so the API always promises an absolute result.
     */
    {
        char *absolute = duplicate_absolute_path(result);
        free(result);
        return absolute;
    }
}

char *find_command_on_path(const char *command)
{
    char *found;

    if (command == NULL || *command == '\0')
        return NULL;

    /*
     * Direct-path mode: SearchPathA is not allowed to interpret a path-like
     * argument as a bare command name.
     */
    if (has_path_separator(command)) {
        if (!is_non_directory(command))
            return NULL;

        return duplicate_absolute_path(command);
    }

    /* Exact name first: "tool.exe", "script.cmd", etc. */
    found = search_path(command, NULL);
    if (found != NULL)
        return found;

    /* For "tool", try ".COM", ".EXE", ".BAT", ".CMD", etc. */
    if (!has_extension(command)) {
        // Normally, pathext would be ".COM;.EXE;.BAT;.CMD". ZIMH is VERY restrictive here for reasonably
        // obvious reasons -- hijacking via a CMD earlier in the PATH, for example.
        //
        // Code is written so that if this isn't a valid assumption or restriction, just add the semicolon
        // separated PATHEXT extensions.
        const char *pathext = ".EXE";
        char *extensions;
        char *token;
        char *context = NULL;

        extensions = _strdup(pathext);
        if (extensions == NULL)
            return NULL;

        for (token = strtok_s(extensions, ";", &context);
             token != NULL;
             token = strtok_s(NULL, ";", &context)) {

            found = search_path(command, token);
            if (found != NULL) {
                free(extensions);
                return found;
            }
        }

        free(extensions);
    }

    return NULL;
}

#else /* Linux / POSIX */

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_executable_regular_file(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 &&
           S_ISREG(st.st_mode) &&
           access(path, X_OK) == 0;
}

static char *make_absolute_path(const char *path)
{
    char *resolved;

    /*
     * realpath() returns a newly allocated canonical absolute path when its
     * second argument is NULL. It also resolves symlinks and removes "."/"..".
     */
    resolved = realpath(path, NULL);
    if (resolved == NULL)
        return NULL;

    return resolved;
}

static char *join_path(const char *directory, size_t directory_len,
                       const char *command)
{
    size_t command_len = strlen(command);
    size_t length;
    char *result;

    /*
     * An empty PATH component is the current working directory. Use "." here
     * so realpath() can convert it to an absolute path later.
     */
    if (directory_len == 0) {
        directory = ".";
        directory_len = 1;
    }

    length = directory_len + 1 + command_len + 1;
    result = malloc(length);
    if (result == NULL)
        return NULL;

    memcpy(result, directory, directory_len);
    result[directory_len] = '/';
    memcpy(result + directory_len + 1, command, command_len + 1);

    return result;
}

char *find_command_on_path(const char *command)
{
    const char *path;
    const char *component;

    if (command == NULL || *command == '\0')
        return NULL;

    /*
     * As with execvp(), a slash means the caller supplied a path rather than
     * a PATH-resolved command name.
     */
    if (strchr(command, '/') != NULL) {
        if (!is_executable_regular_file(command))
            return NULL;

        return make_absolute_path(command);
    }

    path = getenv("PATH");
    if (path == NULL)
        return NULL;

    component = path;

    for (;;) {
        const char *next = strchr(component, ':');
        size_t component_len =
            next ? (size_t)(next - component) : strlen(component);
        char *candidate =
            join_path(component, component_len, command);

        if (candidate == NULL)
            return NULL;

        if (is_executable_regular_file(candidate)) {
            char *absolute = make_absolute_path(candidate);
            free(candidate);
            return absolute;
        }

        free(candidate);

        if (next == NULL)
            break;

        component = next + 1;
    }

    return NULL;
}

#endif

#ifdef FIND_COMMAND_DEMO_MAIN

int main(int argc, char **argv)
{
    char *path;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s COMMAND\n", argv[0]);
        return 2;
    }

    path = find_command_on_path(argv[1]);

    if (path == NULL) {
        printf("%s: not found\n", argv[1]);
        return 1;
    }

    printf("%s\n", path);
    free(path);
    return 0;
}

#endif
