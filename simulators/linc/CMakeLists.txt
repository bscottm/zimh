## LINC simulator
##
## Update this file as the local simulator sources and build metadata
## change. Keep shared target-construction logic in the common CMake helpers.
##
## Not yet: if this directory later gets a local unit-test subtree, declare
## it explicitly here with add_subdirectory(unit-tests).

add_simulator(linc
    SOURCES
        linc_cpu.c
        linc_crt.c
        linc_dpy.c
        linc_kbd.c
        linc_sys.c
        linc_tape.c
        linc_tty.c
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
    FEATURE_DISPLAY
    USES_AIO
    LABEL linc
    PKG_FAMILY default_family
    TEST linc)

# Force serial execution to avoid parallel test conflicts.
# LINC uses both FEATURE_DISPLAY (SDL) and USES_AIO (threads), which causes
# resource conflicts when multiple tests run simultaneously. See TIMEOUT_INVESTIGATION.md.
set_tests_properties(zimh-linc PROPERTIES RUN_SERIAL TRUE)
