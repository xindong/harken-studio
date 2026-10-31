#pragma once

#include "AssetPipeline/MediaBuildSnapshot.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include <harken_studio/ObjectId.h>

#include <functional>
#include <memory>
#include <optional>

namespace VNX {

class PackMediaPreparation
{
public:
    struct Result
    {
        std::optional<MediaConversionRequest> conversion;
        SoundSourceDef source;
        std::optional<SoundSourceDerivative::MaterializedArtifact> artifact;
        QString error;
        QString conversionError;
        bool fallback = false;
    };

    PackMediaPreparation(const MediaBuildSnapshot& snapshot,
                         std::shared_ptr<SoundSourceDerivative::MaterializationCache> cache,
                         ProjectPlatformId platform, QString derivativeDir);
    ~PackMediaPreparation();

    void add(harkenstudio::ObjectId soundId, const SoundSourceDef& source,
             double scheduleOffsetSec);
    void start();
    Result take(harkenstudio::ObjectId soundId, const QUuid& entryId,
                const std::function<bool()>& cancelCheck);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
