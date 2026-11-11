#pragma once

#include "harken/render/InsertEffect.h"
#include "harken/core/ChannelLayout.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/PreviewEffectChain.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace harken {

static_assert(PreviewEffectChainEntry{}.effectType == kEffectTypeNone);

// One lazily allocated, stable row per edited physical Voice slot. Logic owns
// Free/Preparing, the command consumer owns Queued/Retiring, and the cascade
// borrows Active DSP fields until rendezvous. Complete is a release handoff:
// no audio reader retains scratch or accesses fields that logic will reset.
struct EffectChainUpdate {
    enum class Phase : std::uint8_t { Free, Preparing, Queued, Active, Retiring, Complete };
    std::atomic<Phase> phase{Phase::Free};
    std::uint64_t acceptedHandle = 0; // Logic only; survives descriptor reuse.
    std::uint64_t acceptedRevision = 0;
    std::uint64_t host = 0;
    std::uint64_t baseRevision = 0;
    std::uint64_t targetRevision = 0;
    std::array<PreviewEffectChainEntry, kMaxFxSlotsPerVoice> targets{};
    std::array<EffectInstanceRef, kMaxFxSlotsPerVoice> targetRefs{};
    std::array<EffectInstanceRef, kMaxFxSlotsPerVoice> oldRefs{};
    std::array<InsertEffect, kMaxFxSlotsPerVoice> oldEffects{};
    std::array<float, kMaxFxSlotsPerVoice> oldBypass{};
    std::array<std::array<float, 2>, kMaxFxSlotsPerVoice> preparedParams{};
    std::array<std::uint8_t, kMaxFxSlotsPerVoice> parameterMask{};
    std::uint32_t bypassMask = 0;
    std::uint32_t changedMask = 0;
    std::uint32_t ownedTargets = 0;
    std::uint32_t ownedOld = 0;
    std::uint32_t channels = 0;
    std::uint32_t maxFrames = 0;
    std::uint32_t totalFrames = 0;
    std::uint32_t done = 0;
    std::vector<float> scratch;
    std::array<float*, kMaxChannels> dry{};
    std::array<float*, kMaxChannels> alternate{};
};

} // namespace harken
