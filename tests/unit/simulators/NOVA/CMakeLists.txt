add_unit_test(unit-nova-dsk-attach
    LABEL NOVA
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_nova_dsk_attach.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/simulators/NOVA
)
