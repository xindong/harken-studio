# harken_project_schema: the Project v6 authoring domain model and the front of
# its compiler (parse -> stable identity -> structural/placement graph
# validation). A tooling library because this work belongs to program
# compilation; the runtime consumes a compiled program and never re-derives the
# schema or re-runs these checks.
#
# The schema is a candidate marked experimental: its serialization exists only
# to make fixtures legible and to prove determinism, and promises nothing about
# forward or backward persistence compatibility.

add_library(harken_project_schema STATIC
    src/CompileTargetProfile.cpp
    src/RuntimeIdentityIndex.cpp
    src/FrontEnd.cpp
    src/BuildSegmentIR.cpp
    src/CapacityEstimate.cpp
    src/BuildSegmentProgram.cpp
    src/PropertyFold.cpp
    src/ReferenceResolution.cpp
    src/SendResolution.cpp
    src/EffectChainSelection.cpp
    src/FixedSlotLowerer.cpp
    src/Serialize.cpp
    src/VariableContract.cpp
)
target_include_directories(harken_project_schema
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(harken_project_schema PUBLIC harken_studio_pack_build)
# The reference-slot resolution core. PUBLIC because a public tooling header
# (ReferenceResolution.h) embeds harkenreferenceslots resolution types, so a
# consumer of this library resolves that header too. This is the reuse of the
# single resolution rule — this library wires the chains and consumes the core,
# it does not re-implement precedence.
target_link_libraries(harken_project_schema PUBLIC harken_reference_slots)
target_link_libraries(harken_project_schema PUBLIC harken_playable_properties)
