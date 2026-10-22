#ifndef HARKEN_CORE_TIMELINE_VOICE_INSTALL_INPUT_H
#define HARKEN_CORE_TIMELINE_VOICE_INSTALL_INPUT_H

#include "harken/core/TimelineProgram.h"

#include <span>

namespace harken {

struct TimelineVoiceInstallFacts {
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
    VnObjectId soundId{};
    VnObjectId outputBusId{};
    std::uint64_t sourceStartFrame = 0;
    std::uint64_t authoredDurationFrames = UINT64_MAX;
    float volumeDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    float pitchRatio = 1.0f;
    float pan = 0.0f;
    float spread = 0.0f;
    float spatializationMix = 1.0f;
    float attenuationDistanceScaling = 1.0f;
    VnObjectId attenuationId{};
    std::uint32_t loopCount = 1;
    bool spatialized = false;
    bool attenuationResolved = false;
    TimelineSemanticTarget target{};
};

// Synchronous borrowed input. Its owner keeps the facts, columns and media alive.
struct TimelineVoiceInstallInput {
    const TimelineVoiceInstallFacts* facts = nullptr;
    std::span<const TimelineVoiceBindingInstall> bindings{};
    std::span<const PlaybackPathRow> pathRows{};
    std::span<const TimelineVoiceSendInstall> sends{};
    std::span<const TimelineVoiceEffectInstall> effects{};
};

} // namespace harken

#endif // HARKEN_CORE_TIMELINE_VOICE_INSTALL_INPUT_H
