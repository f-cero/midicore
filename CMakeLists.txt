cmake_minimum_required(VERSION 3.13)
project(midicore C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(midicore STATIC
    src/synth.c
    src/smf.c
    src/api.c
    src/mc_tables.c
)
target_include_directories(midicore PUBLIC include)
target_include_directories(midicore PRIVATE src)

if(NOT MSVC)
  target_compile_options(midicore PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(host_render examples/host_render.c)
target_link_libraries(host_render PRIVATE midicore)
