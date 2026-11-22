#include <harken/Harken.h>
#include <harken/HarkenDevelopment.h>

#include "Engine.h"
#include "HarkenApiInternal.h"
#include "TraceRecorder.h"
#include "harken/core/LogRing.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/media/PackLogicImage.h"
#include "harken/render/EffectRegistry.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

static_assert(VN_COMPRESSOR_RUNTIME_BYPASSED == harken::kCompressorRuntimeBypassed);
static_assert(VN_COMPRESSOR_RUNTIME_BYPASS_TRANSITION == harken::kCompressorRuntimeBypassTransition);
static_assert(VN_COMPRESSOR_RUNTIME_PARAM_TRANSITION == harken::kCompressorRuntimeParamTransition);
static_assert(VN_COMPRESSOR_RUNTIME_EXTERNAL_AUTHORED == harken::kCompressorRuntimeExternalAuthored);
static_assert(VN_COMPRESSOR_RUNTIME_EXTERNAL_ACTIVE == harken::kCompressorRuntimeExternalActive);
static_assert(VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK == harken::kCompressorRuntimeResetThisBlock);
static_assert(VN_COMPRESSOR_RUNTIME_STALE == harken::kCompressorRuntimeStale);
static_assert(VN_COMPRESSOR_RUNTIME_RESET_BRIDGE == harken::kCompressorRuntimeResetBridge);

using harken::detail::FinishCaptureTrace;
using harken::detail::TraceQuery;

extern "C" {

VnResult Vn_CaptureEffectDynamicRoutes(VnEffectDynamicRouteInfo* out,
                                       uint32_t capacity,
                                       uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace =
        TraceQuery(engine, harken::kTraceCallCaptureEffectDynamicRoutes);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);

    VnResult result = VN_OK;
    if (capacity == 0 || out == nullptr || requiredCount == nullptr) {
        result = engine->dynamicRoutes.CaptureRoutes(
            out, capacity, requiredCount);
    } else {
        const std::uint32_t active =
            engine->dynamicRoutes.ActiveRouteCount();
        std::vector<VnEffectDynamicRouteInfo>& rows =
            engine->dynamicRouteCaptureScratch;
        const std::uint32_t scratchCapacity =
            static_cast<std::uint32_t>(rows.size());
        assert(active <= scratchCapacity &&
               "Dynamic Route capture scratch matches configured capacity");
        rows[0].structSize = out[0].structSize;
        std::uint32_t captured = 0;
        result = engine->dynamicRoutes.CaptureRoutes(
            rows.data(), scratchCapacity, &captured);
        *requiredCount = captured;
        if (result == VN_OK) {
            std::sort(
                rows.begin(), rows.begin() + captured,
                [engine](const VnEffectDynamicRouteInfo& lhs,
                         const VnEffectDynamicRouteInfo& rhs) {
                    const std::uint32_t lhsBus =
                        engine->buses.FindIndex(lhs.targetBusId);
                    const std::uint32_t rhsBus =
                        engine->buses.FindIndex(rhs.targetBusId);
                    if (lhsBus != rhsBus) {
                        return lhsBus < rhsBus;
                    }
                    if (lhs.effectSlot != rhs.effectSlot) {
                        return lhs.effectSlot < rhs.effectSlot;
                    }
                    return lhs.routeSlot < rhs.routeSlot;
                });
            const std::uint32_t written =
                (std::min)(capacity, captured);
            std::copy_n(rows.begin(), written, out);
            if (captured > capacity) {
                harken::NoteCaptureTruncated(
                    *engine,
                    harken::kCaptureSurfaceEffectDynamicRoutes,
                    captured);
            }
        }
    }
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

VnResult Vn_CaptureCompressorRuntimeInfo(
    VnCompressorRuntimeInfo* out, uint32_t capacity,
    uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace =
        TraceQuery(engine, harken::kTraceCallCaptureCompressorRuntimeInfo);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);
    const VnResult result = harken::CaptureCompressorRuntimeInfo(
        *engine, out, capacity, requiredCount);
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

VnResult Vn_TraceStart(const char* utf8Path) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    if (utf8Path == nullptr) {
        return VN_INVALID_ARG;
    }
    if (engine->trace != nullptr && harken::tracing::Active(engine->trace.get())) {
        return VN_IN_USE;
    }
    if ((engine->statefulCallSeen && !engine->traceOriginWorldProbeOnly) ||
        engine->meterProbeWithdrawalCount != 0) {
        // The journal must begin from a reconstructible origin. VTRC4 can
        // capture a current definitions-only world and installed private probe
        // image, but no other pre-start mutation or in-flight withdrawal.
        return VN_TRACE_TOO_LATE;
    }
    if (engine->traceSessionEpoch == UINT64_MAX) return VN_DEVICE_ERROR;
    ++engine->traceSessionEpoch;
    try {
        engine->trace.reset(); // drop a stopped or failed recorder before reuse
        harken::TraceRecorderOwner recorder(new harken::TraceRecorder(*engine));
        const VnResult result = recorder->Start(utf8Path);
        if (result != VN_OK) {
            // The engine is unaffected: no recorder installs, and the sticky
            // traceFailed flag keeps whatever a previous session left.
            return result;
        }
        // The accepted start owns the sticky failure flag from here on.
        engine->traceFailedSticky = false;
        engine->trace = std::move(recorder);
        return VN_OK;
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ApiEntry);
        return VN_BAD_TRACE_FILE;
    }
    });
}

VnResult Vn_TraceStop(void) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    // Stopping with no active trace is a benign no-op; a stop after a
    // mid-session failure just drops the inert recorder.
    if (engine->trace != nullptr) {
        if (engine->traceSessionEpoch != UINT64_MAX) ++engine->traceSessionEpoch;
        engine->CompleteVariableOperation();
        harken::tracing::Stop(engine->trace.get());
        engine->trace.reset();
    }
    return VN_OK;
    });
}

VnResult Vn_CaptureEffectCapabilities(VnEffectCapability* out,
                                      uint32_t capacity,
                                      uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace =
        TraceQuery(engine, harken::kTraceCallCaptureEffectCapabilities);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);
    const VnResult result = harken::CaptureEffectCapabilities(
        *engine, out, capacity, requiredCount);
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

VnResult Vn_CaptureEffectParameterCapabilities(
    VnEffectParameterCapability* out, uint32_t capacity,
    uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace = TraceQuery(
        engine, harken::kTraceCallCaptureEffectParameterCapabilities);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);
    const VnResult result =
        harken::CaptureEffectParameterCapabilities(
            *engine, out, capacity, requiredCount);
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

VnResult Vn_CaptureEffectDynamicCapabilities(
    VnEffectDynamicCapability* out, uint32_t capacity,
    uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace = TraceQuery(
        engine, harken::kTraceCallCaptureEffectDynamicCapabilities);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);
    const VnResult result =
        harken::CaptureEffectDynamicCapabilities(
            *engine, out, capacity, requiredCount);
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

VnResult Vn_CaptureEffectDynamicParameterCapabilities(
    VnEffectDynamicParameterCapability* out, uint32_t capacity,
    uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace = TraceQuery(
        engine,
        harken::kTraceCallCaptureEffectDynamicParameterCapabilities);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);
    const VnResult result =
        harken::CaptureEffectDynamicParameterCapabilities(
            *engine, out, capacity, requiredCount);
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

VnResult Vn_CapturePropertyTargets(VnPropertyTargetInfo* out,
                                   uint32_t capacity,
                                   uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    /* Query journal, the snapshot-capture precedent: the row vintage is
     * journaled explicitly (0 when the buffer is absent or unread —
     * capacity 0 never reads it), and the requiredCount pointer's
     * presence as a flag, so replay reconstructs the exact argument shape
     * including refused calls — the truncation counter and log record are
     * observable state replay must reproduce. */
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace =
        TraceQuery(engine, harken::kTraceCallCapturePropertyTargets);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);
    const VnResult result = harken::CapturePropertyTargets(
        *engine, out, capacity, requiredCount);
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

VnResult Vn_CapturePropertyBindings(VnPropertyBindingInfo* out,
                                    uint32_t capacity,
                                    uint32_t* requiredCount) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    const uint32_t rowVintage =
        (capacity > 0 && out != nullptr) ? out->structSize : 0u;
    harken::TraceCallScribe trace =
        TraceQuery(engine, harken::kTraceCallCapturePropertyBindings);
    trace.U32(capacity)
        .U32(rowVintage)
        .U32(requiredCount != nullptr ? 1u : 0u);
    const VnResult result = harken::CapturePropertyBindings(
        *engine, out, capacity, requiredCount);
    FinishCaptureTrace(trace, result, requiredCount);
    return result;
    });
}

} // extern "C"
