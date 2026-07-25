/* sim_raw_osfile.c: "Raw" (low-level) operating system file primitives */

// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: X11

#include "sim_defs.h"
#include "sim_raw_osfile.h"

/* ============================================================================
 * Atomic disk I/O operations
 * ============================================================================
 *
 * Platform-abstracted atomic seek+read and seek+write operations.
 *
 * These functions provide pread()/pwrite() semantics across platforms:
 * - POSIX: Uses pread()/pwrite() directly
 * - Windows: Uses ReadFile()/WriteFile() with OVERLAPPED structure
 *
 * Both implementations provide atomic positioning without modifying the
 * file's global position pointer, eliminating the need for serialization
 * via threading.
 */

/* sim_disk_pread - atomic positioned read
 *
 * Reads 'bytes' bytes from 'handle' at 'offset' into 'buf'.
 * Does not modify the file's current position pointer.
 *
 * Returns: number of bytes actually read, or (size_t)-1 on error
 */
size_t sim_disk_pread (sim_raw_osfile_t handle, void *buf, size_t bytes, t_offset offset)
{
#if defined(_WIN32)
    OVERLAPPED ovl;
    DWORD bytesRead = 0;

    if (handle != SIM_INVALID_OSFILE || !buf)
        return (size_t)-1;

    /* Setup OVERLAPPED structure for positioned I/O */
    memset(&ovl, 0, sizeof(ovl));
    ovl.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ovl.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);

    /* Perform positioned read */
    if (!ReadFile(handle, buf, (DWORD)bytes, &bytesRead, &ovl)) {
        DWORD err = GetLastError();
        /* Handle EOF gracefully */
        if (err == ERROR_HANDLE_EOF)
            return 0;
        /* ERROR_IO_PENDING means the operation is asynchronous - wait for it */
        if (err == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(handle, &ovl, &bytesRead, TRUE)) {
                err = GetLastError();
                if (err == ERROR_HANDLE_EOF)
                    return 0;
                errno = EIO;
                return (size_t)-1;
            }
        } else {
            errno = EIO;  /* Set errno for error reporting */
            return (size_t)-1;
        }
    }

    return (size_t)bytesRead;

#else /* POSIX */
    ssize_t result;

    if (handle == SIM_INVALID_OSFILE || buf == NULL)
        return ((size_t) -1);

    /* pread() is atomic and thread-safe */
    result = pread(handle, buf, bytes, (off_t) offset);

    if (result < 0)
        return ((size_t)- 1);

    return ((size_t) result);
#endif
}

/* sim_disk_pwrite - atomic positioned write
 *
 * Writes 'bytes' bytes from 'buf' to 'handle' at 'offset'.
 * Does not modify the file's current position pointer.
 *
 * Returns: number of bytes actually written, or (size_t)-1 on error
 */
size_t sim_disk_pwrite (sim_raw_osfile_t handle, const void *buf, size_t bytes, t_offset offset)
{
#if defined(_WIN32)
    OVERLAPPED ovl;
    DWORD bytesWritten = 0;

    if (handle == SIM_INVALID_OSFILE || buf == NULL)
        return (size_t)-1;

    /* Setup OVERLAPPED structure for positioned I/O */
    memset(&ovl, 0, sizeof(ovl));
    ovl.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ovl.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);

    /* Perform positioned write */
    if (!WriteFile(handle, buf, (DWORD)bytes, &bytesWritten, &ovl)) {
        DWORD dwError = GetLastError();
        /* ERROR_IO_PENDING means the operation is asynchronous - wait for it */
        if (dwError == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(handle, &ovl, &bytesWritten, TRUE)) {
                errno = EIO;
                return ((size_t) -1);
            }
        } else {
            errno = EIO;  /* Set errno for error reporting */
            return ((size_t) -1);
        }
    }

    return ((size_t) bytesWritten);

#else /* POSIX */
    ssize_t result;

    if (handle == SIM_INVALID_OSFILE || buf == NULL)
        return ((size_t) -1);

    /* pwrite() is atomic and thread-safe */
    result = pwrite(handle, buf, bytes, (off_t)offset);

    if (result < 0)
        return ((size_t) -1);

    return ((size_t) result);
#endif
}

/* sim_fopen_handle - open file and return native handle
 *
 * Opens a disk image file for positioned read/write I/O.
 * Always opens in binary read/write mode, fails if file doesn't exist.
 * Returns native handle suitable for pread/pwrite (POSIX) or
 * ReadFile/WriteFile with OVERLAPPED (Windows).
 */
sim_raw_osfile_t sim_disk_open_handle (const char *file)
{
#if defined(_WIN32)
    HANDLE h;

    if (file == NULL)
        return SIM_INVALID_OSFILE;

    /* Open for read/write, fail if doesn't exist, enable positioned I/O */
    h = CreateFileA(file,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                    NULL);

    if (h == INVALID_HANDLE_VALUE) {
        /* Map Windows error to errno for error reporting */
        DWORD dwError = GetLastError();
        switch (dwError) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                errno = ENOENT;
                break;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
                errno = EACCES;
                break;
            default:
                errno = EIO;
                break;
        }
        return SIM_INVALID_OSFILE;
    }

    return h;
#else
    int fd;
    int flags = O_RDWR;

    if (file == NULL)
        return SIM_INVALID_OSFILE;

#if defined(O_LARGEFILE)
    flags |= O_LARGEFILE;
#endif

    fd = open(file, flags);
    return (fd >= 0 ? fd : SIM_INVALID_OSFILE);
#endif
}

/*
 * Opens a file with explicit mode for VHD/RAW disk internals.
 * Returns native handle suitable for positioned I/O.
 */
sim_raw_osfile_t sim_fopen_handle (const char *file, const char *mode)
{
#if defined(_WIN32)
    DWORD dwDesiredAccess = 0;
    DWORD dwCreationDisposition = 0;
    HANDLE h;

    if (!file)
        return SIM_INVALID_OSFILE;

    /* Parse mode string to determine access and creation flags */
    if (strchr(mode, '+')) {
        /* Read and write */
        dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
        if (strchr(mode, 'w'))
            dwCreationDisposition = CREATE_ALWAYS;
        else if (strchr(mode, 'a'))
            dwCreationDisposition = OPEN_ALWAYS;
        else
            dwCreationDisposition = OPEN_EXISTING;
    } else if (strchr(mode, 'w')) {
        /* Write only */
        dwDesiredAccess = GENERIC_WRITE;
        dwCreationDisposition = CREATE_ALWAYS;
    } else if (strchr(mode, 'a')) {
        /* Append */
        dwDesiredAccess = GENERIC_WRITE;
        dwCreationDisposition = OPEN_ALWAYS;
    } else {
        /* Read only (default) */
        dwDesiredAccess = GENERIC_READ;
        dwCreationDisposition = OPEN_EXISTING;
    }

    /* Open with FILE_FLAG_OVERLAPPED for positioned I/O support */
    h = CreateFileA(file,
                    dwDesiredAccess,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    dwCreationDisposition,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                    NULL);

    if (h == INVALID_HANDLE_VALUE) {
        /* Map Windows error to errno for error reporting */
        DWORD dwError = GetLastError();
        switch (dwError) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                errno = ENOENT;
                break;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
                errno = EACCES;
                break;
            default:
                errno = EIO;
                break;
        }
        return SIM_INVALID_OSFILE;
    }

    return h;
#else
    int fd;
    int flags = 0;

    if (file == NULL)
        return SIM_INVALID_OSFILE;

    /* Parse mode to flags */
    if (strchr(mode, '+')) {
        flags = O_RDWR;
        if (strchr(mode, 'w'))
            flags |= O_CREAT | O_TRUNC;
        else if (strchr(mode, 'a'))
            flags |= O_CREAT | O_APPEND;
    } else if (strchr(mode, 'w')) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (strchr(mode, 'a')) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else {
        flags = O_RDONLY;
    }

#if defined(O_LARGEFILE)
    flags |= O_LARGEFILE;
#endif

    fd = open(file, flags, 0666);
    return (fd >= 0 ? fd : SIM_INVALID_OSFILE);
#endif
}

/* sim_fclose_handle - close a native file handle */
int sim_fclose_handle (sim_raw_osfile_t handle)
{
#if defined(_WIN32)
    if (handle == SIM_INVALID_OSFILE)
        return -1;
    return CloseHandle(handle) ? 0 : -1;
#else
    if (handle == SIM_INVALID_OSFILE)
        return -1;
    return close(handle);
#endif
}

/* sim_fflush_handle - flush a native file handle */
void sim_fflush_handle (sim_raw_osfile_t handle)
{
    if (handle != SIM_INVALID_OSFILE) {
#if defined(_WIN32)
        FlushFileBuffers(handle);
#else
        fsync(handle);
#endif
    }
}

/* sim_fsize_handle - get size of file from native handle */
t_offset sim_fsize_handle (sim_raw_osfile_t handle)
{
#if defined(_WIN32)
    LARGE_INTEGER size;
    if (handle == SIM_INVALID_OSFILE)
        return ((t_offset) -1);
    if (!GetFileSizeEx(handle, &size))
        return ((t_offset) -1);
    return ((t_offset) size.QuadPart);
#else
    struct stat statbuf;
    if (handle == SIM_INVALID_OSFILE)
        return ((t_offset) -1);
    if (fstat(handle, &statbuf) != 0)
        return ((t_offset) -1);
    return ((t_offset) statbuf.st_size);
#endif
}
