# harken_remote: the remote-control channel (tooling layer, never part of a
# shipping game). Embedded by harken_host behind the remote-serve verb; the
# engine is driven exclusively through the public C API.
#
# The wire layer (opcodes, marshalling, layout asserts) is machine-generated
# from wire/harken_wire_table.json into generated/ and CHECKED IN, so the
# build never depends on Python. After editing the table, build the
# harken_wire_regen utility target below.

add_library(harken_remote STATIC
    src/WireFormat.cpp
    src/WireDispatch.cpp
    src/RemoteServer.cpp
    src/PipeTransportWin.cpp
)
target_include_directories(harken_remote
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
)
target_link_libraries(harken_remote PUBLIC harken_core)

# Convenience regeneration target; never part of ALL (the products are
# checked in and the build must stay Python-free).
_harken_add_python_regen_target(
    harken_wire_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/wire/gen_wire.py"
    "Regenerating remote wire layer from harken_wire_table.json"
)
