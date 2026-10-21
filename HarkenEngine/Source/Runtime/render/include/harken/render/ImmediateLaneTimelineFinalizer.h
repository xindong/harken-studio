#ifndef HARKEN_RENDER_IMMEDIATE_LANE_TIMELINE_FINALIZER_H
#define HARKEN_RENDER_IMMEDIATE_LANE_TIMELINE_FINALIZER_H

#include "harken/core/NativeTimelineQuantumAdmissionProof.h"
#include <cstdint>
#include <memory>

namespace harken {

class ImmediateActionLane;
class VoicePool;
class TimelineArmTable;
class FrontierTimelineFinalizer;
class ScheduledTransactionPool;
class DecisionTable;
class BlockArena;
struct Plan03ScratchDecisionState;

// Stages every voice in a due bucket before claiming or exposing any Start.
// Scratch is prepared by Attach or lane admission, outside the audio callback.
class ImmediateLaneTimelineFinalizer {
public:
    ImmediateLaneTimelineFinalizer(VoicePool& voices, TimelineArmTable& arms);
    ~ImmediateLaneTimelineFinalizer();
    bool Attach(ImmediateActionLane& lane, std::uint32_t blockFrames);
    void AttachFrontier(FrontierTimelineFinalizer& frontier,
                        const ScheduledTransactionPool& pool) noexcept;
    bool Capture(std::uint64_t quantum) noexcept;
    bool AdmitQuantum(std::uint32_t availableBytes, std::uint32_t blockFrames,
        const DecisionTable* decisions = nullptr,
        const Plan03ScratchDecisionState* currentSlices = nullptr,
        std::uint32_t currentSliceCount = 0) const noexcept;
    // Root issuance checks actual native arena storage, never a capacity DTO.
    // The Engine adapter fixes the Arena/Voice/Arm receiver to its own domain.
    bool ProveQuantumAdmission(const BlockArena& arena, std::uint32_t blockFrames,
        NativeTimelineQuantumAdmissionProof* admitted,
        const DecisionTable* decisions = nullptr,
        const Plan03ScratchDecisionState* currentSlices = nullptr,
        std::uint32_t currentSliceCount = 0) const noexcept;
    void Discard(bool arenaCapacityRefused = false) noexcept;
    void Apply(std::uint64_t quantum) noexcept;
    void Finish() noexcept;

private:
    struct Impl;
    VoicePool& voices_;
    TimelineArmTable& arms_;
    std::unique_ptr<Impl> impl_;
};

} // namespace harken

#endif // HARKEN_RENDER_IMMEDIATE_LANE_TIMELINE_FINALIZER_H
