# harken_preview: the development-only typed current-world Playable control face,
# linked statically by the host. HarkenEngineDev compiles the same sources.
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
        ${HARKEN_DEVELOPMENT_DIR}/include
    PRIVATE
        ${HARKEN_DEVELOPMENT_PRIVATE_INCLUDES}
        ${HARKEN_CORE_DIR}/api/src
        ${HARKEN_CORE_DIR}/core/include
        ${HARKEN_CORE_DIR}/media/include
        ${HARKEN_PACK_FORMAT_INCLUDE_DIR}
        ${HARKEN_CORE_DIR}/param/include
)
target_link_libraries(harken_preview
    PRIVATE
        harken_core
        harken_buildinfo
)
target_compile_definitions(harken_preview PUBLIC
    HARKEN_DEVELOPMENT_PREVIEW=$<BOOL:${HARKEN_DEVELOPMENT_PREVIEW}>)
