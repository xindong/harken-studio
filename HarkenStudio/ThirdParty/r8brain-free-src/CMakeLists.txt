cmake_minimum_required(VERSION 3.21)

add_library(r8brain INTERFACE)

target_include_directories(r8brain INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
