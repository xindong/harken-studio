# Harken Engine — the shipping (HarkenEngine) and development (HarkenEngineDev)
# editions, the internal static runtime the Engine's own tests and tools link,
# and (desktop only) the host-side tooling. Everything the Engine builds is
# defined here; no other product directory is read.

set(HARKEN_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core)
set(HARKEN_DEVELOPMENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Source/Development)
set(HARKEN_THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(HARKEN_PACK_FORMAT_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include-pack)
file(GLOB HARKEN_PACK_FORMAT_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Format/Pack/*.h)
foreach(_header IN LISTS HARKEN_PACK_FORMAT_HEADERS)
    get_filename_component(_name ${_header} NAME)
    configure_file(${_header} ${HARKEN_PACK_FORMAT_INCLUDE_DIR}/harken_pack/${_name} COPYONLY)
endforeach()
add_library(harken_pack_format INTERFACE)
add_library(Harken::PackFormat ALIAS harken_pack_format)
target_include_directories(harken_pack_format INTERFACE
    $<BUILD_INTERFACE:${HARKEN_PACK_FORMAT_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(harken_pack_format PROPERTIES EXPORT_NAME PackFormat)

option(HARKEN_RT_PROBE_DEVICE_CALLBACK
       "Instrument Runtime-owned work inside the production device callback" OFF)
option(HARKEN_RT_PROBE_MIX_PUBLICATION
       "Instrument the end-of-block Runtime publication window" OFF)
option(HARKEN_RT_PROBE_WORKER_BARRIER
       "Instrument the existing render-worker completion barrier" OFF)

set(HARKEN_RT_PROBE_DEFINITIONS "")
foreach(_harken_rt_probe_control
        HARKEN_RT_PROBE_DEVICE_CALLBACK
        HARKEN_RT_PROBE_MIX_PUBLICATION
        HARKEN_RT_PROBE_WORKER_BARRIER)
    if(${_harken_rt_probe_control})
        list(APPEND HARKEN_RT_PROBE_DEFINITIONS
             ${_harken_rt_probe_control})
    endif()
endforeach()
if(HARKEN_RT_PROBE_DEFINITIONS AND NOT HARKEN_BUILD_RT_QUALIFICATION)
    message(FATAL_ERROR
        "HARKEN_RT_PROBE_* controls require "
        "HARKEN_BUILD_RT_QUALIFICATION=ON; shipping Runtime targets never "
        "accept probe definitions")
endif()

# Core is everything a shipping game needs; Development adds observation,
# tracing, remote connection, inspection and live project editing.
set(HARKEN_CORE_SOURCES
    ${HARKEN_CORE_DIR}/api/src/PackInspection.cpp
    ${HARKEN_CORE_DIR}/api/src/DeviceBackend.cpp
    ${HARKEN_CORE_DIR}/api/src/RealDeviceInitStage.cpp
    ${HARKEN_CORE_DIR}/api/src/MiniaudioImpl.cpp
    ${HARKEN_CORE_DIR}/api/src/HarkenApi.cpp
    ${HARKEN_CORE_DIR}/api/src/PlaybackReadiness.cpp
    ${HARKEN_CORE_DIR}/api/src/TopologySlotPlan.cpp
    ${HARKEN_CORE_DIR}/api/src/SignalPublication.cpp
    ${HARKEN_CORE_DIR}/api/src/PackVariablePublication.cpp
    ${HARKEN_CORE_DIR}/core/src/AllocGuard.cpp
    ${HARKEN_CORE_DIR}/core/src/StableRowStorage.cpp
    ${HARKEN_CORE_DIR}/core/src/MonitorRequest.cpp
    ${HARKEN_CORE_DIR}/core/src/CpuClock.cpp
    ${HARKEN_CORE_DIR}/core/src/CurrentSelection.cpp
    ${HARKEN_CORE_DIR}/core/src/CurrentSelectionMemory.cpp
    ${HARKEN_CORE_DIR}/core/src/CurrentWorld.cpp
    ${HARKEN_CORE_DIR}/core/src/CurrentWorldArtifact.cpp
    ${HARKEN_CORE_DIR}/core/src/CurrentWorldCodec.cpp
    ${HARKEN_CORE_DIR}/core/src/ActionFiringTable.cpp
    ${HARKEN_CORE_DIR}/core/src/ActionSeekTable.cpp
    ${HARKEN_CORE_DIR}/core/src/StreamSeekHandoffTable.cpp
    ${HARKEN_CORE_DIR}/core/src/ActionCooldownTable.cpp
    ${HARKEN_CORE_DIR}/core/src/ActionDeferredRecordTable.cpp
    ${HARKEN_CORE_DIR}/core/src/ArmGroupTable.cpp
    ${HARKEN_CORE_DIR}/core/src/AudioSliceLease.cpp
    ${HARKEN_CORE_DIR}/core/src/DecisionTable.cpp
    ${HARKEN_CORE_DIR}/core/src/CompletionProgress.cpp
    ${HARKEN_CORE_DIR}/core/src/ResamplerAdvance.cpp
    ${HARKEN_CORE_DIR}/core/src/CompletionOwner.cpp
    ${HARKEN_CORE_DIR}/core/src/CompletionMutationBinding.cpp
    ${HARKEN_CORE_DIR}/core/src/CompositeCompletionOwnerPool.cpp
    ${HARKEN_CORE_DIR}/core/src/KnownCompletion.cpp
    ${HARKEN_CORE_DIR}/core/src/PlayableProgramKey.cpp
    ${HARKEN_CORE_DIR}/core/src/PlayablePropertyCut.cpp
    ${HARKEN_CORE_DIR}/core/src/BlockArena.cpp
    ${HARKEN_CORE_DIR}/core/src/ClockKernel.cpp
    ${HARKEN_CORE_DIR}/core/src/MarkerIndex.cpp
    ${HARKEN_CORE_DIR}/core/src/MusicPlaybackTimeline.cpp
    ${HARKEN_CORE_DIR}/core/src/DeferredIntentPool.cpp
    ${HARKEN_CORE_DIR}/core/src/EngineConfig.cpp
    ${HARKEN_CORE_DIR}/core/src/LogicCommitCoordinator.cpp
    ${HARKEN_CORE_DIR}/core/src/LogicReasonCode.cpp
    ${HARKEN_CORE_DIR}/core/src/OccurrenceStatePool.cpp
    ${HARKEN_CORE_DIR}/core/src/PackComposition.cpp
    ${HARKEN_CORE_DIR}/core/src/PlayableInstanceTable.cpp
    ${HARKEN_CORE_DIR}/core/src/ContainerPlaybackTable.cpp
    ${HARKEN_CORE_DIR}/core/src/QueueRuntime.cpp
    ${HARKEN_CORE_DIR}/core/src/PrivateCatalogStage.cpp
    ${HARKEN_CORE_DIR}/core/src/VariableControlGrid.cpp
    ${HARKEN_CORE_DIR}/core/src/VariableControlRuntime.cpp
    ${HARKEN_CORE_DIR}/core/src/ProgramGenerationRegistry.cpp
    ${HARKEN_CORE_DIR}/core/src/SamePositionResolver.cpp
    ${HARKEN_CORE_DIR}/core/src/ScheduledTransactionPool.cpp
    ${HARKEN_CORE_DIR}/core/src/ImmediateActionLane.cpp
    ${HARKEN_CORE_DIR}/core/src/TimelinePlanner.cpp
    ${HARKEN_CORE_DIR}/core/src/TimelineArmTable.cpp
    ${HARKEN_CORE_DIR}/core/src/TimelineExecutor.cpp
    ${HARKEN_CORE_DIR}/core/src/TransitionInstancePool.cpp
    ${HARKEN_CORE_DIR}/core/src/VariableSliceTable.cpp
    ${HARKEN_CORE_DIR}/core/src/Utf8Path.cpp
    ${HARKEN_CORE_DIR}/core/src/VoiceLedger.cpp
    ${HARKEN_CORE_DIR}/core/src/VoiceManager.cpp
    ${HARKEN_CORE_DIR}/dsp/src/MonoStereoMix.cpp
    ${HARKEN_CORE_DIR}/dsp/src/MonoStereoMixAvx.cpp
    ${HARKEN_CORE_DIR}/dsp/src/Resampler.cpp
    ${HARKEN_CORE_DIR}/dsp/src/ResamplerAvx2.cpp
    ${HARKEN_CORE_DIR}/media/src/PackMediaRange.cpp
    ${HARKEN_CORE_DIR}/media/src/PcmRegistry.cpp
    ${HARKEN_CORE_DIR}/media/src/ResolvedSlotTicket.cpp
    ${HARKEN_CORE_DIR}/media/src/ResourceFacts.cpp
    ${HARKEN_CORE_DIR}/media/src/PackLogicImage.cpp
    ${HARKEN_CORE_DIR}/media/src/PackTable.cpp
    ${HARKEN_CORE_DIR}/media/src/StreamManager.cpp
    ${HARKEN_CORE_DIR}/media/src/StreamByteSource.cpp
    ${HARKEN_CORE_DIR}/media/src/StreamManagerIo.cpp
    ${HARKEN_CORE_DIR}/param/src/BindingTable.cpp
    ${HARKEN_CORE_DIR}/param/src/BusTargetStore.cpp
    ${HARKEN_CORE_DIR}/param/src/Curve.cpp
    ${HARKEN_CORE_DIR}/param/src/VariableService.cpp
    ${HARKEN_CORE_DIR}/param/src/EffectParamTargetStore.cpp
    ${HARKEN_CORE_DIR}/param/src/EmitterTable.cpp
    ${HARKEN_CORE_DIR}/param/src/MeterProbe.cpp
    ${HARKEN_CORE_DIR}/param/src/MeterSystem.cpp
    ${HARKEN_CORE_DIR}/param/src/ParamSystem.cpp
    ${HARKEN_CORE_DIR}/param/src/PropertyPublisher.cpp
    ${HARKEN_CORE_DIR}/param/src/Spatializer.cpp
    ${HARKEN_CORE_DIR}/render/src/BuiltinEffects.cpp
    ${HARKEN_CORE_DIR}/render/src/BusTree.cpp
    ${HARKEN_CORE_DIR}/render/src/EffectPool.cpp
    ${HARKEN_CORE_DIR}/render/src/InsertEffect.cpp
    ${HARKEN_CORE_DIR}/render/src/MixerLoop.cpp
    ${HARKEN_CORE_DIR}/render/src/NullBackend.cpp
    ${HARKEN_CORE_DIR}/render/src/RenderWorkerPool.cpp
    ${HARKEN_CORE_DIR}/render/src/TopologyPlan.cpp
    ${HARKEN_CORE_DIR}/render/src/VoiceCascade.cpp
    ${HARKEN_CORE_DIR}/render/src/VoicePool.cpp
    ${HARKEN_CORE_DIR}/render/src/SignalRouting.cpp
    ${HARKEN_CORE_DIR}/render/src/FrontierTimelineFinalizer.cpp
    ${HARKEN_CORE_DIR}/render/src/ImmediateLaneTimelineFinalizer.cpp
)

set(HARKEN_DEVELOPMENT_SOURCES
    ${HARKEN_DEVELOPMENT_DIR}/api/DevelopmentApi.cpp
    ${HARKEN_DEVELOPMENT_DIR}/api/SnapshotApi.cpp
    ${HARKEN_DEVELOPMENT_DIR}/api/MonitorApi.cpp
    ${HARKEN_DEVELOPMENT_DIR}/generation/ProjectGenerationTransaction.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorCaptureHub.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorDerivedDispatch.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorMeterHandler.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorMeterReduce.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorScopeHandler.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorScopeTrigger.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorSpectrumFft.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorSpectrumHandler.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorWaveformHandler.cpp
    ${HARKEN_DEVELOPMENT_DIR}/monitor/MonitorWaveformReduce.cpp
    ${HARKEN_DEVELOPMENT_DIR}/remote/src/RemoteClient.cpp
    ${HARKEN_DEVELOPMENT_DIR}/remote/src/RemoteRuntime.cpp
    ${HARKEN_DEVELOPMENT_DIR}/trace/TraceCodec.cpp
    ${HARKEN_DEVELOPMENT_DIR}/trace/TraceRecorder.cpp
)
# Only this isolated kernel may require AVX; startup and fallback stay baseline.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND
   MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64")
    set_source_files_properties(${HARKEN_CORE_DIR}/dsp/src/MonoStereoMixAvx.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX;/fp:strict")
    set_source_files_properties(${HARKEN_CORE_DIR}/dsp/src/ResamplerAvx2.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:strict")
endif()

# Private include dirs shared by the static and shared runtime libraries.
set(HARKEN_RUNTIME_PRIVATE_INCLUDES
    ${HARKEN_BI_GENERATED_DIR}
    ${HARKEN_PACK_FORMAT_INCLUDE_DIR}
    ${HARKEN_CORE_DIR}/core/include
    ${HARKEN_CORE_DIR}/dsp/include
    ${HARKEN_CORE_DIR}/media/include
    ${HARKEN_CORE_DIR}/param/include
    ${HARKEN_CORE_DIR}/render/include
    ${HARKEN_THIRD_PARTY_DIR}/miniaudio
)
set(HARKEN_DEVELOPMENT_PRIVATE_INCLUDES
    ${HARKEN_DEVELOPMENT_DIR}/include
    ${HARKEN_DEVELOPMENT_DIR}/generation
    ${HARKEN_DEVELOPMENT_DIR}/monitor
    ${HARKEN_DEVELOPMENT_DIR}/remote/src
    ${HARKEN_DEVELOPMENT_DIR}/trace
)

# Platform link requirements of the miniaudio device backend. CI and every
# automated test run on the null backend and need none of these; they matter
# only where a real device backend is compiled in. Apple (macOS/iOS) links Core
# Audio and its supporting frameworks; iOS additionally needs AVFoundation for
# the audio session. Android's AAudio/OpenSL ES are dlopen'd by miniaudio at
# runtime, needing only libdl (part of bionic libc), so nothing extra is linked.
# Windows (WASAPI) and Linux need no extra libraries. Declared as usage
# requirements so they propagate to whatever links harken_core (the shared
# library, the iOS app that folds in the static lib, the tests).
set(HARKEN_PLATFORM_AUDIO_LIBS "")
if(APPLE)
    list(APPEND HARKEN_PLATFORM_AUDIO_LIBS
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox")
    if(IOS)
        list(APPEND HARKEN_PLATFORM_AUDIO_LIBS "-framework AVFoundation")
    endif()
endif()

if(IOS)
    enable_language(OBJCXX)
    set_source_files_properties(
        ${HARKEN_CORE_DIR}/api/src/MiniaudioImpl.cpp
        PROPERTIES LANGUAGE OBJCXX)
endif()

option(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE
       "Enable the pre-init timeline capacity override in the test-linked static runtime"
       OFF)
if(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE AND NOT HARKEN_BUILD_RUNTIME_UNIT_TESTS)
    message(FATAL_ERROR
        "HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE requires HARKEN_BUILD_RUNTIME_UNIT_TESTS")
endif()

# Per-voice stage taps follow SnapshotBuffer.h's declared gating: Debug
# always measures, Release measures only by this explicit opt-in. The
# unconditional Release measurement that once lived here protected the
# host demo's master-bus non-silence evidence; from M6 the BUS OUTPUT
# peak scan is compiled into every build unconditionally (spec section
# 11.7), so that evidence no longer needs the opt-in — only the per-voice
# tap diagnostics do. The option feeds EVERY target whose sources branch
# on the gating macro (library, tests, benchmarks, host), so library and
# consumers always agree on which branch is live; flipping it ON restores
# the previous all-taps Release measurement for diagnostic sessions. The
# snapshot layout is build-invariant either way.
option(HARKEN_ENABLE_STAGE_TAPS
       "Compile the per-voice stage-tap peak scans into Release builds (Debug always measures; the bus output scan is unconditional)"
       OFF)

set(HARKEN_BUILD_INTERNAL_RUNTIME OFF)
if(HARKEN_BUILD_TOOLS OR HARKEN_BUILD_RUNTIME_UNIT_TESTS OR
   HARKEN_BUILD_RUNTIME_BENCHMARKS OR HARKEN_BUILD_RT_QUALIFICATION)
    set(HARKEN_BUILD_INTERNAL_RUNTIME ON)
endif()

if(HARKEN_BUILD_INTERNAL_RUNTIME)
add_library(harken_core STATIC ${HARKEN_CORE_SOURCES} ${HARKEN_DEVELOPMENT_SOURCES})
target_link_libraries(harken_core PRIVATE harken_buildinfo)
target_compile_definitions(harken_core PUBLIC HARKEN_ENGINE_DEV=1)
set_target_properties(harken_core PROPERTIES OUTPUT_NAME HarkenCore)
if(HARKEN_BUILD_RUNTIME_UNIT_TESTS)
    # Native lifetime observation belongs to the test-linked static runtime.
    # It does not change capacity or enter the shared shipping runtime.
    target_compile_definitions(harken_core PUBLIC
        HARKEN_TEST_PACK_RESOURCE_OBSERVATION)
endif()
if(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    # Internal Engine headers must use the same test layout as this library.
    target_compile_definitions(harken_core PUBLIC
        HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
endif()
target_include_directories(harken_core
    PUBLIC ${HARKEN_CORE_DIR}/api/include ${HARKEN_DEVELOPMENT_DIR}/include
    PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
            ${HARKEN_DEVELOPMENT_PRIVATE_INCLUDES}
            ${HARKEN_CORE_DIR}/api/src
)
if(HARKEN_PLATFORM_AUDIO_LIBS)
    target_link_libraries(harken_core PUBLIC ${HARKEN_PLATFORM_AUDIO_LIBS})
endif()
if(WIN32)
    target_link_libraries(harken_core PUBLIC advapi32 ws2_32 iphlpapi)
endif()
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_core PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()
endif()

if(HARKEN_BUILD_RUNTIME_UNIT_TESTS)
    add_library(harken_core_release STATIC ${HARKEN_CORE_SOURCES})
    target_link_libraries(harken_core_release PRIVATE harken_buildinfo)
    target_include_directories(harken_core_release
        PUBLIC ${HARKEN_CORE_DIR}/api/include
        PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
                ${HARKEN_CORE_DIR}/api/src)
    target_compile_definitions(harken_core_release PUBLIC
        HARKEN_ENGINE_DEV=0 HARKEN_TEST_PACK_RESOURCE_OBSERVATION)
    if(HARKEN_PLATFORM_AUDIO_LIBS)
        target_link_libraries(harken_core_release PUBLIC ${HARKEN_PLATFORM_AUDIO_LIBS})
    endif()
    if(WIN32)
        target_link_libraries(harken_core_release PUBLIC advapi32)
    endif()
    if(HARKEN_ENABLE_STAGE_TAPS)
        target_compile_definitions(harken_core_release PRIVATE HARKEN_ENABLE_STAGE_TAPS)
    endif()
    if(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
        target_compile_definitions(harken_core_release PUBLIC
            HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    endif()

    add_library(harken_core_hub_absent STATIC ${HARKEN_CORE_SOURCES} ${HARKEN_DEVELOPMENT_SOURCES})
    target_link_libraries(harken_core_hub_absent PRIVATE harken_buildinfo)
    target_include_directories(harken_core_hub_absent
        PUBLIC ${HARKEN_CORE_DIR}/api/include ${HARKEN_DEVELOPMENT_DIR}/include
        PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
                ${HARKEN_DEVELOPMENT_PRIVATE_INCLUDES}
                ${HARKEN_CORE_DIR}/api/src)
    target_compile_definitions(harken_core_hub_absent PUBLIC HARKEN_ENGINE_DEV=1 PRIVATE HARKEN_MONITOR_HUB_ABSENT)
    if(HARKEN_ENABLE_STAGE_TAPS)
        target_compile_definitions(harken_core_hub_absent PRIVATE HARKEN_ENABLE_STAGE_TAPS)
    endif()
    if(HARKEN_PLATFORM_AUDIO_LIBS)
        target_link_libraries(harken_core_hub_absent PUBLIC ${HARKEN_PLATFORM_AUDIO_LIBS})
    endif()
    if(WIN32)
        target_link_libraries(harken_core_hub_absent PUBLIC advapi32 ws2_32 iphlpapi)
    endif()
endif()

option(HARKEN_DEVELOPMENT_PREVIEW
       "Build the development preview play surface with its gate open" ON)

# The two published editions. Both build from the same runtime sources; the
# development edition adds the Development surface and compiles Core with
# HARKEN_ENGINE_DEV=1. Shared libraries everywhere except iOS, which links the
# engine statically as __Internal. Consumers see only the staged public headers.
set(HARKEN_ENGINE_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include)
set(HARKEN_ENGINE_DEV_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include-dev)
set(HARKEN_ENGINE_PUBLIC_HEADERS
    ${HARKEN_CORE_DIR}/api/include/harken/Harken.h
    ${HARKEN_CORE_DIR}/api/include/harken/ObjectId.h)
set(HARKEN_ENGINE_DEV_PUBLIC_HEADERS
    ${HARKEN_DEVELOPMENT_DIR}/include/harken/HarkenDevelopment.h)
foreach(_header IN LISTS HARKEN_ENGINE_PUBLIC_HEADERS)
    get_filename_component(_name ${_header} NAME)
    configure_file(${_header} ${HARKEN_ENGINE_PUBLIC_INCLUDE_DIR}/harken/${_name} COPYONLY)
endforeach()
foreach(_header IN LISTS HARKEN_ENGINE_DEV_PUBLIC_HEADERS)
    get_filename_component(_name ${_header} NAME)
    configure_file(${_header} ${HARKEN_ENGINE_DEV_PUBLIC_INCLUDE_DIR}/harken/${_name} COPYONLY)
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(HARKEN_ENGINE_LIBRARY_TYPE STATIC)
else()
    set(HARKEN_ENGINE_LIBRARY_TYPE SHARED)
endif()

function(harken_add_engine_edition target output_name export_name)
    add_library(${target} ${HARKEN_ENGINE_LIBRARY_TYPE} ${HARKEN_CORE_SOURCES} ${ARGN})
    add_library(Harken::${export_name} ALIAS ${target})
    target_link_libraries(${target} PRIVATE $<BUILD_INTERFACE:harken_buildinfo>)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${HARKEN_ENGINE_PUBLIC_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${HARKEN_CORE_DIR}/api/include
            ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
            ${HARKEN_CORE_DIR}/api/src)
    if(HARKEN_ENABLE_STAGE_TAPS)
        target_compile_definitions(${target} PRIVATE HARKEN_ENABLE_STAGE_TAPS)
    endif()
    if(ANDROID)
        # Keep the Unity plugin self-contained and consistent with the Android
        # presets' ANDROID_STL=c++_static policy, and keep every LOAD segment
        # compatible with 16 KiB page-size devices.
        target_link_libraries(${target} PRIVATE c++_static c++abi)
        target_link_options(${target} PRIVATE
            "-Wl,-z,max-page-size=16384"
            "-Wl,-z,common-page-size=16384")
    endif()
    if(HARKEN_PLATFORM_AUDIO_LIBS)
        target_link_libraries(${target} PRIVATE ${HARKEN_PLATFORM_AUDIO_LIBS})
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE advapi32)
    endif()
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${output_name}
        EXPORT_NAME ${export_name})
endfunction()

if(HARKEN_BUILD_ENGINE)
    harken_add_engine_edition(harken_engine HarkenEngine Engine)
    target_compile_definitions(harken_engine PRIVATE HARKEN_ENGINE_DEV=0)
    if(WIN32)
        # The export surface is a reviewed allowlist: CMake forwards the
        # checked-in .def to the linker as /DEF:. Regenerate it from Harken.h
        # with Source/Core/api/abi/gen_abi_exports.py after an entry point
        # changes; the abi export checks fail on drift.
        target_sources(harken_engine PRIVATE ${HARKEN_CORE_DIR}/api/abi/HarkenEngine.def)
    endif()
endif()

if(HARKEN_BUILD_ENGINE_DEV)
    set(_harken_preview_dir ${CMAKE_CURRENT_SOURCE_DIR}/Source/Development/preview)
    harken_add_engine_edition(harken_engine_dev HarkenEngineDev EngineDev
        ${HARKEN_DEVELOPMENT_SOURCES}
        ${_harken_preview_dir}/src/PreviewAbi.cpp
        ${_harken_preview_dir}/src/PreviewAbiLayout.cpp
        ${_harken_preview_dir}/src/PreviewProject.cpp)
    target_include_directories(harken_engine_dev
        PUBLIC $<BUILD_INTERFACE:${HARKEN_ENGINE_DEV_PUBLIC_INCLUDE_DIR}>
        PRIVATE ${HARKEN_DEVELOPMENT_PRIVATE_INCLUDES})
    if(WIN32)
        target_link_libraries(harken_engine_dev PRIVATE ws2_32 iphlpapi)
    endif()
    target_compile_definitions(harken_engine_dev PRIVATE
        HARKEN_ENGINE_DEV=1
        HARKEN_DEVELOPMENT_PREVIEW=$<BOOL:${HARKEN_DEVELOPMENT_PREVIEW}>)
    file(STRINGS "${HARKEN_CORE_DIR}/api/abi/HarkenEngine.def" _dev_exports
        REGEX "^[ \t]+Vn_[A-Za-z0-9_]+$")
    list(TRANSFORM _dev_exports STRIP)
    foreach(_header IN LISTS HARKEN_ENGINE_DEV_PUBLIC_HEADERS)
        file(READ "${_header}" _dev_header)
        string(REGEX MATCHALL "Vn(Preview)?_[A-Za-z0-9_]+[ \r\n]*\\(" _dev_entries "${_dev_header}")
        foreach(_entry IN LISTS _dev_entries)
            string(REGEX REPLACE "[ \r\n]*\\(" "" _entry "${_entry}")
            list(APPEND _dev_exports "${_entry}")
        endforeach()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${HARKEN_ENGINE_DEV_PUBLIC_HEADERS}
        "${HARKEN_CORE_DIR}/api/abi/HarkenEngine.def")
    if(WIN32)
        list(JOIN _dev_exports "\n    " _dev_exports)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineDev.def"
            CONTENT "EXPORTS\n    ${_dev_exports}\n" @ONLY)
        target_sources(harken_engine_dev PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineDev.def")
    elseif(APPLE AND HARKEN_ENGINE_LIBRARY_TYPE STREQUAL "SHARED")
        list(TRANSFORM _dev_exports PREPEND "_")
        list(JOIN _dev_exports "\n" _dev_exports)
        set(_dev_export_file "${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineDev.exports")
        file(CONFIGURE OUTPUT "${_dev_export_file}" CONTENT "${_dev_exports}\n" @ONLY)
        target_link_options(harken_engine_dev PRIVATE
            "LINKER:-exported_symbols_list,${_dev_export_file}")
        set_property(TARGET harken_engine_dev APPEND PROPERTY LINK_DEPENDS "${_dev_export_file}")
    elseif(UNIX AND HARKEN_ENGINE_LIBRARY_TYPE STREQUAL "SHARED")
        list(JOIN _dev_exports ";\n    " _dev_exports)
        set(_dev_export_file "${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineDev.map")
        file(CONFIGURE OUTPUT "${_dev_export_file}"
            CONTENT "{\n  global:\n    ${_dev_exports};\n  local: *;\n};\n" @ONLY)
        target_link_options(harken_engine_dev PRIVATE "LINKER:--version-script,${_dev_export_file}")
        set_property(TARGET harken_engine_dev APPEND PROPERTY LINK_DEPENDS "${_dev_export_file}")
    endif()
endif()

set(_harken_installed_editions)
foreach(_edition harken_engine harken_engine_dev)
    if(TARGET ${_edition})
        list(APPEND _harken_installed_editions ${_edition})
    endif()
endforeach()
install(TARGETS ${_harken_installed_editions} harken_pack_format
    EXPORT HarkenEngineTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${HARKEN_ENGINE_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/harken)
install(FILES ${HARKEN_PACK_FORMAT_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/harken_pack)
if(TARGET harken_engine_dev)
    install(FILES ${HARKEN_ENGINE_DEV_PUBLIC_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/harken)
endif()
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/api/abi/generated/harken_abi_manifest.g.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/HarkenEngine
    RENAME harken_abi_manifest.json)
install(EXPORT HarkenEngineTargets
    NAMESPACE Harken::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HarkenEngine)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/HarkenEngineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HarkenEngine)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineConfigVersion.cmake
    VERSION ${HARKEN_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/HarkenEngineConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HarkenEngine)

if(HARKEN_BUILD_RT_QUALIFICATION)
    set(_harken_rt_qualification_sources ${HARKEN_CORE_SOURCES} ${HARKEN_DEVELOPMENT_SOURCES})
    if(HARKEN_RT_PROBE_DEFINITIONS)
        list(APPEND _harken_rt_qualification_sources
            ${HARKEN_CORE_DIR}/core/src/RtProbe.cpp)
    endif()
    add_library(harken_core_rt_qualification STATIC
        ${_harken_rt_qualification_sources})
    target_link_libraries(harken_core_rt_qualification PRIVATE harken_buildinfo)
    target_include_directories(harken_core_rt_qualification
        PUBLIC ${HARKEN_CORE_DIR}/api/include ${HARKEN_DEVELOPMENT_DIR}/include
        PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
                ${HARKEN_DEVELOPMENT_PRIVATE_INCLUDES}
                ${HARKEN_CORE_DIR}/api/src)
    target_compile_definitions(harken_core_rt_qualification PUBLIC HARKEN_ENGINE_DEV=1)
    target_compile_definitions(harken_core_rt_qualification PRIVATE
        HARKEN_RT_QUALIFICATION
        ${HARKEN_RT_PROBE_DEFINITIONS})
    if(HARKEN_ENABLE_STAGE_TAPS)
        target_compile_definitions(harken_core_rt_qualification PRIVATE
            HARKEN_ENABLE_STAGE_TAPS)
    endif()
    if(HARKEN_PLATFORM_AUDIO_LIBS)
        target_link_libraries(harken_core_rt_qualification
            PUBLIC ${HARKEN_PLATFORM_AUDIO_LIBS})
    endif()
    if(WIN32)
        target_link_libraries(harken_core_rt_qualification PUBLIC advapi32 ws2_32 iphlpapi)
    endif()
    add_subdirectory(Tool/Qualification)
endif()

if(HARKEN_BUILD_TOOLS)

# Convenience regeneration target for the machine-readable public ABI
# manifest. The manifest is checked in for release provenance and does not make
# the ordinary build depend on Python.
harken_add_python_regen_target(
    harken_abi_manifest_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/api/abi/gen_abi_manifest.py"
    "Regenerating the public ABI manifest"
)

# Convenience regeneration target for the Windows export allowlist (same
# discipline as the generators above). The .def is checked in and consumed
# directly by the shared-library link, so the ordinary build stays Python-free;
# this target only rewrites it after Harken.h gains or loses an entry point.
harken_add_python_regen_target(
    harken_abi_exports_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/api/abi/gen_abi_exports.py"
    "Regenerating the Windows export allowlist from Harken.h"
)

# Remote-control channel (Tool/HostControl): a static tooling library the
# host embeds for its remote-serve verb. Never linked into a game; drives
# the engine only through the public C API.
add_subdirectory(Tool/HostControl)

# Development preview play surface (Source/Development): the host links it
# statically; the published HarkenEngineDev compiles the same sources itself.
add_subdirectory(Source/Development/preview)

# Host-side tooling executables (Tool/Host): the demo/serve host
# and the .pack compiler. The spec's host/ module lives here physically —
# never part of the shipping library, driving the engine as a game would.
add_subdirectory(Tool/Host)

endif()

if(HARKEN_BUILD_RUNTIME_UNIT_TESTS OR HARKEN_BUILD_RUNTIME_BENCHMARKS)
    add_subdirectory(Test)
endif()

if(TARGET harken_rt_probe_test)
    add_test(NAME harken_rt_probe_test COMMAND harken_rt_probe_test)
endif()

if(TARGET harken_runtime_ci_tests AND TARGET harken_host_selection_copy_test)
    add_dependencies(harken_runtime_ci_tests harken_host_selection_copy_test)
endif()
