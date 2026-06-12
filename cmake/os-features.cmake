## Various and sundry operating system features.
##
## Author: B. Scott Michel
## "scooter me fecit"

include_guard(GLOBAL)

include(CheckSymbolExists)
include(CheckSourceCompiles)
include(CheckCSourceRuns)
include(CMakePushCheckState)

set(C11_THREAD_FRAG "
#include <threads.h>
int main(void) {
    thrd_t thread;
    return 0;
}
")

# =============================================================================
# The threading interface library: thread_lib
# =============================================================================

# Ensure the interface target exists
if(NOT TARGET thread_lib)
    add_library(thread_lib INTERFACE)
endif()

# AIO_FLAGS: Used in add_simulator.cmake to enable asynchronous I/O support primarily
# in Ethernet devices. It's only useful if the platform threading libraries are
# detected and building the AIO version of the core libraries.
set(AIO_FLAGS)

# Check if C11 threads are available:
check_source_compiles(C "${C11_THREAD_FRAG}" HAVE_C11_THREADS)

if (NOT HAVE_C11_THREADS)
    # Windows / vcpkg Path: Check for our modernized pthreads4w target
    if(NOT TARGET PTW::PTW AND WIN32)
        find_package(PTW)
    endif()

    if(TARGET PTW::PTW)
        # Forward all header paths, multi-config libs, and definitions to thread_lib
        target_link_libraries(thread_lib INTERFACE PTW::PTW)
        target_compile_definitions(thread_lib INTERFACE HAVE_PTHREADS)
        set(valid_threads TRUE)
        message(STATUS "pthreads-dep: Using modern PTW::PTW interface target")
    else()
        # POSIX Path: Fallback to native system threads (Linux, FreeBSD, macOS)
        # FindThreads looks for -pthread, -lpthread, etc. based on platform/compiler
        find_package(Threads REQUIRED)

        if (TARGET Threads::Threads)
            # Run the C11 check again, just in case the Linux or macOS compiler requires
            # linking with the thread library. Obviously, we got this far because
            # HAVE_C11_THREADS is FALSE.
            cmake_push_check_state()
            set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
            check_source_compiles(C "${C11_THREAD_FRAG}" HAVE_C11_THREADS)
            cmake_pop_check_state()

            if (NOT HAVE_C11_THREADS)
                # Nope. Fall back to ordinary pthreads.
                target_compile_definitions(thread_lib INTERFACE HAVE_PTHREADS)
                message(STATUS "pthreads-dep: Using system native Threads::Threads")
            else ()
                target_compile_definitions(thread_lib INTERFACE HAVE_C11_THREADS)
                message(STATUS "Using C11 standard concurrency library with Threads::Threads target.")
            endif ()

            # Add Threads::Threads because it's needed whether we're using C11 or native.
            target_link_libraries(thread_lib INTERFACE Threads::Threads)
        endif ()
    endif()
else ()
    message(STATUS "Using C11 standard concurrency library.")
    target_compile_definitions(thread_lib INTERFACE HAVE_C11_THREADS)
endif ()

if (TARGET PTW::PTW OR TARGET Threads::Threads OR HAVE_C11_THREADS)
    list(APPEND AIO_FLAGS "SIM_ASYNCH_IO" "USE_READER_THREAD")
endif ()

include(uuid-dep)

set(NEED_LIBRT FALSE)

## The os_features interface library: os_features
## Compatibility sources that are needed to fill in missing OS features are added to the
## os_compat OBJECT library.
add_library(os_features INTERFACE)
add_library(os_compat OBJECT)

## Editline support?
find_package(EDITLINE)
if (TARGET Editline::Editline)
    target_link_libraries(os_features INTERFACE Editline::Editline)
endif ()

if (WITH_ASYNC)
    ## semaphores and sem_timedwait support (OS feature):
    check_include_file(semaphore.h semaphore_h_found)
    if (semaphore_h_found)
        cmake_push_check_state()

        get_property(zz_thread_defs  TARGET thread_lib PROPERTY INTERFACE_COMPILE_DEFINITIONS)
        get_property(zz_thread_incs  TARGET thread_lib PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
        get_property(zz_thread_lopts TARGET thread_lib PROPERTY INTERFACE_LINK_OPTIONS)
        get_property(zz_thread_libs  TARGET thread_lib PROPERTY INTERFACE_LINK_LIBRARIES)

        list(APPEND CMAKE_REQUIRE_DEFINITIONS ${zz_thread_defs})
        list(APPEND CMAKE_REQUIRED_INCLUDES ${zz_thread_incs})
        list(APPEND CMAKE_REQUIRED_LINK_OPTIONS ${zz_thread_lopts})
        list(APPEND CMAKE_REQUIRED_LIBRARIES ${zz_thread_libs})

        check_symbol_exists(sem_timedwait semaphore.h have_sem_timedwait)

        if (NOT have_sem_timedwait)
            ## Maybe it's in librt, like shm_open (and more likely, it's not.)
            list(APPEND CMAKE_REQUIRED_LIBRARIES rt)
            check_symbol_exists(sem_timedwait semaphore.h have_sem_timedwait_rt)
            if (have_sem_timedwait_rt)
                set(NEED_LIBRT TRUE)
            endif (have_sem_timedwait_rt)
        endif (NOT have_sem_timedwait)

        cmake_pop_check_state()

        if (have_sem_timedwait OR have_sem_timedwait_rt)
            target_compile_definitions(os_features INTERFACE HAVE_SEMAPHORE)
        endif ()
    endif (semaphore_h_found)
endif (WITH_ASYNC)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(os_features INTERFACE _GNU_SOURCE)
endif ()

cmake_push_check_state()
if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
endif ()

check_symbol_exists(strlcpy string.h HAVE_STRLCPY)
check_symbol_exists(strlcat string.h HAVE_STRLCAT)
check_symbol_exists(strnlen string.h HAVE_STRNLEN)
check_symbol_exists(strdup string.h HAVE_STRDUP)
check_symbol_exists(strndup string.h HAVE_STRNDUP)
check_symbol_exists(strcasecmp strings.h HAVE_STRCASECMP)
check_symbol_exists(strncasecmp strings.h HAVE_STRNCASECMP)
check_symbol_exists(fmemopen stdio.h HAVE_FMEMOPEN)
cmake_pop_check_state()

if (HAVE_FMEMOPEN)
    target_compile_definitions(os_features INTERFACE HAVE_FMEMOPEN)
endif ()

configure_uuid_feature(os_features)

if (NOT HAVE_STRLCPY)
    target_sources(os_compat PRIVATE ${SIMH_COMPAT_ROOT}/strlcpy.c)
    target_compile_definitions(os_features INTERFACE SIMH_NEED_STRLCPY)
endif ()

if (NOT HAVE_STRLCAT)
    target_sources(os_compat PRIVATE ${SIMH_COMPAT_ROOT}/strlcat.c)
    target_compile_definitions(os_features INTERFACE SIMH_NEED_STRLCAT)
endif ()

if (NOT HAVE_STRNLEN)
    target_sources(os_compat PRIVATE ${SIMH_COMPAT_ROOT}/strnlen.c)
    target_compile_definitions(os_features INTERFACE SIMH_NEED_STRNLEN)
endif ()

if (NOT HAVE_STRDUP)
    target_sources(os_compat PRIVATE ${SIMH_COMPAT_ROOT}/strdup.c)
    target_compile_definitions(os_features INTERFACE SIMH_NEED_STRDUP)
endif ()

if (NOT HAVE_STRNDUP)
    target_sources(os_compat PRIVATE ${SIMH_COMPAT_ROOT}/strndup.c)
    target_compile_definitions(os_features INTERFACE SIMH_NEED_STRNDUP)
endif ()

if (NOT HAVE_STRCASECMP)
    target_sources(os_compat PRIVATE ${SIMH_COMPAT_ROOT}/strcasecmp.c)
    target_compile_definitions(os_features INTERFACE SIMH_NEED_STRCASECMP)
endif ()

if (NOT HAVE_STRNCASECMP)
    target_sources(os_compat PRIVATE ${SIMH_COMPAT_ROOT}/strncasecmp.c)
    target_compile_definitions(os_features INTERFACE SIMH_NEED_STRNCASECMP)
endif ()

if (WIN32)
    target_sources(os_compat PRIVATE 
        ${SIMH_COMPAT_ROOT}/localtime_r.c
        ${SIMH_COMPAT_ROOT}/gmtime_r.c
        ${SIMH_COMPAT_ROOT}/setenv.c
    )
endif ()

## <sys/ioctl.h>
check_include_file(sys/ioctl.h have_sys_ioctl_h)
if (have_sys_ioctl_h)
    target_compile_definitions(os_features INTERFACE HAVE_SYS_IOCTL)
endif (have_sys_ioctl_h)

## <linux/cdrom.h>
check_include_file(linux/cdrom.h have_linux_cdrom_h)
if (have_linux_cdrom_h)
    target_compile_definitions(os_features INTERFACE HAVE_LINUX_CDROM)
endif (have_linux_cdrom_h)

## <utime.h>
check_include_file(utime.h have_utime_h)
if (have_utime_h)
    target_compile_definitions(os_features INTERFACE HAVE_UTIME)
endif (have_utime_h)

## <glob.h>
check_include_file(glob.h have_glob_h)
if (have_glob_h)
    target_compile_definitions(os_features INTERFACE HAVE_GLOB)
else ()
    ## <fnmatch.h>
    check_include_file(fnmatch.h have_fnmatch_h)
    if (have_fnmatch_h)
        target_compile_definitions(os_features INTERFACE HAVE_FNMATCH)
    endif (have_fnmatch_h)
endif (have_glob_h)

## fopen(..."x") exclusive-create mode
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
if (NOT have_working_fopen_x_mode)
    target_compile_definitions(os_features INTERFACE SIMH_NO_FOPEN_X)
endif ()

## <sys/mman.h> and shm_open
check_include_file(sys/mman.h have_sys_mman_h)
if (have_sys_mman_h)
    cmake_push_check_state()

    check_symbol_exists(shm_open sys/mman.h have_shm_open)

    if (NOT have_shm_open OR NEED_LIBRT)
        ## Linux: shm_open is in the rt library?
        set(CMAKE_REQUIRED_LIBRARIES rt)
        check_symbol_exists(shm_open sys/mman.h have_shm_open_lrt)
    endif (NOT have_shm_open OR NEED_LIBRT)

    if (have_shm_open OR have_shm_open_lrt)
        target_compile_definitions(os_features INTERFACE HAVE_SHM_OPEN)
    endif (have_shm_open OR have_shm_open_lrt)
    if (have_shm_open_lrt)
        set(NEED_LIBRT TRUE)
    endif (have_shm_open_lrt)

    cmake_pop_check_state()
endif (have_sys_mman_h)

IF (NEED_LIBRT)
    target_link_libraries(os_features INTERFACE rt)
ENDIF (NEED_LIBRT)

if (NOT MSVC AND NOT (WIN32 AND CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
    # Need the math library on non-Windows platforms
    target_link_libraries(os_features INTERFACE m)
endif ()

set(HAVE_TAP_NETWORK False)
set(HAVE_BSDTUNTAP False)

if (WITH_NETWORK)
    ## TAP/TUN devices
    if (WITH_TAP)
        check_include_file(linux/if_tun.h if_tun_found)

        if (NOT if_tun_found)
            check_include_file(net/if_tun.h net_if_tun_found)
            if (net_if_tun_found OR EXISTS /Library/Extensions/tap.kext)
                set(HAVE_BSDTUNTAP True)
            endif (net_if_tun_found OR EXISTS /Library/Extensions/tap.kext)
        endif (NOT if_tun_found)

        if (if_tun_found OR net_if_tun_found)
            set(HAVE_TAP_NETWORK True)
        endif (if_tun_found OR net_if_tun_found)
    endif (WITH_TAP)

    # poll/select detection
    set(sim_use_select 0)
    set(sim_use_poll   0)

    if (NOT WIN32)
        check_symbol_exists(poll "poll.h" have_poll_h)
        if (have_poll_h)
            set(sim_use_poll   1)
        else ()
            set(sim_use_select 1)
        endif()
    else ()
        cmake_push_check_state()
        list(APPEND CMAKE_REQUIRED_LIBRARIES "ws2_32" "wsock32")
        check_symbol_exists(WSAPoll "winsock2.h;windows.h" have_wsa_poll)
        if (have_wsa_poll)
            set(sim_use_poll  1)
        else ()
            set(sim_use_select 1)
        endif ()
        cmake_pop_check_state()
    endif()

    target_compile_definitions(os_features INTERFACE
        SIM_USE_POLL=${sim_use_poll}
        SIM_USE_SELECT=${sim_use_select}
    )
endif (WITH_NETWORK)

if (WIN32)
    ## Ensure the Windows API is at least Windows 8. WINVER and _WIN32_WINNT should
    ## have the same value (theoretically, WINVER is deprecated, but you still need
    ## both.)
    # 0x0602 -> Windows 8 / Windows Server 2012 across all SDK subsystems
    target_compile_definitions(os_features INTERFACE
        WINVER=0x0a00
        _WIN32_WINNT=0x0a00
        NTDDI_VERSION=0x0a000000
    )

    ## winmm (for ms timer functions), socket functions (even when networking is
    ## disabled.
    ##
    ## Also squelch the deprecation warnings (these warnings can be enabled
    ## via the -DENABLE_WINAPI_DEPRECATION_WARNINGS:Bool=On flag at configure
    ## time.)
    target_link_libraries(os_features INTERFACE ws2_32 winmm)
    target_compile_definitions(os_features INTERFACE HAVE_WINMM)
    if (NOT ENABLE_WINAPI_DEPRECATION_WARNINGS)
        target_compile_definitions(os_features INTERFACE
            _WINSOCK_DEPRECATED_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
            _CRT_SECURE_NO_WARNINGS
        )
    endif ()
endif ()

## os_compat finalization: add include paths and inherit os_features definitions and libraries
target_include_directories(os_compat PRIVATE
    "${SIMH_COMPAT_ROOT}"
    "${SIMH_CORE_ROOT}"
    "${SIMH_INCLUDE_ROOT}"
)

target_link_libraries(os_compat PRIVATE os_features)