add_unit_test(unit-dynstr
    LABEL unit
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_dynstr.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/src/lib
        ${PROJECT_SOURCE_DIR}/tests/unit/support
)

add_unit_test(unit-xalloc
    LABEL unit
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xalloc.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/tests/unit/support
)

add_unit_test(unit-string-util
    LABEL unit
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_string_util.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/tests/unit/support
)

add_unit_test(unit-tailq
    LABEL unit
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_tailq.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/tests/unit/support
    USES_AIO
)

add_unit_test(unit-thread-name
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_thread_name.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/tests/unit/support
    DEFINES
        THREAD_NAMING_METHOD="${THREAD_NAMING_METHOD}"
    USES_AIO
)
