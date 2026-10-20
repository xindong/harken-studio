#ifndef HARKEN_CORE_TIMELINE_PROGRAM_H
#define HARKEN_CORE_TIMELINE_PROGRAM_H

#include <harken/ObjectId.h>
#include "harken/core/ProgramGenerationRegistry.h"
#include "harken/core/AuthoredStopOperationIdentity.h"
#include "harken/core/MonitorRequest.h"
#include "harken/core/MonitorIdentity.h"
#include "harken/core/PcmView.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/PlaybackPath.h"
#include "harken/core/WeightSegment.h"
#include "harken/core/PackPlaybackView.h"
#include "harken/core/VariableWeightControl.h"
#include "harken/param/Curve.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace harken {

enum class TimelineEventKind : std::uint8_t {
    SetStartCursor = 1,
    Start = 2,
    Envelope = 3,
    Stop = 4,
    VariableWeight = 5,
    ControllerActivation = 6,
};

enum class TimelineEnvelopeDirection : std::uint8_t {
    FadeIn = 1,
    FadeOut = 2,
};

inline constexpr std::uint16_t kTimelineEventTransientStartArm = 1u << 0;
inline constexpr std::uint16_t kTimelineEventTransientControlArm = 1u << 1;
inline constexpr std::uint16_t kTimelineEventAuthoredRootStop = 1u << 2;

struct TimelineSemanticTarget {
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t clipGuidHigh = 0;
    std::uint64_t clipGuidLow = 0;
    std::uint64_t subjectGuidHigh = 0;
    std::uint64_t subjectGuidLow = 0;
    VnObjectId trackId{};
    VnObjectId clipId{};
    VnObjectId subjectId{};
    std::uint32_t loopIteration = 0;
    std::uint32_t occurrenceOrdinal = 0;
    bool operator==(const TimelineSemanticTarget&) const noexcept = default;
};

struct TimelineEventPayload {
    TimelineEventKind kind = TimelineEventKind::Start;
    TimelineEnvelopeDirection envelopeDirection =
        TimelineEnvelopeDirection::FadeIn;
    std::uint16_t reserved = 0;
    std::uint32_t resolvedOccurrenceIndex = 0;
    std::uint32_t envelopeIndex = 0;
    TimelineSemanticTarget target{};
    std::uint64_t sourceStartFrame = 0;
    std::uint64_t envelopeStartFrame = 0;
    std::uint64_t envelopeEndFrame = 0;
    const CurveLUT* envelopeCurve = nullptr;
    bool authoredFadeOutCandidate = false;
    WeightSegmentV1 variableWeight{};
    VariableWeightControlIdentity variableWeightOwner{};
    AuthoredStopOperationIdentity authoredStopOperation{};
    MonitorControlCause controlCause{};
};

struct LogicalOnlyOccurrence {
    std::uint32_t resolvedOccurrenceIndex = 0;
    std::uint64_t logicalCompleteFrame = 0;
};

struct AuthoredStopFenceDescriptor;

enum class TimelineProgramRole : std::uint8_t {
    Voice = 0,
    AuthoredStopFence = 1,
    ControllerActivation = 2,
};

class TimelineProgramReadFence {
public:
    void RecordRead(std::uint64_t audioBlock) noexcept {
        lastReadBlock_.store(audioBlock, std::memory_order_release);
    }
    bool ReadCompleted(std::uint64_t completedBlocks) const noexcept {
        const auto block = lastReadBlock_.load(std::memory_order_acquire);
        return block == UINT64_MAX || completedBlocks > block;
    }
    void ResetAfterSafeRetirement() noexcept {
        lastReadBlock_.store(UINT64_MAX, std::memory_order_relaxed);
    }
private:
    std::atomic<std::uint64_t> lastReadBlock_{UINT64_MAX};
};

struct TimelineProgramView {
    ProgramGeneration generation = 0;
    const TimelineEventPayload* payloads = nullptr;
    std::uint32_t payloadCount = 0;
    TimelineProgramRole role = TimelineProgramRole::Voice;
    const AuthoredStopFenceDescriptor* authoredStopFence = nullptr;
    TimelineProgramReadFence* readFence = nullptr;
};

struct OwningTimelineProgram {
    OwningTimelineProgram() = default;
    OwningTimelineProgram(const OwningTimelineProgram&) = delete;
    OwningTimelineProgram& operator=(const OwningTimelineProgram&) = delete;
    OwningTimelineProgram(OwningTimelineProgram&& other) noexcept
        : payloads(std::move(other.payloads)) {
        Seal(other.view.generation);
    }
    OwningTimelineProgram& operator=(OwningTimelineProgram&& other) noexcept {
        payloads = std::move(other.payloads);
        Seal(other.view.generation);
        return *this;
    }

    std::vector<TimelineEventPayload> payloads;
    TimelineProgramView view{};

    void Seal(ProgramGeneration generation) noexcept {
        view.generation = generation;
        view.payloads = payloads.data();
        view.payloadCount = static_cast<std::uint32_t>(payloads.size());
    }
};

struct TimelineProgramSet {
    std::vector<OwningTimelineProgram> programs;
};

struct TimelineTransactionProgramRef {
    ProgramGeneration programGeneration = 0;
    const TimelineProgramView* program = nullptr;
};

inline constexpr std::uint32_t kTimelineMaxEventsPerVoiceQuantum = 8;
inline constexpr std::uint32_t kTimelineMaxLaneEventsPerVoiceQuantum = 8;
inline constexpr std::uint32_t kTimelineMaxCombinedEventsPerVoiceQuantum =
    kTimelineMaxEventsPerVoiceQuantum + kTimelineMaxLaneEventsPerVoiceQuantum;
inline constexpr std::uint32_t kTimelineMaxVoiceSends = 8;
inline constexpr std::uint32_t kTimelineMaxVoiceEffects = 8;
inline constexpr std::uint32_t kTimelineMaxEffectDescriptorBytes = 152;

struct TimelineVoiceSendInstall {
    VnObjectId targetBusId{};
    float levelDb = 0.0f;
};

struct TimelineVoiceEffectInstall {
    std::uint32_t slot = 0;
    std::uint32_t type = 0;
    std::uint32_t descriptorSize = 0;
    alignas(std::uint64_t)
        std::uint8_t descriptor[kTimelineMaxEffectDescriptorBytes]{};
};

struct TimelineVoiceBindingInstall {
    VnObjectId objectId = {};
    VnBindingId bindingId{};
    Curve curve;
    std::uint32_t revision = 1;
    std::uint32_t transitionMs = 0;
    std::uint32_t targetId = 0;
    VnVariableId variableId{};
};

struct TimelineVoiceInstallDesc {
    MonitorContentIdentity monitorContent{};
    MonitorOccurrenceIdentity monitorOccurrence{};
    PCMView capturedMedia{};
    bool streamed = false;
    std::uint32_t streamFileIndex = 0;
    std::uint32_t streamSampleRate = 0;
    std::uint32_t streamChannelCount = 0;
    std::uint32_t streamFlags = 0;
    std::uint64_t streamFrameCount = 0;
    std::uint64_t streamContentRevision = 0;
    std::uint64_t streamLoopStartFrame = 0;
    std::uint64_t streamLoopEndFrame = 0;
    std::uint32_t bindingCount = 0;
    TimelineVoiceBindingInstall bindingStorage[kMaxVoiceBindingInstanceSlots]{};
    PlaybackPath playbackPath{};
    VnObjectId soundId{};
    VnObjectId outputBusId{};
    std::uint64_t sourceStartFrame = 0;
    std::uint64_t authoredDurationFrames = UINT64_MAX;
    float volumeDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    float pitchRatio = 1.0f;
    float pan = 0.0f;
    // Runtime Width in [0, 1], already converted from authoring percent.
    float spread = 0.0f;
    float spatializationMix = 1.0f;
    float attenuationDistanceScaling = 1.0f;
    VnObjectId attenuationId{};
    std::uint32_t sendCount = 0;
    TimelineVoiceSendInstall sends[kTimelineMaxVoiceSends]{};
    std::uint32_t effectCount = 0;
    TimelineVoiceEffectInstall effects[kTimelineMaxVoiceEffects]{};
    std::uint32_t loopCount = 1;
    bool spatialized = false;
    bool attenuationResolved = false;
    TimelineSemanticTarget target{};
};

} // namespace harken

#endif // HARKEN_CORE_TIMELINE_PROGRAM_H
