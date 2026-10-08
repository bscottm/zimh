add_unit_test(unit-pdp8-td
    LABEL PDP8
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pdp8_td.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/simulators/PDP8
        ${PROJECT_SOURCE_DIR}/tests/unit/support
)

add_unit_test(unit-pdp8-rf-attach
    LABEL PDP8
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pdp8_rf_attach.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/simulators/PDP8
)

add_unit_test(unit-pdp8-rx-attach
    LABEL PDP8
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pdp8_rx_attach.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/simulators/PDP8
)

add_unit_test(unit-pdp8-df-attach
    LABEL PDP8
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pdp8_df_attach.c
    INCLUDES
        ${PROJECT_SOURCE_DIR}/simulators/PDP8
)
