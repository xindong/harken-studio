# harken_program_registration: the compile/load-time step that resolves a compiled
# PieceProgram to device-rate scheduled work the runtime files verbatim. A tooling
# library because the resolution belongs to program registration, not to the audio
# runtime; the runtime consumes the finished ProgramViews through the planner it
# already has and re-derives none of the conversion.
#
# Links harken_core (for the clock, marker index and the planner's ProgramView type)
# and harken_project_schema (for the PieceProgram it consumes) -- the same tooling
# -> runtime direction harken_remote already takes. harken_core links neither this
# library nor the schema back, so the runtime/tooling isolation is preserved: the
# dependency edge only ever points at the runtime, never out of it. Qt-free like
# every other library in this subtree.
add_library(harken_program_registration STATIC
    src/ProgramRegistration.cpp
    src/VoiceStartInstall.cpp
    src/EffectDescriptorWriter.cpp
    src/BindingMaterialization.cpp
)
target_include_directories(harken_program_registration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # This library's public header exposes the runtime scheduling types
        # (ProgramView, ClockKernel, MarkerIndex), which harken_core keeps in its
        # private core/include root rather than the public C API. A consumer of
        # this header must resolve them too, so the root is carried PUBLIC -- a
        # build-time include of internal headers, not a new ABI or link edge.
        ${HARKEN_RUNTIME_DIR}/core/include
        ${HARKEN_RUNTIME_DIR}/core/contract/include
        ${HARKEN_RUNTIME_DIR}/media/include
        ${HARKEN_RUNTIME_DIR}/param/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/HarkenEngine/Test
        ${HARKEN_RUNTIME_DIR}/api/src
        ${HARKEN_RUNTIME_DIR}/dsp/include
)
target_link_libraries(harken_program_registration
    PUBLIC
        harken_core
        harken_project_schema
)
