#pragma once

#include "harken/render/BuiltinEffects.h"

namespace harken {

// Only installed effects own residuals. Parameters remain in the existing
// target store; these values preserve history that reset cannot reconstruct.
struct BuiltinEffectDormantState {
    std::uint32_t type = kEffectTypeNone;
    std::uint32_t sampleRate = 0;
    bool hasRendered = false;
    union Values {
        float gain;
        double limiter;
        struct { double gain, wetGain, bridgeStart; } compressor;
        struct { float maxFeedback; std::uint32_t writeIndex, lineFrames; } delay;
        constexpr Values() : compressor{} {}
    } values;
};

// Tone alone needs section history. Logic prepares this extra record only
// for a Tone slot, not for every effect or every Bus definition.
struct ToneDormantState {
    struct Section {
        BiquadCoeffs coefficients;
        double signalHistory = 1;
        std::uint32_t tailFrames = 0;
        std::uint32_t zeroSigns = 0;
        bool everActive = false;
    } sections[3];
};

// Render-owner only, in the actual render floating-point environment, after
// proving all main/detector input is silent and no worker can touch the state.
// Failure leaves outputs unchanged. A zero tail estimate is not sufficient.
bool CaptureBuiltinEffectDormantState(std::uint32_t type, const void* state,
    std::uint32_t channels, BuiltinEffectDormantState& out,
    ToneDormantState* tone = nullptr) noexcept;

// Apply only to a fresh unpublished instance reconstructed from current target
// settings, with the same effect identity and format as the captured owner.
bool RestoreBuiltinEffectDormantState(void* state,
    const BuiltinEffectDormantState& saved,
    const ToneDormantState* tone = nullptr) noexcept;

} // namespace harken
