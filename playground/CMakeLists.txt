add_executable(playground playground.c)
pico_generate_pio_header(playground ${CMAKE_CURRENT_LIST_DIR}/timing.pio)
target_link_libraries(playground pico_stdlib hardware_pio hardware_clocks)
pico_add_extra_outputs(playground)
