## I650 simulator
##
## Update this file as the local simulator sources and build metadata
## change. Keep shared target-construction logic in the common CMake helpers.
##
## Not yet: if this directory later gets a local unit-test subtree, declare
## it explicitly here with add_subdirectory(unit-tests).

add_simulator(i650
    SOURCES
        i650_cpu.c
        i650_cdr.c
        i650_cdp.c
        i650_dsk.c
        i650_mt.c
        i650_sys.c
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
    FEATURE_INT64
    LABEL I650
    PKG_FAMILY ibm_family
    TEST i650)

if (WIN32)
    if (MSVC)
        set(I650_STACK_FLAG "/STACK:8388608")
    elseif (CMAKE_C_COMPILER_ID STREQUAL "Clang")
        set(I650_STACK_FLAG "-Wl,/STACK:8388608")
    else ()
        set(I650_STACK_FLAG "-Wl,--stack,8388608")
    endif ()

    target_link_options(i650 PUBLIC "${I650_STACK_FLAG}")
endif()
