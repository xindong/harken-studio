# harken_runtime_bridge -- the editor's single point of contact with the
# runtime. Public headers expose editor-owned Qt/std structs only; the private
# side talks to harken_core through the in-process Harken.h C API.

add_library(harken_runtime_bridge STATIC
    # Public interface, partitioned by migration cluster (one header each).
    PlaybackBridge.h
    PreviewBridge.h
    PreviewBridge.cpp
    PackagingBridge.h
    ProjectSyncBridge.h
    CurrentWorldSyncBridge.h
    CurrentWorldSyncBridgeInternal.h
    DiagnosticsBridge.h
    IpcBridge.h
    # Private side. Conversion editor-type <-> runtime-type lives here.
    BusTopologySync.cpp
    BusTopologySync.h
    RuntimeLink.cpp
    RuntimeLink.h
    RuntimeLinkInternal.h
    MonitorBridge.h
    MonitorBridge.cpp
    MeterBridge.h
    MeterBridge.cpp
    PackagingBridge.cpp
    PlaybackBridge.cpp
    ProjectSyncBridge.cpp
    CurrentWorldSyncBridge.cpp
    DiagnosticsBridge.cpp
    IpcBridge.cpp
    RemoteConnection.cpp
    RemoteConnection.h
)

set_target_properties(harken_runtime_bridge PROPERTIES AUTOMOC ON)

# The public interface carries Qt value types (QString/QByteArray/QStringList).
target_link_libraries(harken_runtime_bridge PUBLIC
    Qt6::Core Qt6::Network harken_studio_types harken_studio_pack_build)

# Consumers include the interface as "RuntimeBridge/<Cluster>Bridge.h".
target_include_directories(harken_runtime_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_include_directories(harken_runtime_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/miniaudio
)

target_link_libraries(harken_runtime_bridge PRIVATE
    harken_current_world_compiler
    harken_bus_compiler
    harken_studio_runtime_api
    harken_studio_media_decode
    harken_remote_protocol
    vorbisfile
)

# Reference-slot resolution is shared with the program compiler and the
# offline explain runner, so the editor consumes the same rule rather than
# keeping its own copy. Wired here while the bridge has no call sites yet:
# proving the include path and symbol relationship now is cheaper than
# discovering a conflict once authoring code depends on it. Guarded because
# the library belongs to the desktop tooling layer, which a runtime-only
# configuration does not build.
if(TARGET harken_reference_slots)
    target_link_libraries(harken_runtime_bridge PRIVATE harken_reference_slots)
endif()

if(MSVC)
    target_compile_options(harken_runtime_bridge PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_runtime_bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
