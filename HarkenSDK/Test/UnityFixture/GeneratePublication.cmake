function(add_harken_publication_generator)
    if(NOT TARGET harken_editor_lib)
        message(FATAL_ERROR "Unity publication generation requires HARKEN_BUILD_EDITOR=ON")
    endif()
    add_executable(harken_unity_publication
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GeneratePublication.cpp")
    target_compile_features(harken_unity_publication PRIVATE cxx_std_20)
    target_link_libraries(harken_unity_publication PRIVATE harken_editor_lib)
    target_include_directories(harken_unity_publication PRIVATE
        "${CMAKE_SOURCE_DIR}/HarkenStudio/Test/UnitTest")
endfunction()
cmake_language(DEFER CALL add_harken_publication_generator)
