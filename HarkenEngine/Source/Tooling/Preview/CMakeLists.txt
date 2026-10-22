# harken_preview: the development-only typed current-world Playable control face.
#
# The gate is compile-time. HARKEN_DEVELOPMENT_PREVIEW is on for this repository's
# builds and can be configured off; with it off the entry points still exist and
# still return a stable refusal before parsing any descriptor or reserving any
# resource, which is the shape a shared binary layout would need. Both sides are
# built and tested -- a gate only one side compiles is a gate that has not been
# checked.
add_library(harken_preview STATIC
    src/PreviewAbi.cpp
    src/PreviewAbiLayout.cpp
)
target_include_directories(harken_preview
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${HARKEN_RUNTIME_DIR}/api/src
        ${HARKEN_RUNTIME_DIR}/core/include
        ${HARKEN_RUNTIME_DIR}/media/include
        ${HARKEN_RUNTIME_DIR}/param/include
)
target_link_libraries(harken_preview
    PRIVATE
        harken_core
        harken_buildinfo
        harken_voice_slot_layout_contract
)

option(HARKEN_DEVELOPMENT_PREVIEW
       "Build the development preview play surface with its gate open" ON)
if(HARKEN_DEVELOPMENT_PREVIEW)
    target_compile_definitions(harken_preview PUBLIC HARKEN_DEVELOPMENT_PREVIEW=1)
else()
    target_compile_definitions(harken_preview PUBLIC HARKEN_DEVELOPMENT_PREVIEW=0)
endif()

if(HARKEN_BUILD_EDITOR)
    add_library(harken_studio_runtime SHARED
        src/PreviewAbi.cpp
        src/PreviewAbiLayout.cpp
        src/PreviewProject.cpp)
    target_include_directories(harken_studio_runtime PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HARKEN_RUNTIME_DIR}/api/include
        PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES} ${HARKEN_RUNTIME_DIR}/api/src)
    target_link_libraries(harken_studio_runtime PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,harken_core>" harken_buildinfo)
    target_compile_definitions(harken_studio_runtime PRIVATE
        HARKEN_DEVELOPMENT_PREVIEW=$<BOOL:${HARKEN_DEVELOPMENT_PREVIEW}>)
    set_target_properties(harken_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(harken_studio_runtime PROPERTIES OUTPUT_NAME HarkenStudioRuntime)
    file(STRINGS "${HARKEN_RUNTIME_DIR}/api/abi/HarkenCore.def" _studio_exports
        REGEX "^[ \t]+Vn_[A-Za-z0-9_]+$")
    list(TRANSFORM _studio_exports STRIP)
    foreach(_header PreviewAbi.h PreviewProject.h)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/harken_preview/${_header}" _preview_header)
        string(REGEX MATCHALL "VnPreview_[A-Za-z0-9_]+[ \r\n]*\\(" _preview_entries "${_preview_header}")
        foreach(_entry IN LISTS _preview_entries)
            string(REGEX REPLACE "[ \r\n]*\\(" "" _entry "${_entry}")
            list(APPEND _studio_exports "${_entry}")
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/include/harken_preview/${_header}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${HARKEN_RUNTIME_DIR}/api/abi/HarkenCore.def")
    if(WIN32)
        list(JOIN _studio_exports "\n    " _studio_exports)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/HarkenStudioRuntime.def"
            CONTENT "EXPORTS\n    ${_studio_exports}\n" @ONLY)
        target_sources(harken_studio_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/HarkenStudioRuntime.def")
    elseif(APPLE)
        list(TRANSFORM _studio_exports PREPEND "_")
        list(JOIN _studio_exports "\n" _studio_exports)
        set(_studio_export_file "${CMAKE_CURRENT_BINARY_DIR}/HarkenStudioRuntime.exports")
        file(CONFIGURE OUTPUT "${_studio_export_file}" CONTENT "${_studio_exports}\n" @ONLY)
        target_link_options(harken_studio_runtime PRIVATE
            "LINKER:-exported_symbols_list,${_studio_export_file}")
        set_property(TARGET harken_studio_runtime APPEND PROPERTY LINK_DEPENDS "${_studio_export_file}")
    elseif(UNIX)
        list(JOIN _studio_exports ";\n    " _studio_exports)
        set(_studio_export_file "${CMAKE_CURRENT_BINARY_DIR}/HarkenStudioRuntime.map")
        file(CONFIGURE OUTPUT "${_studio_export_file}"
            CONTENT "{\n  global:\n    ${_studio_exports};\n  local: *;\n};\n" @ONLY)
        target_link_options(harken_studio_runtime PRIVATE "LINKER:--version-script,${_studio_export_file}")
        set_property(TARGET harken_studio_runtime APPEND PROPERTY LINK_DEPENDS "${_studio_export_file}")
    endif()

    add_library(harken_studio_runtime_api INTERFACE)
    target_include_directories(harken_studio_runtime_api INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${HARKEN_RUNTIME_DIR}/api/include)
    if(HARKEN_BUILD_EDITOR_UNIT_TESTS)
        target_link_libraries(harken_studio_runtime_api INTERFACE
            "$<IF:$<BOOL:$<TARGET_PROPERTY:HARKEN_INTERNAL_RUNTIME_TEST>>,harken_studio_runtime_test,harken_studio_runtime>")
    else()
        target_link_libraries(harken_studio_runtime_api INTERFACE harken_studio_runtime)
    endif()
endif()
