# harken_core — the engine runtime libraries and (desktop only) the host-side
# tooling. Split from the repository root so engine build edits live under
# HarkenEngine/** — the path the engine-facing PR gates watch. Reads the
# root-scope anchors  (HARKEN_RUNTIME_DIR, HARKEN_THIRD_PARTY_DIR), the shared
# build switches, and the root's Python 3 discovery.

# The runtime module sources, factored into a variable so the static library
# and the optional shared library for the C# binding build from one list.
set(HARKEN_RUNTIME_SOURCES
    ${HARKEN_RUNTIME_DIR}/remote/src/RemoteRuntime.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/DeviceBackend.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/RealDeviceInitStage.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MiniaudioImpl.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/TraceCodec.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/TraceRecorder.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorApi.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorCaptureHub.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorSessionRecover.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorSessionCompare.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorDerivedDispatch.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorWaveformHandler.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorScopeHandler.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorSpectrumHandler.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/MonitorMeterHandler.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/HarkenApi.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/PackInspection.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/ProjectGenerationTransaction.cpp
    ${HARKEN_RUNTIME_DIR}/api/src/PackVariablePublication.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/AllocGuard.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/StableRowStorage.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/MonitorRequest.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/MonitorWaveformReduce.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/MonitorScopeTrigger.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/MonitorSpectrumFft.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/MonitorMeterReduce.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CpuClock.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CurrentSelection.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CurrentSelectionMemory.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CurrentWorld.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CurrentWorldArtifact.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CurrentWorldCodec.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ActionFiringTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ActionSeekTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/StreamSeekHandoffTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ActionCooldownTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ActionDeferredRecordTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ArmGroupTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/AudioSliceLease.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/DecisionTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CompletionProgress.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ResamplerAdvance.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CompletionOwner.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CompletionMutationBinding.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CompositeCompletionOwnerPool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/KnownCompletion.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/PlayableProgramKey.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/PlayablePropertyCut.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/BlockArena.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ClockKernel.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/MarkerIndex.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/DeferredIntentPool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/EngineConfig.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/LogicCommitCoordinator.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/LogicReasonCode.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/OccurrenceStatePool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/PackComposition.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/PackCompanionNames.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/PlayableInstanceTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ContainerPlaybackTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/QueueRuntime.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/PrivateCatalogStage.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/VariableControlGrid.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/VariableControlRuntime.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ProgramGenerationRegistry.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/SamePositionResolver.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ScheduledTransactionPool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/ImmediateActionLane.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/TimelinePlanner.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/TimelineArmTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/TimelineExecutor.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/TransitionInstancePool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/VariableSliceTable.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/Utf8Path.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/VoiceLedger.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/VoiceManager.cpp
    ${HARKEN_RUNTIME_DIR}/dsp/src/MonoStereoMix.cpp
    ${HARKEN_RUNTIME_DIR}/dsp/src/MonoStereoMixAvx.cpp
    ${HARKEN_RUNTIME_DIR}/dsp/src/Resampler.cpp
    ${HARKEN_RUNTIME_DIR}/media/src/PcmRegistry.cpp
    ${HARKEN_RUNTIME_DIR}/media/src/ResolvedSlotTicket.cpp
    ${HARKEN_RUNTIME_DIR}/media/src/ResourceFacts.cpp
    ${HARKEN_RUNTIME_DIR}/media/src/PackLogicImage.cpp
    ${HARKEN_RUNTIME_DIR}/media/src/PackTable.cpp
    ${HARKEN_RUNTIME_DIR}/media/src/StreamManager.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/BindingTable.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/BusTargetStore.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/Curve.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/VariableService.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/EffectParamTargetStore.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/EmitterTable.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/MeterProbe.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/MeterSystem.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/ParamSystem.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/PropertyPublisher.cpp
    ${HARKEN_RUNTIME_DIR}/param/src/Spatializer.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/BuiltinEffects.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/BusTree.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/EffectPool.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/InsertEffect.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/MixerLoop.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/NullBackend.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/RenderWorkerPool.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/TopologyPlan.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/VoiceCascade.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/VoicePool.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/FrontierTimelineFinalizer.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/ImmediateLaneTimelineFinalizer.cpp
)
# Only this isolated kernel may require AVX; startup and fallback stay baseline.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND
   MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64")
    set_source_files_properties(${HARKEN_RUNTIME_DIR}/dsp/src/MonoStereoMixAvx.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX;/fp:strict")
endif()

# Private include dirs shared by the static and shared runtime libraries.
set(HARKEN_RUNTIME_PRIVATE_INCLUDES
    ${HARKEN_RUNTIME_DIR}/remote/include
    ${HARKEN_RUNTIME_DIR}/remote/src
    ${HARKEN_BI_GENERATED_DIR}
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/media/contract/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${HARKEN_THIRD_PARTY_DIR}/miniaudio
)

# The engine-free ResourceFacts value contract. A tooling-side compiler links
# this INTERFACE target to consume harken/media/ResourceFactsValue.h — the plain
# value an audio resource reduces to — without gaining the rest of the media
# include root or any harken_core dependency. The runtime targets reach the same
# header through their private media includes above; the concept stays owned by
# the runtime media layer.
add_library(harken_resource_facts_contract INTERFACE)
target_include_directories(harken_resource_facts_contract INTERFACE
    ${HARKEN_RUNTIME_DIR}/media/contract/include)

# The engine-free voice slot-layout contract. A tooling-side compiler links this
# INTERFACE target to consume harken/abi/VoiceSlotLayout.h — the published
# per-voice send-slot layout ceiling — without gaining the core include root or
# any harken_core dependency. The runtime targets reach the same header through
# their private core/contract include above; the ceiling stays owned by the core
# config that sizes the per-voice arrays.
add_library(harken_voice_slot_layout_contract INTERFACE)
target_include_directories(harken_voice_slot_layout_contract INTERFACE
    ${HARKEN_RUNTIME_DIR}/core/contract/include)

add_library(harken_bus_definition_fingerprint_contract INTERFACE)
target_include_directories(harken_bus_definition_fingerprint_contract INTERFACE
    ${HARKEN_RUNTIME_DIR}/api/include)

# Header-only Variable execution fingerprint (FNV-1a domain hasher). Tooling
# consumes it without linking harken_core.
add_library(harken_variable_execution_fingerprint_contract INTERFACE)
target_include_directories(harken_variable_execution_fingerprint_contract INTERFACE
    ${HARKEN_RUNTIME_DIR}/core/include)

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
        ${HARKEN_RUNTIME_DIR}/api/src/MiniaudioImpl.cpp
        PROPERTIES LANGUAGE OBJCXX)
endif()

option(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE
       "Enable the pre-init timeline capacity override in the test-linked static runtime"
       OFF)
if(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE AND NOT HARKEN_BUILD_RUNTIME_UNIT_TESTS)
    message(FATAL_ERROR
        "HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE requires HARKEN_BUILD_RUNTIME_UNIT_TESTS")
endif()

add_library(harken_core STATIC ${HARKEN_RUNTIME_SOURCES})
target_link_libraries(harken_core PRIVATE harken_buildinfo)
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
add_library(harken_remote_protocol INTERFACE)
target_include_directories(harken_remote_protocol INTERFACE
    ${HARKEN_RUNTIME_DIR}/remote/include)
target_include_directories(harken_core
    PUBLIC ${HARKEN_RUNTIME_DIR}/api/include
    PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
            ${HARKEN_RUNTIME_DIR}/api/src
)
if(HARKEN_PLATFORM_AUDIO_LIBS)
    target_link_libraries(harken_core PUBLIC ${HARKEN_PLATFORM_AUDIO_LIBS})
endif()
if(WIN32)
    target_link_libraries(harken_core PUBLIC advapi32 ws2_32 iphlpapi)
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
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_core PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()

if(HARKEN_BUILD_RUNTIME_UNIT_TESTS)
    add_library(harken_core_hub_absent STATIC ${HARKEN_RUNTIME_SOURCES})
    target_link_libraries(harken_core_hub_absent PRIVATE harken_buildinfo)
    target_include_directories(harken_core_hub_absent
        PUBLIC ${HARKEN_RUNTIME_DIR}/api/include
        PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
                ${HARKEN_RUNTIME_DIR}/api/src)
    target_compile_definitions(harken_core_hub_absent PRIVATE HARKEN_MONITOR_HUB_ABSENT)
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

# Optional shared library for the C# binding (HarkenIntegration): the native artifact the
# UPM package's Plugins/ ships and the managed ABI/zero-GC harness loads. Built
# from the SAME runtime sources as the static harken_core — no render code is
# duplicated or altered. The public API is extern "C", and on Windows the DLL
# exports exactly the entry points listed in the generated module-definition
# file below — the export surface is a reviewed allowlist, not whatever the
# linker happens to find. On macOS/Linux the symbols are exported by default.
# It is an independent additional target: existing targets and test results are
# unaffected whether it is ON or OFF. iOS ships the STATIC library linked as
# __Internal (the binding bakes that name), not this shared library. The static
# harken_core needs none of this: an archive has no export table, so its
# consumers resolve Vn_* at link time from the object symbols directly.
if(HARKEN_BUILD_SHARED)
    add_library(harken_core_shared SHARED ${HARKEN_RUNTIME_SOURCES})
    target_link_libraries(harken_core_shared PRIVATE harken_buildinfo)
    target_include_directories(harken_core_shared
        PUBLIC ${HARKEN_RUNTIME_DIR}/api/include
        PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
                ${HARKEN_RUNTIME_DIR}/api/src
    )
    if(HARKEN_ENABLE_STAGE_TAPS)
        target_compile_definitions(harken_core_shared PRIVATE
            HARKEN_ENABLE_STAGE_TAPS)
    endif()
    if(ANDROID)
        # Keep the Unity plugin self-contained and consistent with the Android
        # presets' ANDROID_STL=c++_static policy. A dynamic libc++ dependency
        # would require shipping another ABI-specific library beside HarkenCore.
        target_link_libraries(harken_core_shared PRIVATE c++_static c++abi)
        # Android 15 devices may use a 16 KiB memory page size. Keep every LOAD
        # segment compatible so Unity can package and load this plugin there.
        target_link_options(harken_core_shared PRIVATE
            "-Wl,-z,max-page-size=16384"
            "-Wl,-z,common-page-size=16384")
    endif()
    if(HARKEN_PLATFORM_AUDIO_LIBS)
        target_link_libraries(harken_core_shared PRIVATE ${HARKEN_PLATFORM_AUDIO_LIBS})
    endif()
    if(WIN32)
        target_link_libraries(harken_core_shared PRIVATE advapi32 ws2_32 iphlpapi)
        # Listing the .def among the target's sources makes CMake forward it to
        # the linker as /DEF:, so the allowlist is the same artifact reviewers
        # read and the linker obeys. Regenerate it from Harken.h with
        # HarkenEngine/Source/Tooling/Abi/gen_abi_exports.py after adding or
        # removing a public entry point; the abi_export_allowlist_check and
        # abi_export_surface_check tests fail on drift.
        target_sources(harken_core_shared PRIVATE
            ${HARKEN_RUNTIME_DIR}/api/abi/HarkenCore.def)
    endif()
    set_target_properties(harken_core_shared PROPERTIES
        # HarkenCore.dll / libHarkenCore.{so,dylib} matches the native binding.
        OUTPUT_NAME HarkenCore
        # Keep the Windows import library from clashing with the static
        # HarkenCore.lib in the archive output directory.
        ARCHIVE_OUTPUT_NAME HarkenCoreImport)
endif()

if(HARKEN_BUILD_RT_QUALIFICATION)
    set(_harken_rt_qualification_sources ${HARKEN_RUNTIME_SOURCES})
    if(HARKEN_RT_PROBE_DEFINITIONS)
        list(APPEND _harken_rt_qualification_sources
            ${HARKEN_RUNTIME_DIR}/core/src/RtProbe.cpp)
    endif()
    add_library(harken_core_rt_qualification STATIC
        ${_harken_rt_qualification_sources})
    target_link_libraries(harken_core_rt_qualification PRIVATE harken_buildinfo)
    target_include_directories(harken_core_rt_qualification
        PUBLIC ${HARKEN_RUNTIME_DIR}/api/include
        PRIVATE ${HARKEN_RUNTIME_PRIVATE_INCLUDES}
                ${HARKEN_RUNTIME_DIR}/api/src)
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

function(_harken_add_python_regen_target target_name script_path target_comment)
    if(Python3_Interpreter_FOUND)
        add_custom_target(${target_name}
            COMMAND "${Python3_EXECUTABLE}" "${script_path}"
            COMMENT "${target_comment}"
            VERBATIM
        )
    else()
        add_custom_target(${target_name}
            COMMAND "${CMAKE_COMMAND}" -E echo
                    "ERROR: ${target_name} requires a Python 3 interpreter; install Python 3 and reconfigure."
            COMMAND "${CMAKE_COMMAND}" -E false
            COMMENT "${target_comment}"
            VERBATIM
        )
    endif()
endfunction()

# Convenience regeneration target for the C# binding (mirrors harken_wire_regen).
# Never part of ALL: the generated products are checked in and the build stays
# Python-free.
_harken_add_python_regen_target(
    harken_binding_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Tooling/Binding/gen_binding.py"
    "Regenerating the C# binding from harken_wire_table.json"
)

# Convenience regeneration target for the machine-readable public ABI
# manifest. The manifest is checked in for release provenance and does not make
# the ordinary build depend on Python.
_harken_add_python_regen_target(
    harken_abi_manifest_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Tooling/Abi/gen_abi_manifest.py"
    "Regenerating the public ABI manifest"
)

# Convenience regeneration target for the built-in effect registry (same
# discipline as harken_binding_regen). The generated header is checked in under
# the render include tree — already on every consumer's include path — so the
# ordinary build needs no Python; this target only rewrites it after the source
# table changes.
_harken_add_python_regen_target(
    harken_effect_registry_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Tooling/Effects/gen_effect_registry.py"
    "Regenerating the built-in effect registry from effect_registry.json"
)

# Convenience regeneration target for the Windows export allowlist (same
# discipline as the generators above). The .def is checked in and consumed
# directly by the shared-library link, so the ordinary build stays Python-free;
# this target only rewrites it after Harken.h gains or loses an entry point.
_harken_add_python_regen_target(
    harken_abi_exports_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Tooling/Abi/gen_abi_exports.py"
    "Regenerating the Windows export allowlist from Harken.h"
)

# Convenience regeneration target for the reference-slot vocabulary. Emits the
# compile-side and runtime-side slot definitions from one table, so the two
# numberings cannot be edited apart. Products are checked in; the build stays
# Python-free.
_harken_add_python_regen_target(
    harken_reference_slots_regen
    "${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/ReferenceSlots/slots/gen_reference_slots.py"
    "Regenerating the reference-slot vocabulary from harken_reference_slot_table.json"
)

_harken_add_python_regen_target(
    harken_playable_properties_regen
    "${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/PlayableProperties/properties/gen_playable_properties.py"
    "Regenerating PlayablePropertyRegistryV1 from harken_playable_property_table.json"
)

_harken_add_python_regen_target(
    harken_variable_catalog_regen
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Tooling/VariableCatalog/gen_variable_catalog.py"
    "Regenerating Native/Unity Variable catalog constants from variable_catalog.json"
)

# Remote-control channel (Tool/Remote): a static tooling library the
# host embeds for its remote-serve verb. Never linked into a game; drives
# the engine only through the public C API.
add_subdirectory(Tool/Remote)

# Development preview play surface (Source/Tooling): the C entry face a
# Studio-side host uses to hear one registered program. A tooling library because
# playing a piece directly is not a gameplay capability -- production content
# starts through the Action program surface -- and a separate target because which
# targets link it is what keeps it out of shipped products. harken_core exports
# nothing for it and knows nothing about it.
add_subdirectory(Source/Tooling/Preview)

# v5->v6 migration dry-run inventory (Source/Tooling): a read-only pass that
# reports how a v5 project's items map to the v6 object model and which items it
# refuses to migrate automatically. A tooling library for the same reason the
# schema front-end is one — the work belongs to migration/compilation, not the
# runtime — and Qt-free despite reading a Qt-authored format.

# Host-side tooling executables (Tool/Host): the demo/serve host
# and the .pack compiler. The spec's host/ module lives here physically —
# never part of the shipping library, driving the engine as a game would.
add_subdirectory(Tool/Host)

endif()
