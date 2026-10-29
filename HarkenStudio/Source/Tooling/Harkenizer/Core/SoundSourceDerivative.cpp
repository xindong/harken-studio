#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX::SoundSourceDerivative {
namespace {

constexpr char kDerivativeCacheNamespace[] = "harken.sound-source-derivative/v2";

void fail(QString* error, const QString& message)
{
    if (error) *error = message;
}

QString inputFingerprint(const QString& path)
{
    return AssetImporter::computeContentHash(path);
}

}

std::optional<MaterializedArtifact> MaterializationCache::find(const QByteArray& request)
{
    QMutexLocker lock(&mutex_);
    const auto found = artifacts_.constFind(request);
    if (found == artifacts_.cend()) return std::nullopt;
    return *found;
}

void MaterializationCache::insert(const QByteArray& request, const MaterializedArtifact& artifact)
{
    QMutexLocker lock(&mutex_);
    artifacts_.insert(request, artifact);
}

std::optional<FrameRange> mapFrameRange(const SoundSourceDef& source,
                                        uint64_t totalFrames)
{
    if (totalFrames == 0) return std::nullopt;
    const uint64_t start = std::min(source.trimStartFrame, totalFrames);
    const uint64_t end = std::clamp(source.trimEndFrame.value_or(totalFrames),
                                    start, totalFrames);
    if (end <= start) return std::nullopt;
    return FrameRange{start, end};
}

QString cacheKey(const DerivativeKey& key)
{
    QByteArray value(kDerivativeCacheNamespace);
    value += '\0';
    value += QByteArray::number(key.sourceId) + '\0';
    value += QByteArray::number(key.processingHash) + '\0';
    value += QByteArray::number(key.sourceChannels) + '\0';
    value += key.inputFingerprint.toUtf8() + '\0';
    value += ProjectPlatformSemantics::idString(key.platform).toUtf8() + '\0';
    value += QByteArray::number(static_cast<qulonglong>(key.trimStartFrame)) + '\0';
    value += QByteArray::number(static_cast<qulonglong>(key.trimEndFrame)) + '\0';
    value += QByteArray::number(key.scheduleOffsetSec, 'g', 17) + '\0';
    if (key.loopRegion)
        value += QByteArray::number(static_cast<qulonglong>(key.loopRegion->startFrame)) + ':'
            + QByteArray::number(static_cast<qulonglong>(key.loopRegion->endFrame));
    return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}

std::optional<PreparedMedia> prepare(const QString& inputPath,
                                     const SoundSourceDef& source,
                                     double scheduleOffsetSec,
                                     const ConversionSettings& settings,
                                     QString* error)
{
    if (!std::isfinite(scheduleOffsetSec) || scheduleOffsetSec < 0.0) {
        fail(error, QStringLiteral("Schedule offset is invalid"));
        return std::nullopt;
    }
    std::vector<float> samples;
    int channels = 0;
    int sampleRate = 0;
    if (!HarkenEditor::Bridge::decodeAudioFileInterleaved(inputPath, samples,
                                                         channels, sampleRate)) {
        fail(error, QStringLiteral("failed to decode source"));
        return std::nullopt;
    }
    if (channels < 1 || channels > 8 || sampleRate <= 0
        || samples.size() % static_cast<size_t>(channels) != 0) {
        fail(error, QStringLiteral("source format is not supported"));
        return std::nullopt;
    }
    const uint64_t totalFrames = static_cast<uint64_t>(
        samples.size() / static_cast<size_t>(channels));
    const auto range = mapFrameRange(source, totalFrames);
    if (!range) {
        fail(error, QStringLiteral("trim range is empty"));
        return std::nullopt;
    }
    const auto begin = samples.begin()
        + static_cast<std::ptrdiff_t>(range->start * static_cast<uint64_t>(channels));
    const auto end = samples.begin()
        + static_cast<std::ptrdiff_t>(range->end * static_cast<uint64_t>(channels));
    std::vector<float> trimmed(begin, end);
    LoopInfo loop;
    if (source.loopRegion) {
        const uint64_t start = std::clamp(source.loopRegion->startFrame, range->start, range->end);
        const uint64_t end = std::clamp(source.loopRegion->endFrame, start, range->end);
        if (end > start) {
            loop.enabled = true;
            loop.loopStart = start - range->start;
            loop.loopEnd = end - range->start - 1;
        }
    }
    if (settings.performResample) {
        if (!AssetConverter::processSamples(trimmed, sampleRate, channels, settings, loop, error))
            return std::nullopt;
    } else if (loop.enabled) {
        ++loop.loopEnd;
    }
    const uint32_t outputRate = static_cast<uint32_t>(sampleRate);
    const double offset = scheduleOffsetSec * outputRate;
    const size_t availableFrames = (trimmed.max_size() - trimmed.size())
        / static_cast<size_t>(channels);
    if (!std::isfinite(offset) || offset >= static_cast<double>(availableFrames)
        || offset >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        fail(error, QStringLiteral("schedule offset is too large"));
        return std::nullopt;
    }
    const uint64_t offsetFrames = static_cast<uint64_t>(std::llround(offset));
    if (offsetFrames > 0) {
        trimmed.insert(trimmed.begin(),
                       static_cast<size_t>(offsetFrames) * static_cast<size_t>(channels),
                       0.0f);
    }

    const uint64_t loopStart = loop.enabled ? offsetFrames + loop.loopStart : 0;
    const uint64_t loopEnd = loop.enabled ? offsetFrames + loop.loopEnd : 0;
    return PreparedMedia{std::move(trimmed), *range, outputRate,
                         static_cast<uint16_t>(channels), loopStart, loopEnd};
}

std::optional<MaterializedArtifact> materialize(const QString& inputPath,
                                                const SoundSourceDef& source,
                                                double scheduleOffsetSec,
                                                ProjectPlatformId platform,
                                                const QString& outputDir,
                                                const ConversionSettings& settings,
                                                QString* error,
                                                MaterializationCache* cache)
{
    if (platform.isNull()) {
        fail(error, QStringLiteral("Platform ID is required for derivative media"));
        return std::nullopt;
    }
    const QString inputRevision = inputFingerprint(inputPath);
    QByteArray request;
    if (cache && !inputRevision.isEmpty()) {
        QDataStream stream(&request, QIODevice::WriteOnly);
        stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
        stream << QFileInfo(inputPath).absoluteFilePath() << inputRevision
               << static_cast<quint32>(source.sourceId)
               << static_cast<quint64>(source.trimStartFrame)
               << source.trimEndFrame.has_value();
        if (source.trimEndFrame)
            stream << static_cast<quint64>(*source.trimEndFrame);
        stream << source.loopRegion.has_value();
        if (source.loopRegion)
            stream << static_cast<quint64>(source.loopRegion->startFrame)
                   << static_cast<quint64>(source.loopRegion->endFrame);
        stream << scheduleOffsetSec << platform
               << QJsonDocument(settings.toJson()).toJson(QJsonDocument::Compact);
        auto cached = cache->find(request);
        if (cached) {
            cached->path = QDir(outputDir).filePath(cached->cacheKey + QStringLiteral(".wav"));
            if (QFileInfo::exists(cached->path)) {
                if (inputFingerprint(inputPath) != inputRevision) {
                    fail(error, QStringLiteral("Source file changed during derivative conversion"));
                    return std::nullopt;
                }
                cached->cacheHit = true;
                return cached;
            }
        }
    }
    const auto media = prepare(inputPath, source, scheduleOffsetSec,
                                settings, error);
    if (!media)
        return std::nullopt;

    if (inputRevision.isEmpty() || inputFingerprint(inputPath) != inputRevision) {
        fail(error, QStringLiteral("Source file changed during derivative conversion"));
        return std::nullopt;
    }
    DerivativeKey key{source.sourceId, inputRevision, platform,
                      media->trimRange.start, media->trimRange.end,
                      source.loopRegion, scheduleOffsetSec, settings.hash(), media->channels};
    const QString id = cacheKey(key);
    const QString path = QDir(outputDir).filePath(id + QStringLiteral(".wav"));
    const bool cacheHit = QFileInfo::exists(path);
    if (!cacheHit
        && !AssetConverter::writeFloatWav(path, media->samples,
                                          media->channels, media->sampleRate)) {
        fail(error, QStringLiteral("failed to write derivative media"));
        return std::nullopt;
    }
    MaterializedArtifact artifact{path, id, media->sampleRate, media->channels,
                                  media->loopStartFrame,
                                  media->loopEndFrame,
                                  media->loopEndFrame != 0, cacheHit};
    if (cache) cache->insert(request, artifact);
    return artifact;
}

}
