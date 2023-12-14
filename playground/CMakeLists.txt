add_executable(playground
  playground.c
)
target_link_libraries(playground pico_stdlib)
pico_add_extra_outputs(playground)
