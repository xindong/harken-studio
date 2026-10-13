// Scoped parameter-target metadata: the
// target locator triple and the single point of truth for every
// addressable target's documented facts — unit, combine domain, neutral
// value, legal range, transition support, and the wire opcode the resolved
// value commits through. The table is constexpr and asserted complete
// against the target-id constants at compile time; a unit test walks every
// row against the documented values. Successor include of ParamTargets.h:
// that header keeps the per-voice id constants and the combine-domain
// vocabulary; this one owns the scoped view, the bus rows, and the
// domain/clamp helpers, which read their answers from the table rows.
#ifndef HARKEN_PARAM_PARAM_TARGET_TABLE_H
#define HARKEN_PARAM_PARAM_TARGET_TABLE_H

#include "harken/core/CommandQueue.h"
#include "harken/param/ParamTargets.h"
#include "harken/param/Curve.h"
#include "harken/param/PropertyPublisher.h"

#include <cstdint>

namespace harken {

// Bus-scope target ids. Append-only: BusVolume (id 0) then BusOutputVolume
// (id 1). Both are dB, add-in-dB, neutral 0; Output Volume flushes with
// outputOnly=1 so Send does not follow.
inline constexpr std::uint32_t kBusTargetBusVolume = 0;
inline constexpr std::uint32_t kBusTargetBusOutputVolume = 1;
inline constexpr std::uint32_t kBusTargetCount = 2;

inline constexpr CurveDomain BindingCurveDomain(std::uint32_t scopeKind,
                                                std::uint32_t targetId) {
    return (scopeKind == kTargetScopeVoice && targetId == kTargetVoiceVolume) ||
           (scopeKind == kTargetScopeBus &&
            (targetId == kBusTargetBusVolume ||
             targetId == kBusTargetBusOutputVolume))
        ? CurveDomain::MirroredAmplitudeDb : CurveDomain::LinearValue;
}

// One parameter destination: (scope kind, object id, target id). For
// VOICE-scope rows the object is implied by the owning voice instance; for
// BUS-scope rows objectId is the public VnBusId. Scope kinds and per-scope
// target tables are append-only; an unknown scope resolves to no row and
// is refused by callers.
struct TargetLocator {
    std::uint32_t scopeKind = 0;
    VnObjectId objectId{};
    std::uint32_t targetId = 0;
};

// One row of documented target facts. neutral is the domain's
// no-contribution identity for the target (an untouched target reduces to
// it); clamped rows bound the RESOLVED value into [clampMin, clampMax]
// (contributions themselves stay unclamped — ClampResolvedTarget below
// enforces exactly these columns); supportsTransition marks the targets
// whose wire command carries a ramp.
struct TargetMeta {
    std::uint32_t scopeKind;
    std::uint32_t targetId;
    const char* unit;
    CombineDomain domain;
    float neutral;
    bool clamped;
    float clampMin;
    float clampMax;
    bool supportsTransition;
    std::uint16_t wireOpcode;
};

inline constexpr TargetMeta kTargetMetaTable[] = {
    {kTargetScopeVoice, kTargetVoiceVolume, "dB", CombineDomain::AddDb, 0.0f,
     false, 0.0f, 0.0f, true, kOpcodeSetVoiceVolume},
    {kTargetScopeVoice, kTargetVoicePitch, "cents", CombineDomain::AddCents,
     0.0f, false, 0.0f, 0.0f, true, kOpcodeSetVoicePitch},
    {kTargetScopeVoice, kTargetVoiceLowpassCutoff, "Hz", CombineDomain::MinHz,
     kLowpassOpenHz, false, 0.0f, 0.0f, true, kOpcodeSetVoiceFilter},
    {kTargetScopeVoice, kTargetVoiceHighpassCutoff, "Hz", CombineDomain::MaxHz,
     kHighpassClosedHz, false, 0.0f, 0.0f, true, kOpcodeSetVoiceFilter},
    {kTargetScopeVoice, kTargetVoicePan, "position", CombineDomain::Add, 0.0f,
     true, -1.0f, 1.0f, false, kOpcodeSetVoicePan},
    {kTargetScopeVoice, kTargetVoiceWidth, "width", CombineDomain::Max, 0.0f,
     true, 0.0f, 1.0f, false, kOpcodeSetVoiceWidth},
    {kTargetScopeVoice, kTargetVoiceLowShelfGainDb, "dB", CombineDomain::AddDb,
     0.0f, true, -48.0f, 48.0f, true, kOpcodeSetVoiceLowShelf},
    {kTargetScopeVoice, kTargetVoiceLowpassQ, "Q", CombineDomain::AddLogQ,
     kToneDefaultQ, true, 0.1f, 10.0f, true, kOpcodeSetVoiceFilter},
    {kTargetScopeVoice, kTargetVoiceHighpassQ, "Q", CombineDomain::AddLogQ,
     kToneDefaultQ, true, 0.1f, 10.0f, true, kOpcodeSetVoiceFilter},
    {kTargetScopeVoice, kTargetVoiceLowShelfFreqHz, "Hz",
     CombineDomain::AddLogHz, kToneLowShelfDefaultHz, true, 1.0f,
     kLowpassOpenHz, true, kOpcodeSetVoiceLowShelf},
    {kTargetScopeVoice, kTargetVoiceLowShelfQ, "Q", CombineDomain::AddLogQ,
     kToneDefaultQ, true, 0.1f, 10.0f, true, kOpcodeSetVoiceLowShelf},
    {kTargetScopeVoice, kTargetVoiceHighShelfGainDb, "dB",
     CombineDomain::AddDb, 0.0f, true, -48.0f, 48.0f, true,
     kOpcodeSetVoiceLowShelf},
    {kTargetScopeVoice, kTargetVoiceHighShelfFreqHz, "Hz",
     CombineDomain::AddLogHz, kToneHighShelfDefaultHz, true, 1.0f,
     kLowpassOpenHz, true, kOpcodeSetVoiceLowShelf},
    {kTargetScopeVoice, kTargetVoiceHighShelfQ, "Q",
     CombineDomain::AddLogQ, kToneDefaultQ, true, 0.1f, 10.0f, true,
     kOpcodeSetVoiceLowShelf},
    {kTargetScopeBus, kBusTargetBusVolume, "dB", CombineDomain::AddDb, 0.0f,
     false, 0.0f, 0.0f, true, kOpcodeSetBusVolume},
    {kTargetScopeBus, kBusTargetBusOutputVolume, "dB", CombineDomain::AddDb,
     0.0f, false, 0.0f, 0.0f, true, kOpcodeSetBusVolume},
};

inline constexpr std::uint32_t kTargetMetaCount =
    sizeof(kTargetMetaTable) / sizeof(kTargetMetaTable[0]);

// Compile-time completeness: every voice target and every bus target has
// exactly one row, in id order (voice rows first), with transition flags
// matching the command payloads. A new target id without a
// row, or a row drifting out of order, fails the build here.
constexpr bool TargetMetaTableIsComplete() {
    if (kTargetMetaCount != kBuiltinTargetCount + kBusTargetCount) {
        return false;
    }
    for (std::uint32_t i = 0; i < kBuiltinTargetCount; ++i) {
        if (kTargetMetaTable[i].scopeKind != kTargetScopeVoice ||
            kTargetMetaTable[i].targetId != i) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < kBusTargetCount; ++i) {
        const TargetMeta& row = kTargetMetaTable[kBuiltinTargetCount + i];
        if (row.scopeKind != kTargetScopeBus || row.targetId != i) {
            return false;
        }
    }
    std::uint32_t transitions = 0;
    for (std::uint32_t i = 0; i < kTargetMetaCount; ++i) {
        if (kTargetMetaTable[i].supportsTransition) {
            ++transitions;
        }
    }
    return transitions == 14 && kTargetMetaTable[kTargetVoiceVolume].supportsTransition &&
           kTargetMetaTable[kTargetVoicePitch].supportsTransition &&
           kTargetMetaTable[kBuiltinTargetCount + kBusTargetBusVolume]
               .supportsTransition &&
           kTargetMetaTable[kBuiltinTargetCount + kBusTargetBusOutputVolume]
               .supportsTransition;
}

static_assert(TargetMetaTableIsComplete(),
              "target metadata table must cover every scoped target exactly "
              "once, in id order, with valid transition flags");

// Row lookup by scoped target id; nullptr when the pair addresses no known
// target (including scope kinds that do not exist yet — refused, never
// guessed).
inline constexpr const TargetMeta* FindTargetMeta(std::uint32_t scopeKind,
                                                  std::uint32_t targetId) {
    for (const TargetMeta& row : kTargetMetaTable) {
        if (row.scopeKind == scopeKind && row.targetId == targetId) {
            return &row;
        }
    }
    return nullptr;
}

// Combine domain of one built-in per-voice target, read from the table
// (voice rows sit id-ordered at the table head, so the lookup is a direct
// index — no scan on the reduction path). Unknown ids keep the historical
// AddDb fallback.
inline CombineDomain DomainOfTarget(std::uint32_t targetId) {
    if (targetId < kBuiltinTargetCount) {
        return kTargetMetaTable[targetId].domain;
    }
    return CombineDomain::AddDb;
}

// Clamps a RESOLVED per-voice value into the target's table-declared span
// (contributions themselves stay unclamped, so a later opposing
// contribution resolves normally). Rows without a clamp pass through
// unchanged, as do unknown ids.
inline float ClampResolvedTarget(std::uint32_t targetId, float value) {
    if (targetId < kBuiltinTargetCount) {
        const TargetMeta& row = kTargetMetaTable[targetId];
        if (row.clamped) {
            return (value < row.clampMin)   ? row.clampMin
                   : (value > row.clampMax) ? row.clampMax
                                            : value;
        }
    }
    return value;
}

// Folds one more contribution into an accumulated value in the given
// domain. The add domains are commutative sums; the min/max domains take
// the more-restrictive (filters) or larger (width) value — commutative and
// order-free by construction, which is what lets multiple bindings on one
// target accumulate without arbitration.
inline constexpr float CombineValue(CombineDomain domain, float accumulated,
                                    float contribution) {
    switch (domain) {
    case CombineDomain::AddDb:
    case CombineDomain::AddCents:
    case CombineDomain::Add:
        return accumulated + contribution;
    case CombineDomain::AddLogHz:
    case CombineDomain::AddLogQ:
        return accumulated + contribution;
    case CombineDomain::MinHz:
        return contribution < accumulated ? contribution : accumulated;
    case CombineDomain::MaxHz:
    case CombineDomain::Max:
        return contribution > accumulated ? contribution : accumulated;
    }
    return accumulated;
}

} // namespace harken

#endif // HARKEN_PARAM_PARAM_TARGET_TABLE_H
