#pragma once

#include "harken/render/Voice.h"
#include <atomic>
#include <vector>

namespace harken {

// Logic prepares each immutable membership snapshot before publication. The
// audio owner mutates only its states. Complete publishes the final read;
// only logic destroys backing, including cancelled and stale queued updates.
struct VoiceSendDefinition {
    std::uint32_t slot = 0;
    std::uint32_t targetBusIndex = 0;
    std::uint32_t targetBusGeneration = 0;
    float levelDb = 0;
    std::uint64_t incarnation = 0;
    bool operator==(const VoiceSendDefinition&) const noexcept = default;
};

struct VoiceSendRoute {
    bool active = false;
    std::uint32_t busIndex = 0;
    std::uint32_t busGeneration = 0;
    std::uint32_t slot = 0;
    std::uint64_t incarnation = 0;
    bool operator==(const VoiceSendRoute&) const noexcept = default;
};

struct VoiceSendSet {
    enum class Phase : std::uint8_t { Preparing, Queued, Active, Complete };
    std::atomic<Phase> phase{Phase::Preparing};
    VnVoiceHandle host = 0;
    std::vector<VoiceSendDefinition> definitions;
    std::vector<VoiceSendSlot> states;
    std::shared_ptr<const std::vector<VoiceSendRoute>> preparedRoutes;
    // No sentinel slot value: every uint32 label is valid.
    bool resetEdited = false;
    std::uint32_t editedSlot = 0;
};

} // namespace harken
