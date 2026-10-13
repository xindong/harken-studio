add_library(harken_studio_types INTERFACE)
target_include_directories(harken_studio_types INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Types/include)

add_subdirectory(PackBuild)
add_subdirectory(ReferenceSlots)
add_subdirectory(PlayableProperties)
add_subdirectory(ProjectSchema)
add_subdirectory(BusCompiler)
add_subdirectory(CurrentWorldCompiler)
add_subdirectory(PackC)
