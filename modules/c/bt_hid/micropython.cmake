add_library(usermod_bt_hid INTERFACE)

target_sources(usermod_bt_hid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/bindings.c
)

target_include_directories(usermod_bt_hid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${MICROPY_DIR}/lib/btstack/src
)

target_link_libraries(usermod INTERFACE usermod_bt_hid)
