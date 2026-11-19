add_library(harken_playable_properties INTERFACE)
target_include_directories(harken_playable_properties INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(harken_playable_properties INTERFACE Harken::PackFormat)
