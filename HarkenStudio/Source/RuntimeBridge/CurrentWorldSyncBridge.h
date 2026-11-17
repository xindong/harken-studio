#pragma once

#include "Utils/ObjectId.h"
#include "RuntimeBridge/ProjectSyncBridge.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace HarkenEditor::Bridge {

struct PreparedPreviewMedia;

struct PreviewEffectSource {
    harkenstudio::ObjectId id{};
    harkenstudio::ObjectId presetId{};
    std::uint32_t type = 0;
    std::vector<float> parameters;
    bool bypass = false;

    bool operator==(const PreviewEffectSource&) const = default;
};

using PreviewEffectDefinitions =
    std::map<harkenstudio::ObjectId, std::vector<PreviewEffectSource>>;

bool captureInstalledPreviewEffects(std::uint64_t generation,
    harkenstudio::ObjectId soundId, std::vector<PreviewEffectSource>& effects);
bool updateLivePreviewBusEffect(std::uint64_t runtimeEpoch, std::uint64_t authoringRevision,
    harkenstudio::ObjectId busId,
    harkenstudio::ObjectId definitionId, const PreviewEffectSource& effect);

enum class CurrentWorldSyncStatus : std::uint32_t {
    Ok = 0,
    RuntimeUnavailable,
    MediaUnavailable,
    InvalidProject,
    CompileFailed,
    InstallFailed,
    Pending,
    Superseded,
    Cancelled,
    IdentityExhausted,
};

enum class PendingCurrentWorldDisposition : std::uint32_t {
    Superseded,
    Cancelled,
};

struct CurrentWorldSyncToken {
    std::uint64_t projectSessionId = 0;
    std::uint64_t authoringRevision = 0;
    std::uint64_t runtimeEpoch = 0;
    std::uint64_t submissionSerial = 0;

    bool operator==(const CurrentWorldSyncToken& other) const noexcept {
        return projectSessionId == other.projectSessionId
            && authoringRevision == other.authoringRevision
            && runtimeEpoch == other.runtimeEpoch
            && submissionSerial == other.submissionSerial;
    }
};

struct CurrentWorldSyncResult {
    CurrentWorldSyncStatus status = CurrentWorldSyncStatus::InvalidProject;
    CurrentWorldSyncToken token;
    harkenstudio::ObjectId relatedStableId = {};
    std::uint64_t worldGeneration = 0;

    bool ok() const noexcept { return status == CurrentWorldSyncStatus::Ok; }
    bool pending() const noexcept {
        return status == CurrentWorldSyncStatus::Pending;
    }
};

struct CurrentWorldSoundPreparation {
    struct SourceState {
        std::uint32_t sourceId = 0;
        QString path;
        QString contentHash;
        bool assetAvailable = false;
        bool fileExists = false;
        qint64 size = 0;
        qint64 lastModifiedMs = 0;
    };
    struct Sound {
        SoundDesc binding;
        QString sourcePath;
        std::vector<float> samples;
        std::uint32_t channels = 0;
        std::uint32_t sampleRate = 0;
        std::uint64_t loopStartFrame = 0;
        std::uint64_t loopEndFrame = 0;
        std::shared_ptr<const PreparedPreviewMedia> media;
    };

    std::vector<Sound> sounds;
    std::vector<SourceState> sources;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> contentRevisions;
};

struct CurrentWorldArtifact {
    struct Bus {
        harkenstudio::ObjectId id = {};
        harkenstudio::ObjectId parentId = {};
        float faderDb = 0.0f;
        float outputBusVolumeDb = 0.0f;
        std::uint32_t faderAfterSlot = 0;
    };
    struct Effect {
        harkenstudio::ObjectId busId = {};
        harkenstudio::ObjectId effectId = {};
        harkenstudio::ObjectId definitionId = {};
        std::uint32_t slot = 0;
        std::uint32_t effectType = 0;
        std::uint32_t descriptorVintage = 0;
        std::uint32_t flags = 0;
        std::uint32_t parameterFirst = 0;
        std::uint32_t parameterCount = 0;
    };
    struct EffectParameter {
        std::uint32_t index = 0;
        float value = 0.0f;
    };
    struct Send {
        harkenstudio::ObjectId sourceId = {};
        harkenstudio::ObjectId targetId = {};
        float levelDb = 0.0f;
    };
    struct Profile {
        std::uint32_t vintage = 0;
        std::uint32_t sampleRate = 0;
        std::uint32_t mixLayout = 0;
        std::uint32_t deviceLayout = 0;
        std::uint64_t capabilityMask = 0;
    };

    std::vector<std::uint8_t> body;
    std::uint32_t bodyVintage = 0;
    std::vector<Bus> buses;
    std::vector<Send> sends;
    std::vector<Effect> effects;
    std::vector<EffectParameter> effectParameters;
    PreviewEffectDefinitions voiceEffects;
    std::uint64_t publishedBusFingerprint = 0;
    std::uint64_t compileTargetProfileFingerprint = 0;
    std::optional<Profile> profile;
    std::uint64_t ownerBaseRevision = 0;
    std::shared_ptr<const CurrentWorldSoundPreparation> soundPreparation;
};

struct CurrentWorldArtifactResult {
    CurrentWorldSyncStatus status = CurrentWorldSyncStatus::InvalidProject;
    harkenstudio::ObjectId relatedStableId = {};
    CurrentWorldArtifact artifact;

    bool ok() const noexcept { return status == CurrentWorldSyncStatus::Ok; }
};

using CurrentWorldArtifactBuilder = std::function<CurrentWorldArtifactResult()>;

CurrentWorldSyncResult submitCurrentWorld(CurrentWorldSyncToken token,
                                          const CurrentWorldArtifactBuilder& build);
CurrentWorldSyncResult submitCompiledCurrentWorld(CurrentWorldArtifactResult compiled,
                                                  CurrentWorldSyncToken token);
CurrentWorldSyncResult closeCurrentWorld(CurrentWorldSyncToken token,
                                         const CurrentWorldArtifactBuilder& buildEmpty);
std::optional<CurrentWorldSyncResult> discardPendingCurrentWorld(
    PendingCurrentWorldDisposition disposition =
        PendingCurrentWorldDisposition::Cancelled) noexcept;
std::optional<CurrentWorldSyncResult> takeCurrentWorldSyncCompletion() noexcept;

} // namespace HarkenEditor::Bridge
