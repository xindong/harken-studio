# harken_host: the CLI demo driver (spec section 17-1 mechanics) and the
# remote-serve wrapper around the remote-control channel. Not part of the
# shipping library; links harken_core like a game would (public C API),
# plus the internal null pump for the --null smoke mode (test-clock
# precedent) and miniaudio's decoder declarations (symbols come from
# harken_core's single implementation TU).
add_executable(harken_host
    Main.cpp
    Att03KeepProbe.cpp
    Att04NearFarProbe.cpp
    DownmixNullDrill.cpp
    PackPlay.cpp
    PreviewPlay.cpp
    RemoteServe.cpp
    SchedulingMeasure.cpp
    TraceReplay.cpp
)
harken_attach_version_resource(harken_host "Harken Host" "harken_host.exe")
add_dependencies(harken_host harken_schema_codegen)
target_link_libraries(harken_host PRIVATE harken_core harken_remote harken_buildinfo
                      harken_preview)
# The demo scenes' per-voice-tap silence verdicts branch on the stage-tap
# gating macro and must agree with the library about whether measurement
# is live (the master-bus verdicts need no gate — the bus output scan is
# compiled into every build from M6, spec section 11.7).
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_host PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()

add_executable(harken_host_selection_copy_test
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Test/UnitTest/PackPlaySelectionTest.cpp
)
target_include_directories(harken_host_selection_copy_test PRIVATE
    ${HARKEN_SCHEMA_GENERATED_CORE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${HARKEN_THIRD_PARTY_DIR}/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Common
)
target_link_libraries(harken_host_selection_copy_test PRIVATE harken_core)

target_include_directories(harken_host PRIVATE
    ${HARKEN_SCHEMA_GENERATED_CORE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${HARKEN_THIRD_PARTY_DIR}/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Common
)

if(WIN32)
    add_executable(harken_process_loopback_capture
        ProcessLoopbackCapture.cpp
    )
    harken_attach_version_resource(
        harken_process_loopback_capture
        "Harken Process Loopback Capture"
        "harken_process_loopback_capture.exe"
    )
    target_link_libraries(harken_process_loopback_capture PRIVATE
        harken_buildinfo
        mmdevapi
        ole32
    )
endif()
