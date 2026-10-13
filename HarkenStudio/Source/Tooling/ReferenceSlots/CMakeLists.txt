# harken_reference_slots: the reference-slot resolution rule (tooling layer,
# never part of a shipping game). The rule runs at program-compile time; the
# runtime consumes the resolved result and does not own a copy of the rule.
#
# Deliberately does NOT link harken_core: slot values cross this boundary as
# plain 64-bit ids, so the library stays consumable by the program compiler,
# the offline explain runner, and the tests alike. The dependency edge only
# ever points at this library, never out of it.
#
# Being Qt-free here is structural rather than a convention to remember: this
# subtree is never configured with Qt6, so a Qt include cannot compile.

add_library(harken_reference_slots STATIC
    src/ReferenceSlotResolver.cpp
)
# The generated vocabulary is checked in and consumed directly, so an ordinary
# build needs no Python. Regenerate with the harken_reference_slots_regen target
# after editing slots/harken_reference_slot_table.json.
target_include_directories(harken_reference_slots
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
)
target_link_libraries(harken_reference_slots PUBLIC harken_studio_types)
