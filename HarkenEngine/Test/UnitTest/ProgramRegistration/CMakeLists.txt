# harken_program_registration: Engine-side test support that installs voice
# starts, effect descriptors and binding materializations through the runtime's
# own internal types. Test-only; nothing shipped links it.
add_library(harken_program_registration STATIC
    src/VoiceStartInstall.cpp
    src/EffectDescriptorWriter.cpp
    src/BindingMaterialization.cpp
)
target_include_directories(harken_program_registration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HARKEN_CORE_DIR}/core/include
        ${HARKEN_CORE_DIR}/media/include
        ${HARKEN_PACK_FORMAT_INCLUDE_DIR}
        ${HARKEN_CORE_DIR}/param/include
    PRIVATE
        ${HARKEN_CORE_DIR}/api/src
        ${HARKEN_DEVELOPMENT_PRIVATE_INCLUDES}
        ${HARKEN_CORE_DIR}/dsp/include
)
target_link_libraries(harken_program_registration PUBLIC harken_core)
