add_executable(playground playground.c tvout.c)
pico_generate_pio_header(playground ${CMAKE_CURRENT_LIST_DIR}/tvout.pio)
pico_enable_stdio_uart(playground 1)
target_link_libraries(
  playground
  pico_stdlib pico_sync
  hardware_pio hardware_clocks hardware_dma hardware_irq
)
pico_add_extra_outputs(playground)
