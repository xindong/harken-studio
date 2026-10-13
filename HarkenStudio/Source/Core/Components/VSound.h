#pragma once

#include "../Component.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

struct SoundLoopRegion
{
    uint64_t startFrame = 0;
    uint64_t endFrame = 0;

    bool operator==(const SoundLoopRegion&) const = default;
};

enum class SoundLoopProvenance
{
    Authored,
    File
};

struct SoundSourceDef
{
    QUuid entryId;
    uint32_t sourceId = 0;
    uint64_t trimStartFrame = 0;
    std::optional<uint64_t> trimEndFrame;
    std::optional<SoundLoopRegion> loopRegion;
    SoundLoopProvenance loopProvenance = SoundLoopProvenance::Authored;
    QString rangeContentHash;

    bool operator==(const SoundSourceDef&) const = default;
};

struct SoundSourcePlatformOverride
{
    QUuid active;

    bool operator==(const SoundSourcePlatformOverride&) const = default;
};

struct SoundSourceSet
{
    std::vector<SoundSourceDef> entries;
    QUuid active;
    QMap<QString, SoundSourcePlatformOverride> platforms;

    bool operator==(const SoundSourceSet&) const = default;
};

struct SoundSources
{
    SoundSourceSet plain;
    QMap<QString, SoundSourceSet> localized;

    bool operator==(const SoundSources&) const = default;
};

class VSound : public IComponent
{
public:
    static constexpr ComponentType kType = ComponentType::Sound;
    ComponentType getType() const override { return kType; }

    SoundSources sources;
    double scheduleOffsetSec = 0.0;
    bool distinguishLanguage = false;

    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;
};

}
