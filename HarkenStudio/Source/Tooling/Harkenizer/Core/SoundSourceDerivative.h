#pragma once

#include "Components/VSound.h"
#include "ProjectPlatform.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace VNX {
class BuildSourceInput;
}

namespace VNX::SoundSourceDerivative {

struct FrameRange
{
    uint64_t start = 0;
    uint64_t end = 0;
};

struct DerivativeKey
{
    uint32_t sourceId = 0;
    QString inputFingerprint;
    ProjectPlatformId platform;
    uint64_t trimStartFrame = 0;
    uint64_t trimEndFrame = 0;
    std::optional<SoundLoopRegion> loopRegion;
    double scheduleOffsetSec = 0.0;
    uint32_t processingHash = 0;
    uint16_t sourceChannels = 0;
    QString processingFingerprint;
    QString compilerIdentity;

    bool operator==(const DerivativeKey&) const = default;
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
    QHash<QByteArray, uint64_t> contentRevisions_;
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

std::optional<FrameRange> mapFrameRange(const SoundSourceDef& source,
                                        uint64_t totalFrames);
QString cacheKey(const DerivativeKey& key);
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
                                                const BuildSourceInput* input = nullptr);

}
