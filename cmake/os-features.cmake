# # Various and sundry operating system features.
# #
# # Author: B. Scott Michel
# # "scooter me fecit"

include_guard(GLOBAL)

include(CheckSymbolExists)
include(CheckSourceCompiles)
include(CheckCSourceRuns)
include(CheckSourceCompiles)
include(CMakePushCheckState)

# =============================================================================
# The sim_support library
#
# This library includes O/S-specific compiler definitions and compatibility
# shim sources (e.g., strlcpy, strlcat). It's also a catch-all for functions
# that are shared across the core and network libraries.
# =============================================================================
set(DUMMY_SRC "${CMAKE_CURRENT_BINARY_DIR}/osfeatures_dummy.c")
if (NOT EXISTS ${DUMMY_SRC})
    file(WRITE ${DUMMY_SRC} "/* Dummy source for sim_support object library */\n")
endif ()

add_library(sim_support STATIC ${DUMMY_SRC})
target_include_directories(sim_support PRIVATE
    "${SIMH_COMPAT_ROOT}"
    "${SIMH_CORE_ROOT}"
    "${SIMH_INCLUDE_ROOT}"
    "${SIMH_RUNTIME_ROOT}"
)

# =============================================================================
# The threading and asynchronous I/O library: aio_support
# =============================================================================

# aio_support: Support library for thread management functions, e.g.,
# sim_set_thread_name(), AIO defines.
if(NOT TARGET aio_support)
    add_library(aio_support STATIC
        ${SIMH_LIB_ROOT}/sim_threads.c
        ${SIMH_LIB_ROOT}/sim_tailq.c
    )
    target_include_directories(aio_support PRIVATE
        "${SIMH_INCLUDE_ROOT}"
        "${SIMH_CORE_ROOT}"
        "${SIMH_RUNTIME_ROOT}"
        "${SIMH_COMPAT_ROOT}"
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT DEFINED WINVER)
    # Windows version configuration: Make sure that it's at least the Win 10 API.
    # Windows 10 has SetThreadDescription support.
    message(STATUS "Setting Windows target version to Windows 10 (0x0A00)")
    target_compile_definitions(sim_support PUBLIC
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        NTDDI_VERSION=0x0A000002
    )
elseif(CMAKE_HOST_SYSTEM MATCHES "Linux")
    # # Linux: Make sure _GNU_SOURCE is defined.
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)

    # # Expose _GNU_SOURCE publicly from the sim_support library, since everything uses it.
    target_compile_definitions(sim_support PUBLIC _GNU_SOURCE)

    # # Privately, in the aio_support library, it's also needed, but aio_support doesn't depend on sim_support.
    target_compile_definitions(aio_support PRIVATE _GNU_SOURCE)
endif()

# Windows / vcpkg Path: Check for our modernized pthreads4w target
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    find_package(PTW)

    if(TARGET PTW::PTW)
        # Forward all header paths, multi-config libs, and definitions to aio_support
        target_link_libraries(aio_support PUBLIC PTW::PTW)
        target_compile_definitions(aio_support PUBLIC HAVE_PTHREADS)
        set(valid_threads TRUE)
        message(STATUS "pthreads-dep: Using modern PTW::PTW interface target")
    endif()
else()
    # POSIX Path: Fallback to native system threads (Linux, FreeBSD, macOS)
    # FindThreads looks for -pthread, -lpthread, etc. based on platform/compiler
    find_package(Threads REQUIRED)

    if(TARGET Threads::Threads)
        target_compile_definitions(aio_support PUBLIC HAVE_PTHREADS)
        message(STATUS "pthreads-dep: Using system native Threads::Threads")
        target_link_libraries(aio_support PUBLIC Threads::Threads)
    endif()
endif()

include(uuid-dep)

set(NEED_LIBRT FALSE)

# # Editline support?
find_package(EDITLINE)

if(TARGET Editline::Editline)
    target_link_libraries(sim_support PUBLIC Editline::Editline)
endif()

if(WITH_ASYNC)
    # # semaphores and sem_timedwait support (OS feature):
    check_include_file(semaphore.h semaphore_h_found)

    if(semaphore_h_found)
        cmake_push_check_state()

        get_property(zz_thread_defs TARGET aio_support PROPERTY INTERFACE_COMPILE_DEFINITIONS)
        get_property(zz_thread_incs TARGET aio_support PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
        get_property(zz_thread_lopts TARGET aio_support PROPERTY INTERFACE_LINK_OPTIONS)
        get_property(zz_aio_support TARGET aio_support PROPERTY INTERFACE_LINK_LIBRARIES)

        foreach(def IN LISTS zz_thread_defs)
            if(NOT def MATCHES "^-D")
                string(PREPEND def "-D")
            endif()

            list(APPEND CMAKE_REQUIRED_DEFINITIONS ${def})
        endforeach()

        list(APPEND CMAKE_REQUIRED_INCLUDES ${zz_thread_incs})
        list(APPEND CMAKE_REQUIRED_LINK_OPTIONS ${zz_thread_lopts})
        list(APPEND CMAKE_REQUIRED_LIBRARIES ${zz_aio_support})

        check_symbol_exists(sem_timedwait "semaphore.h;time.h" have_sem_timedwait)

        if(NOT have_sem_timedwait)
            # # Maybe it's in librt, like shm_open (and more likely, it's not.)
            list(APPEND CMAKE_REQUIRED_LIBRARIES rt)
            check_symbol_exists(sem_timedwait semaphore.h have_sem_timedwait_rt)

            if(have_sem_timedwait_rt)
                set(NEED_LIBRT TRUE)
            endif(have_sem_timedwait_rt)
        endif(NOT have_sem_timedwait)

        cmake_pop_check_state()

        if(have_sem_timedwait OR have_sem_timedwait_rt)
            target_compile_definitions(sim_support PUBLIC HAVE_SEMAPHORE)
        endif()
    endif(semaphore_h_found)
endif(WITH_ASYNC)

# =============================================================================
# X-platform string compatibility functions.
#
# If not found or present, add sources to the sim_support library and link
# against the sim_support library.
# =============================================================================
cmake_push_check_state()
check_symbol_exists(strlcpy string.h HAVE_STRLCPY)
check_symbol_exists(strlcat string.h HAVE_STRLCAT)
check_symbol_exists(strnlen string.h HAVE_STRNLEN)
check_symbol_exists(strdup string.h HAVE_STRDUP)
check_symbol_exists(strndup string.h HAVE_STRNDUP)
check_symbol_exists(strcasecmp strings.h HAVE_STRCASECMP)
check_symbol_exists(strncasecmp strings.h HAVE_STRNCASECMP)
cmake_pop_check_state()

configure_uuid_feature(sim_support)

if(NOT HAVE_STRLCPY)
    target_sources(sim_support PRIVATE ${SIMH_COMPAT_ROOT}/strlcpy.c)
else()
    target_compile_definitions(sim_support PUBLIC HAVE_STRLCPY)
endif()

if(NOT HAVE_STRLCAT)
    target_sources(sim_support PRIVATE ${SIMH_COMPAT_ROOT}/strlcat.c)
else()
    target_compile_definitions(sim_support PUBLIC HAVE_STRLCAT)
endif()

if(NOT HAVE_STRNLEN)
    # strnlen is a define for Windows
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_sources(sim_support PRIVATE ${SIMH_COMPAT_ROOT}/strnlen.c)
    endif()
else()
    target_compile_definitions(sim_support PUBLIC HAVE_STRNLEN)
endif()

if(NOT HAVE_STRDUP)
    target_sources(sim_support PRIVATE ${SIMH_COMPAT_ROOT}/strdup.c)
else()
    target_compile_definitions(sim_support PUBLIC HAVE_STRDUP)
endif()

if(NOT HAVE_STRNDUP)
    target_sources(sim_support PRIVATE ${SIMH_COMPAT_ROOT}/strndup.c)
else()
    target_compile_definitions(sim_support PUBLIC HAVE_STRNDUP)
endif()

if(NOT HAVE_STRCASECMP)
    # strcasecmp is a define for Windows
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_sources(sim_support PRIVATE ${SIMH_COMPAT_ROOT}/strcasecmp.c)
    endif()
else()
    target_compile_definitions(sim_support PUBLIC HAVE_STRCASECMP)
endif()

if(NOT HAVE_STRNCASECMP)
    # strncasecmp is a define for Windows
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_sources(sim_support PRIVATE ${SIMH_COMPAT_ROOT}/strncasecmp.c)
    endif()
else()
    target_compile_definitions(sim_support PUBLIC HAVE_STRNCASECMP)
endif()

# =============================================================================
# Windows compatibility shims.
#
# NOTE: Shouldn't CMake test that the re-entrant versions exist, and not just
# for Windows?
# =============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_sources(sim_support PRIVATE
        ${SIMH_COMPAT_ROOT}/localtime_r.c
        ${SIMH_COMPAT_ROOT}/gmtime_r.c
        ${SIMH_COMPAT_ROOT}/setenv.c)
endif()

# =============================================================================
# OS-specific header files:
# =============================================================================

# # <sys/ioctl.h>
check_include_file(sys/ioctl.h have_sys_ioctl_h)

if(have_sys_ioctl_h)
    target_compile_definitions(sim_support INTERFACE HAVE_SYS_IOCTL)
endif(have_sys_ioctl_h)

# # <linux/cdrom.h>
check_include_file(linux/cdrom.h have_linux_cdrom_h)

if(have_linux_cdrom_h)
    target_compile_definitions(sim_support INTERFACE HAVE_LINUX_CDROM)
endif(have_linux_cdrom_h)

# # <utime.h>
check_include_file(utime.h have_utime_h)

if(have_utime_h)
    target_compile_definitions(sim_support INTERFACE HAVE_UTIME)
endif(have_utime_h)

# # <glob.h>
check_include_file(glob.h have_glob_h)

if(have_glob_h)
    target_compile_definitions(sim_support INTERFACE HAVE_GLOB)
else()
    # # <fnmatch.h>
    check_include_file(fnmatch.h have_fnmatch_h)

    if(have_fnmatch_h)
        target_compile_definitions(sim_support INTERFACE HAVE_FNMATCH)
    endif(have_fnmatch_h)
endif(have_glob_h)

# =============================================================================
# fmemopen(), fopen() exclusive-create mode.
# =============================================================================
cmake_push_check_state()
check_symbol_exists(fmemopen stdio.h HAVE_FMEMOPEN)

if(HAVE_FMEMOPEN)
    target_compile_definitions(sim_support PUBLIC HAVE_FMEMOPEN)
endif()

check_c_source_runs("
#include <stdio.h>

int main(void)
{
    FILE *f;

    remove(\"cmake_fopen_x_test.tmp\");

    f = fopen(\"cmake_fopen_x_test.tmp\", \"wbx\");
    if (f == NULL)
        return 1;
    fclose(f);

    f = fopen(\"cmake_fopen_x_test.tmp\", \"wbx\");
    if (f != NULL) {
        fclose(f);
        remove(\"cmake_fopen_x_test.tmp\");
        return 2;
    }

    remove(\"cmake_fopen_x_test.tmp\");
    return 0;
}
" have_working_fopen_x_mode)

if(NOT have_working_fopen_x_mode)
    target_compile_definitions(sim_support INTERFACE SIMH_NO_FOPEN_X)
endif()

cmake_pop_check_state()

# # <sys/mman.h> and shm_open
check_include_file(sys/mman.h have_sys_mman_h)

if(have_sys_mman_h)
    cmake_push_check_state()

    check_symbol_exists(shm_open sys/mman.h have_shm_open)

    if(NOT have_shm_open OR NEED_LIBRT)
        # # Linux: shm_open is in the rt library?
        set(CMAKE_REQUIRED_LIBRARIES rt)
        check_symbol_exists(shm_open sys/mman.h have_shm_open_lrt)
    endif(NOT have_shm_open OR NEED_LIBRT)

    if(have_shm_open OR have_shm_open_lrt)
        target_compile_definitions(sim_support INTERFACE HAVE_SHM_OPEN)
    endif(have_shm_open OR have_shm_open_lrt)

    if(have_shm_open_lrt)
        set(NEED_LIBRT TRUE)
    endif(have_shm_open_lrt)

    cmake_pop_check_state()
endif(have_sys_mman_h)

IF(NEED_LIBRT)
    target_link_libraries(sim_support INTERFACE rt)
ENDIF(NEED_LIBRT)

if(NOT MSVC AND NOT(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_C_COMPILER_ID MATCHES "Clang"))
    # Need the math library on non-Windows platforms
    target_link_libraries(sim_support INTERFACE m)
endif()

if(TARGET PTW::PTW OR TARGET Threads::Threads)
    target_compile_definitions(aio_support PUBLIC "SIM_ASYNCH_IO")

    # =============================================================================
    # Thread naming capability detection
    # =============================================================================
    message(STATUS "Detecting thread naming capabilities...")

    cmake_push_check_state()

    # Inherit aio_support's configuration for checks
    get_property(zz_thread_defs TARGET aio_support PROPERTY COMPILE_DEFINITIONS)
    get_property(zz_thread_incs TARGET aio_support PROPERTY INCLUDE_DIRECTORIES)
    get_property(zz_aio_support TARGET aio_support PROPERTY LINK_LIBRARIES)

    # # CMAKE_REQUIRED_DEFINITIONS needs "-D" in front of each def.
    foreach(DEF ${zz_thread_defs})
        list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D${DEF}")
    endforeach()

    list(APPEND CMAKE_REQUIRED_INCLUDES ${zz_thread_incs})
    list(APPEND CMAKE_REQUIRED_LIBRARIES ${zz_aio_support})

    set(THREAD_NAMING_METHOD)

    # Windows: SetThreadDescription
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        message(STATUS "  Checking for SetThreadDescription (Windows 10 1607+)...")
        check_source_compiles(C "
            #include <windows.h>
            #include <processthreadsapi.h>
            int main(void) {
                SetThreadDescription(GetCurrentThread(), L\"test\");
                return 0;
            }
        " HAVE_SETTHREADDESCRIPTION)

        if(HAVE_SETTHREADDESCRIPTION)
            target_compile_definitions(aio_support PRIVATE HAVE_SETTHREADDESCRIPTION)
            target_compile_definitions(aio_support PRIVATE THREAD_NAME_MAX=64)
            set(THREAD_NAMING_METHOD "SetThreadDescription")
            set(THREAD_NAME_MAX_VALUE 64)
            message(STATUS "  ✓ SetThreadDescription available")
        else()
            message(STATUS "  ✗ SetThreadDescription not available")
        endif()
    endif()

    # Linux: prctl(PR_SET_NAME)
    if(UNIX AND NOT APPLE AND NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for prctl(PR_SET_NAME) (Linux)...")
        check_source_compiles(C "
            #include <sys/prctl.h>
            int main(void) {
                prctl(PR_SET_NAME, \"test\", 0, 0, 0);
                return 0;
            }
        " HAVE_PRCTL_SET_NAME)

        if(HAVE_PRCTL_SET_NAME)
            target_compile_definitions(aio_support PRIVATE HAVE_PRCTL_SET_NAME)
            target_compile_definitions(aio_support PRIVATE THREAD_NAME_MAX=16)
            set(THREAD_NAMING_METHOD "prctl(PR_SET_NAME)")
            set(THREAD_NAME_MAX_VALUE 16)
            message(STATUS "  ✓ prctl(PR_SET_NAME) available (max length: 16)")
        else()
            message(STATUS "  ✗ prctl(PR_SET_NAME) not available")
        endif()
    endif()

    # macOS: pthread_setname_np (single argument)
    if(NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_setname_np (single arg, macOS)...")
        check_source_compiles(C "
            #include <pthread.h>
            int main(void) {
                pthread_setname_np(\"test\");
                return 0;
            }
        " HAVE_PTHREAD_SETNAME_NP_SINGLE_ARG)

        if(HAVE_PTHREAD_SETNAME_NP_SINGLE_ARG)
            target_compile_definitions(aio_support PRIVATE HAVE_PTHREAD_SETNAME_NP_CURRENT)
            target_compile_definitions(aio_support PRIVATE THREAD_NAME_MAX=64)
            set(THREAD_NAMING_METHOD "pthread_setname_np (current thread)")
            set(THREAD_NAME_MAX_VALUE 64)
            message(STATUS "  ✓ pthread_setname_np (single arg) available")
        else()
            message(STATUS "  ✗ pthread_setname_np (single arg) not available")
        endif()
    endif()

    # FreeBSD/OpenBSD: pthread_set_name_np
    if(NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_set_name_np (FreeBSD/OpenBSD)...")
        check_source_compiles(C "
            #include <pthread.h>
            #include <pthread_np.h>
            int main(void) {
                pthread_set_name_np(pthread_self(), \"test\");
                return 0;
            }
        " HAVE_PTHREAD_SET_NAME_NP)

        if(HAVE_PTHREAD_SET_NAME_NP)
            target_compile_definitions(aio_support PRIVATE HAVE_PTHREAD_SET_NAME_NP)
            target_compile_definitions(aio_support PRIVATE THREAD_NAME_MAX=32)
            set(THREAD_NAMING_METHOD "pthread_set_name_np")
            set(THREAD_NAME_MAX_VALUE 32)
            message(STATUS "  ✓ pthread_set_name_np available")
        else()
            message(STATUS "  ✗ pthread_set_name_np not available")
        endif()
    endif()

    # NetBSD: pthread_setname_np with format string
    if(NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_setname_np (format string, NetBSD)...")
        check_source_compiles(C "
            #include <pthread.h>
            int main(void) {
                pthread_setname_np(pthread_self(), \"%s\", \"test\");
                return 0;
            }
        " HAVE_PTHREAD_SETNAME_NP_NETBSD)

        if(HAVE_PTHREAD_SETNAME_NP_NETBSD)
            target_compile_definitions(aio_support PRIVATE HAVE_PTHREAD_SETNAME_NP_NETBSD)
            target_compile_definitions(aio_support PRIVATE THREAD_NAME_MAX=32)
            set(THREAD_NAMING_METHOD "pthread_setname_np (NetBSD)")
            set(THREAD_NAME_MAX_VALUE 32)
            message(STATUS "  ✓ pthread_setname_np (NetBSD format) available")
        else()
            message(STATUS "  ✗ pthread_setname_np (NetBSD format) not available")
        endif()
    endif()

    # Generic: pthread_setname_np (two arguments)
    if(NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_setname_np (generic POSIX)...")
        check_source_compiles(C "
            #include <pthread.h>
            int main(void) {
                pthread_setname_np(pthread_self(), \"test\");
                return 0;
            }
        " HAVE_PTHREAD_SETNAME_NP_GENERIC)

        if(HAVE_PTHREAD_SETNAME_NP_GENERIC)
            target_compile_definitions(aio_support PRIVATE HAVE_PTHREAD_SETNAME_NP_GENERIC)
            target_compile_definitions(aio_support PRIVATE THREAD_NAME_MAX=64)
            set(THREAD_NAMING_METHOD "pthread_setname_np (generic)")
            set(THREAD_NAME_MAX_VALUE 64)
            message(STATUS "  ✓ pthread_setname_np (generic) available")
        else()
            message(STATUS "  ✗ pthread_setname_np (generic) not available")
        endif()
    endif()

    cmake_pop_check_state()

    # Summary
    if(THREAD_NAMING_METHOD STREQUAL "None")
        message(STATUS "Thread naming: NOT SUPPORTED on this platform")
        message(STATUS "  sim_set_thread_name() will be a no-op")
    else()
        message(STATUS "Thread naming: ${THREAD_NAMING_METHOD}")
        message(STATUS "  Maximum thread name length: ${THREAD_NAME_MAX_VALUE} characters")
    endif()

    # Store for use in tests
    set(THREAD_NAMING_METHOD "${THREAD_NAMING_METHOD}" CACHE INTERNAL "Detected thread naming method")
    set(THREAD_NAME_MAX_VALUE "${THREAD_NAME_MAX_VALUE}" CACHE INTERNAL "Maximum thread name length")
endif()

# =============================================================================
# poll() vs. WSAPoll() vs. select()
#
# Prefer poll() or WSAPoll() over select(). ZIMH will revert to select() for
# backward compatibilty.
#
# NOTE: For single socket network backends, such as TAP, poll() doesn't make
# much of a performance difference. For multi-connection backends, such as
# libslirp, however, it does make a difference as the number of connections
# accumulate.
# =============================================================================
set(sim_use_select 0)
set(sim_use_poll 0)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    check_symbol_exists(poll "poll.h" have_poll_h)

    if(have_poll_h)
        set(sim_use_poll 1)
    else()
        set(sim_use_select 1)
    endif()
else()
    cmake_push_check_state()
    list(APPEND CMAKE_REQUIRED_LIBRARIES "ws2_32" "wsock32")

    check_symbol_exists(WSAPoll "winsock2.h;windows.h" have_wsa_poll)

    if(have_wsa_poll)
        set(sim_use_poll 1)
    else()
        set(sim_use_select 1)
    endif()

    cmake_pop_check_state()
endif()

target_compile_definitions(sim_support PUBLIC
    SIM_USE_POLL=${sim_use_poll}
    SIM_USE_SELECT=${sim_use_select}
)

# # Windows: winmm (for ms timer functions), socket functions (even when networking is
# # disabled. Also squelch the deprecation warnings (these warnings can be enabled
# # via the -DENABLE_WINAPI_DEPRECATION_WARNINGS:Bool=On flag at configure
# # time.)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(sim_support INTERFACE ws2_32 winmm)
    target_compile_definitions(sim_support INTERFACE HAVE_WINMM)

    if(NOT ENABLE_WINAPI_DEPRECATION_WARNINGS)
        target_compile_definitions(sim_support INTERFACE
            _WINSOCK_DEPRECATED_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
            _CRT_SECURE_NO_WARNINGS
        )
    endif()
endif()

# # Cygwin also wants winmm. Note: Untested but should work.
if(CYGWIN)
    check_library_exists(winmm timeGetTime "" HAS_WINMM)

    if(HAS_WINMM)
        target_link_libraries(sim_support INTERFACE ws2_32 winmm)
        target_compile_definitions(sim_support INTERFACE HAVE_WINMM)
    endif()
endif()
