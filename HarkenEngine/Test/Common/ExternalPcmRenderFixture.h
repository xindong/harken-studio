#pragma once

#include "Common/ExternalPcmFixture.h"
#include "Engine.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace harkentest {

// Numerical and worker tests enter the real renderer without public Voice ownership.
class ExternalPcmRenderFixture {
public:
    ExternalPcmRenderFixture(harken::Engine& engine, const std::string& path) : engine_(engine) {
        std::ifstream input(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
        if (!input) return;
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        range_ = PreparedExternalPcmFixtureRange(bytes,
            [path](auto) { return harken::OpenStreamByteSource(path.c_str()); });
        if (range_) result_ = engine_.streams.ResolvePackSource(range_, &facts_);
    }

    VnResult Result() const { return result_; }

    VnResult Start(float volumeDb, std::uint32_t loops, VnVoiceHandle* render) {
        if (result_ != VN_OK) return result_;
        if (render == nullptr) return VN_INVALID_ARG;
        *render = 0;
        std::uint32_t slot = 0;
        if (!engine_.streams.PeekFreeSlot(&slot) ||
            !engine_.streams.PrepareSlotWorkingSet(slot, facts_.channelCount)) return VN_NO_STREAM;
        const bool loopable = (facts_.flags & harken::kStreamFlagLoopable) != 0;
        const auto loopStart = loopable ? facts_.loopStartFrame : 0;
        const auto loopEnd = loopable ? facts_.loopEndFrame : facts_.frameCount;
        std::uint64_t token = 0;
        const auto active = engine_.streams.ActivateSlot(slot, facts_.fileIndex, 0, loopStart, loopEnd, loops, &token);
        if (active != VN_OK) return active;
        auto* ring = engine_.streams.RingOf(slot);
        const auto prebuffer = std::min<std::uint64_t>(
            (std::uint64_t(engine_.config.prebufferMs) * facts_.sampleRate + 999u) / 1000u,
            ring->FrameCapacity(facts_.channelCount));
        const auto played = engine_.voices.PlayStream(ring, facts_.channelCount, facts_.sampleRate,
            static_cast<std::uint32_t>(prebuffer), volumeDb, 1, 0, false, 0, 0, 0,
            harken::ResolvePanGains(engine_.config.mixChannelLayout, 0), 0, facts_.frameCount,
            loopStart, loopEnd, loops, harken::ResampleProfile::LegacyCompatibility, render,
            engine_.buses.GenerationAt(0));
        if (played != VN_OK) { engine_.streams.RetirePreparedSlot(ring, token); return played; }
        engine_.voices.BindStreamRingToken(*render, ring, token);
        engine_.voices.ApplyPlay(*render, *render, 0, engine_.mixer.BlocksRendered());
        return VN_OK;
    }
private:
    harken::Engine& engine_;
    std::shared_ptr<const harken::PackMediaRange> range_;
    harken::StreamManager::StreamFacts facts_{};
    VnResult result_ = VN_BAD_STREAM_FILE;
};

} // namespace harkentest
