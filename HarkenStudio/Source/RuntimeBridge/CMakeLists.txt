# harken_runtime_bridge -- the editor's single point of contact with the
# engine. Public headers expose editor-owned Qt/std structs only; the private
# side talks to the published Harken::EngineDev through its public headers.

add_library(harken_runtime_bridge STATIC
    # Public interface, partitioned by migration cluster (one header each).
    PlaybackBridge.h
    PreviewBridge.h
    PreviewBridge.cpp
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
    MediaRegistry.h
    MediaRegistry.cpp
    MediaServices.h
    MediaServices.cpp
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
    Qt6::Core Qt6::Network harken_studio_types)

# Consumers include the interface as "RuntimeBridge/<Cluster>Bridge.h".
target_include_directories(harken_runtime_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_runtime_bridge PRIVATE Harken::EngineDev)

if(MSVC)
    target_compile_options(harken_runtime_bridge PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_runtime_bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
