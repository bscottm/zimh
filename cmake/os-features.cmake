## Various and sundry operating system features.
##
## Author: B. Scott Michel
## "scooter me fecit"

include_guard(GLOBAL)

include(CheckSymbolExists)
include(CheckSourceCompiles)
include(CheckCSourceRuns)
include(CheckSourceCompiles)
include(CMakePushCheckState)

## =============================================================================
## C11 concurrency source fragment
## =============================================================================

set(C11_THREAD_FRAG "
#include <threads.h>
int main(void) {
    thrd_t thread;
    return 0;
}
")

# =============================================================================
# The os_features library
#
# This library includes O/S-specific compiler definitions and compatibility
# shim sources (e.g., strlcpy, strlcat).
# =============================================================================
set(DUMMY_SRC "${CMAKE_CURRENT_BINARY_DIR}/osfeatures_dummy.c")
file(WRITE ${DUMMY_SRC} "/* Dummy source for os_features object library */\n")
add_library(os_features STATIC ${DUMMY_SRC})
target_include_directories(os_features PRIVATE
            "${SIMH_COMPAT_ROOT}"
            "${SIMH_INCLUDE_ROOT}"
)

## Linux: Make sure _GNU_SOURCE is defined.
if (CMAKE_HOST_SYSTEM MATCHES "Linux")
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    target_compile_definitions(os_features PUBLIC _GNU_SOURCE)
endif()

## =============================================================================
## Windows version configuration: Make sure that it's at least the Win 10 API.
## =============================================================================

if (WIN32 AND NOT DEFINED WINVER)
    # Default to Windows 10 for SetThreadDescription support
    message(STATUS "Setting Windows target version to Windows 10 (0x0A00)")
    target_compile_definitions(os_features PUBLIC
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        NTDDI_VERSION=0x0A000002
    )
endif()

# =============================================================================
# The threading library: thread_lib
# =============================================================================

# thread_lib: Support library for thread management functions, e.g.,
# sim_set_thread_name().

if(NOT TARGET thread_lib)
    add_library(thread_lib STATIC
        ${SIMH_LIB_ROOT}/sim_threads.c
        ${SIMH_LIB_ROOT}/sim_tailq.c
    )
    target_include_directories(thread_lib PRIVATE
        "${SIMH_INCLUDE_ROOT}"
    )
endif()

# AIO_FLAGS: Used in add_simulator.cmake to enable asynchronous I/O support primarily
# in Ethernet devices. It's only useful if the platform threading libraries are
# detected and building the AIO version of the core libraries.
set(AIO_FLAGS)

# Check if C11 threads are available:
check_source_compiles(C "${C11_THREAD_FRAG}" HAVE_C11_THREADS)

## Enable when C11 concurrency better integrated. In the meantime, keep looking
## for pthreads.
##
## if (NOT HAVE_C11_THREADS)
    # Windows / vcpkg Path: Check for our modernized pthreads4w target
    if(NOT TARGET PTW::PTW AND WIN32)
        find_package(PTW)
    endif()

    if(TARGET PTW::PTW)
        # Forward all header paths, multi-config libs, and definitions to thread_lib
        target_link_libraries(thread_lib PUBLIC PTW::PTW)
        target_compile_definitions(thread_lib PUBLIC HAVE_PTHREADS)
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
            check_source_compiles(C "${C11_THREAD_FRAG}" HAVE_C11_THREADS_WITH_PTHREADS)
            cmake_pop_check_state()

            if (HAVE_C11_THREADS_WITH_PTHREADS)
                target_compile_definitions(thread_lib PUBLIC HAVE_C11_THREADS)
                message(STATUS "Using C11 standard concurrency library with Threads::Threads target.")
            else ()
                # Nope. Fall back to ordinary pthreads.
                target_compile_definitions(thread_lib PUBLIC HAVE_PTHREADS)
                message(STATUS "pthreads-dep: Using system native Threads::Threads")
            endif ()

            # Add Threads::Threads because it's needed whether we're using C11 or native.
            target_link_libraries(thread_lib PUBLIC Threads::Threads)
        endif ()
    endif()
## Enable me, replace next line: else ()
if (HAVE_C11_THREADS)
    message(STATUS "Enabling C11 standard concurrency library.")
    target_compile_definitions(thread_lib PUBLIC HAVE_C11_THREADS)
endif ()

include(uuid-dep)

set(NEED_LIBRT FALSE)

## Editline support?
find_package(EDITLINE)
if (TARGET Editline::Editline)
    target_link_libraries(os_features PUBLIC Editline::Editline)
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

        foreach(def IN LISTS zz_thread_defs)
            if (NOT def MATCHES "^-D")
                string(PREPEND def "-D")
            endif()
            list(APPEND CMAKE_REQUIRED_DEFINITIONS ${def})
        endforeach()
        list(APPEND CMAKE_REQUIRED_INCLUDES ${zz_thread_incs})
        list(APPEND CMAKE_REQUIRED_LINK_OPTIONS ${zz_thread_lopts})
        list(APPEND CMAKE_REQUIRED_LIBRARIES ${zz_thread_libs})

        check_symbol_exists(sem_timedwait "semaphore.h;time.h" have_sem_timedwait)

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
            target_compile_definitions(os_features PUBLIC HAVE_SEMAPHORE)
        endif ()
    endif (semaphore_h_found)
endif (WITH_ASYNC)

cmake_push_check_state()
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
    target_compile_definitions(os_features PUBLIC HAVE_FMEMOPEN)
endif ()

configure_uuid_feature(os_features)

set(SIMH_COMPAT_SOURCES)

if (NOT HAVE_STRLCPY)
    target_sources(os_features PRIVATE ${SIMH_COMPAT_ROOT}/strlcpy.c)
else ()
    target_compile_definitions(os_features PUBLIC HAVE_STRLCPY)
endif ()

if (NOT HAVE_STRLCAT)
    set(SIMH_NEED_STRLCAT TRUE)
    target_sources(os_features PRIVATE ${SIMH_COMPAT_ROOT}/strlcat.c)
    target_compile_definitions(os_features PUBLIC SIMH_NEED_STRLCAT)
endif ()

if (NOT HAVE_STRNLEN)
    set(SIMH_NEED_STRNLEN TRUE)
    target_sources(os_features PRIVATE ${SIMH_COMPAT_ROOT}/strnlen.c)
    target_compile_definitions(os_features PUBLIC SIMH_NEED_STRNLEN)
endif ()

if (NOT HAVE_STRDUP)
    set(SIMH_NEED_STRDUP TRUE)
    target_sources(os_features PRIVATE ${SIMH_COMPAT_ROOT}/strdup.c)
    target_compile_definitions(os_features PUBLIC SIMH_NEED_STRDUP)
endif ()

if (NOT HAVE_STRNDUP)
    set(SIMH_NEED_STRNDUP TRUE)
    target_sources(os_features PRIVATE ${SIMH_COMPAT_ROOT}/strndup.c)
    target_compile_definitions(os_features PUBLIC SIMH_NEED_STRNDUP)
endif ()

if (NOT HAVE_STRCASECMP)
    set(SIMH_NEED_STRCASECMP TRUE)
    target_sources(os_features PRIVATE ${SIMH_COMPAT_ROOT}/strcasecmp.c)
    target_compile_definitions(os_features PUBLIC SIMH_NEED_STRCASECMP)
endif ()

if (NOT HAVE_STRNCASECMP)
    set(SIMH_NEED_STRNCASECMP TRUE)
    target_sources(os_features PRIVATE ${SIMH_COMPAT_ROOT}/strncasecmp.c)
    target_compile_definitions(os_features PUBLIC SIMH_NEED_STRNCASECMP)
endif ()

if (WIN32)
    target_sources(os_features PRIVATE
        ${SIMH_COMPAT_ROOT}/localtime_r.c
        ${SIMH_COMPAT_ROOT}/gmtime_r.c
        ${SIMH_COMPAT_ROOT}/setenv.c)
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

if (TARGET PTW::PTW OR TARGET Threads::Threads OR HAVE_C11_THREADS)
    list(APPEND AIO_FLAGS "SIM_ASYNCH_IO" "USE_READER_THREAD")

    ## =============================================================================
    ## Thread naming capability detection
    ## =============================================================================

    message(STATUS "Detecting thread naming capabilities...")
    
    cmake_push_check_state()
    
    # Inherit thread_lib's configuration for checks
    get_property(zz_thread_defs  TARGET thread_lib PROPERTY COMPILE_DEFINITIONS)
    get_property(zz_thread_incs  TARGET thread_lib PROPERTY INCLUDE_DIRECTORIES)
    get_property(zz_thread_libs  TARGET thread_lib PROPERTY LINK_LIBRARIES)

    ## CMAKE_REQUIRED_DEFINITIONS needs "-D" in front of each def.
    foreach (DEF ${zz_thread_defs})
        list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D${DEF}")
    endforeach()

    list(APPEND CMAKE_REQUIRED_INCLUDES ${zz_thread_incs})
    list(APPEND CMAKE_REQUIRED_LIBRARIES ${zz_thread_libs})
    
    set(THREAD_NAMING_METHOD)
    
    # Windows: SetThreadDescription
    if (WIN32)
        message(STATUS "  Checking for SetThreadDescription (Windows 10 1607+)...")
        check_source_compiles(C "
            #include <windows.h>
            #include <processthreadsapi.h>
            int main(void) {
                SetThreadDescription(GetCurrentThread(), L\"test\");
                return 0;
            }
        " HAVE_SETTHREADDESCRIPTION)
        
        if (HAVE_SETTHREADDESCRIPTION)
            target_compile_definitions(thread_lib PRIVATE HAVE_SETTHREADDESCRIPTION)
            target_compile_definitions(thread_lib PRIVATE THREAD_NAME_MAX=64)
            set(THREAD_NAMING_METHOD "SetThreadDescription")
            set(THREAD_NAME_MAX_VALUE 64)
            message(STATUS "  ✓ SetThreadDescription available")
        else()
            message(STATUS "  ✗ SetThreadDescription not available")
        endif()
    endif()
    
    # Linux: prctl(PR_SET_NAME)
    if (UNIX AND NOT APPLE AND NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for prctl(PR_SET_NAME) (Linux)...")
        check_source_compiles(C "
            #include <sys/prctl.h>
            int main(void) {
                prctl(PR_SET_NAME, \"test\", 0, 0, 0);
                return 0;
            }
        " HAVE_PRCTL_SET_NAME)
        
        if (HAVE_PRCTL_SET_NAME)
            target_compile_definitions(thread_lib PRIVATE HAVE_PRCTL_SET_NAME)
            target_compile_definitions(thread_lib PRIVATE THREAD_NAME_MAX=16)
            set(THREAD_NAMING_METHOD "prctl(PR_SET_NAME)")
            set(THREAD_NAME_MAX_VALUE 16)
            message(STATUS "  ✓ prctl(PR_SET_NAME) available (max length: 16)")
        else()
            message(STATUS "  ✗ prctl(PR_SET_NAME) not available")
        endif()
    endif()
    
    # macOS: pthread_setname_np (single argument)
    if (NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_setname_np (single arg, macOS)...")
        check_source_compiles(C "
            #include <pthread.h>
            int main(void) {
                pthread_setname_np(\"test\");
                return 0;
            }
        " HAVE_PTHREAD_SETNAME_NP_SINGLE_ARG)
        
        if (HAVE_PTHREAD_SETNAME_NP_SINGLE_ARG)
            target_compile_definitions(thread_lib PRIVATE HAVE_PTHREAD_SETNAME_NP_CURRENT)
            target_compile_definitions(thread_lib PRIVATE THREAD_NAME_MAX=64)
            set(THREAD_NAMING_METHOD "pthread_setname_np (current thread)")
            set(THREAD_NAME_MAX_VALUE 64)
            message(STATUS "  ✓ pthread_setname_np (single arg) available")
        else()
            message(STATUS "  ✗ pthread_setname_np (single arg) not available")
        endif()
    endif()
    
    # FreeBSD/OpenBSD: pthread_set_name_np
    if (NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_set_name_np (FreeBSD/OpenBSD)...")
        check_source_compiles(C "
            #include <pthread.h>
            #include <pthread_np.h>
            int main(void) {
                pthread_set_name_np(pthread_self(), \"test\");
                return 0;
            }
        " HAVE_PTHREAD_SET_NAME_NP)
        
        if (HAVE_PTHREAD_SET_NAME_NP)
            target_compile_definitions(thread_lib PRIVATE HAVE_PTHREAD_SET_NAME_NP)
            target_compile_definitions(thread_lib PRIVATE THREAD_NAME_MAX=32)
            set(THREAD_NAMING_METHOD "pthread_set_name_np")
            set(THREAD_NAME_MAX_VALUE 32)
            message(STATUS "  ✓ pthread_set_name_np available")
        else()
            message(STATUS "  ✗ pthread_set_name_np not available")
        endif()
    endif()
    
    # NetBSD: pthread_setname_np with format string
    if (NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_setname_np (format string, NetBSD)...")
        check_source_compiles(C "
            #include <pthread.h>
            int main(void) {
                pthread_setname_np(pthread_self(), \"%s\", \"test\");
                return 0;
            }
        " HAVE_PTHREAD_SETNAME_NP_NETBSD)
        
        if (HAVE_PTHREAD_SETNAME_NP_NETBSD)
            target_compile_definitions(thread_lib PRIVATE HAVE_PTHREAD_SETNAME_NP_NETBSD)
            target_compile_definitions(thread_lib PRIVATE THREAD_NAME_MAX=32)
            set(THREAD_NAMING_METHOD "pthread_setname_np (NetBSD)")
            set(THREAD_NAME_MAX_VALUE 32)
            message(STATUS "  ✓ pthread_setname_np (NetBSD format) available")
        else()
            message(STATUS "  ✗ pthread_setname_np (NetBSD format) not available")
        endif()
    endif()
    
    # Generic: pthread_setname_np (two arguments)
    if (NOT THREAD_NAMING_METHOD)
        message(STATUS "  Checking for pthread_setname_np (generic POSIX)...")
        check_source_compiles(C "
            #include <pthread.h>
            int main(void) {
                pthread_setname_np(pthread_self(), \"test\");
                return 0;
            }
        " HAVE_PTHREAD_SETNAME_NP_GENERIC)
        
        if (HAVE_PTHREAD_SETNAME_NP_GENERIC)
            target_compile_definitions(thread_lib PRIVATE HAVE_PTHREAD_SETNAME_NP_GENERIC)
            target_compile_definitions(thread_lib PRIVATE THREAD_NAME_MAX=64)
            set(THREAD_NAMING_METHOD "pthread_setname_np (generic)")
            set(THREAD_NAME_MAX_VALUE 64)
            message(STATUS "  ✓ pthread_setname_np (generic) available")
        else()
            message(STATUS "  ✗ pthread_setname_np (generic) not available")
        endif()
    endif()
    
    cmake_pop_check_state()
    
    # Summary
    if (THREAD_NAMING_METHOD STREQUAL "None")
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

## Windows: winmm (for ms timer functions), socket functions (even when networking is
## disabled. Also squelch the deprecation warnings (these warnings can be enabled
## via the -DENABLE_WINAPI_DEPRECATION_WARNINGS:Bool=On flag at configure
## time.)
if (WIN32)
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

## Cygwin also wants winmm. Note: Untested but should work.
if (CYGWIN)
  check_library_exists(winmm timeGetTime "" HAS_WINMM)
  if (HAS_WINMM)
    target_link_libraries(os_features INTERFACE ws2_32 winmm)
    target_compile_definitions(os_features INTERFACE HAVE_WINMM)
  endif ()
endif ()
