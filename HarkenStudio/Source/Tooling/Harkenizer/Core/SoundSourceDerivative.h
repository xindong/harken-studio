#pragma once

#include "Components/VSound.h"
#include "ProjectPlatform.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace VNX {
class BuildSourceInput;
struct AudioMetadata;
}

namespace VNX::SoundSourceDerivative {

struct FrameRange
{
    uint64_t start = 0;
    uint64_t end = 0;
};

struct MaterializedArtifact
{
    QString path;
    QString cacheKey;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t loopStartFrame = 0;
    uint64_t loopEndFrame = 0;
    bool loopable = false;
    bool cacheHit = false;
    QString contentHash;
    std::shared_ptr<const std::vector<float>> samples;
    uint64_t revision = 0;
};

class MaterializationCache
{
public:
    std::optional<MaterializedArtifact> find(const QByteArray& request);
    void insert(const QByteArray& request, const MaterializedArtifact& artifact);
    std::optional<uint64_t> contentRevision(const MaterializedArtifact& artifact);
    void clear();

private:
    QMutex mutex_;
    QHash<QByteArray, MaterializedArtifact> artifacts_;
};

struct PreparedMedia
{
    std::vector<float> samples;
    FrameRange trimRange;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t loopStartFrame = 0;
    uint64_t loopEndFrame = 0;
};

QString cacheKey(const QString& inputFingerprint, const SoundSourceDef& source,
                 double scheduleOffsetSec, const ConversionSettings& settings,
                 const AudioMetadata& metadata);

std::optional<FrameRange> mapFrameRange(const SoundSourceDef& source,
                                        uint64_t totalFrames);
std::optional<PreparedMedia> prepare(const QString& inputPath,
                                     const SoundSourceDef& source,
                                     double scheduleOffsetSec,
                                     const ConversionSettings& settings,
                                     QString* error = nullptr);
std::optional<MaterializedArtifact> materialize(const QString& inputPath,
                                                const SoundSourceDef& source,
                                                double scheduleOffsetSec,
                                                ProjectPlatformId platform,
                                                const QString& outputDir,
                                                const ConversionSettings& settings,
                                                QString* error = nullptr,
                                                MaterializationCache* cache = nullptr,
                                                const BuildSourceInput* input = nullptr,
                                                const std::function<std::shared_ptr<const BuildSourceInput>(QString*)>&
                                                    captureSource = {});

}
