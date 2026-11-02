#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/CacheArtifactWriter.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX::SoundSourceDerivative {
namespace {

constexpr quint32 kArtifactMagic = 0x4d435048;
constexpr quint32 kArtifactVersion = 1;
constexpr qint64 kHeaderBytes = 40;
constexpr qint64 kDigestBytes = 32;

void fail(QString* error, const QString& message)
{
    if (error) *error = message;
}

std::optional<FrameRange> loopRange(const SoundSourceDef& source, const FrameRange& trim)
{
    if (!source.loopRegion) return std::nullopt;
    const auto start = std::clamp(source.loopRegion->startFrame, trim.start, trim.end);
    const auto end = std::clamp(source.loopRegion->endFrame, start, trim.end);
    return end > start ? std::optional<FrameRange>(FrameRange{start - trim.start, end - trim.start})
        : std::nullopt;
}

std::optional<uint64_t> scheduleFrames(double seconds, uint32_t sampleRate)
{
    const auto offset = seconds * sampleRate;
    if (seconds < 0.0 || !std::isfinite(offset)
        || offset >= static_cast<double>(std::numeric_limits<int64_t>::max())) return std::nullopt;
    return static_cast<uint64_t>(std::llround(offset));
}

QByteArray requestKey(const QString& inputFingerprint, const SoundSourceDef& source,
                      double scheduleOffsetSec, const ConversionSettings& settings,
                      const AudioMetadata& metadata)
{
    if (inputFingerprint.isEmpty() || !std::isfinite(scheduleOffsetSec) || scheduleOffsetSec < 0.0
        || (settings.performResample && settings.targetSampleRate == 0))
        return {};
    const auto range = mapFrameRange(source, metadata.totalSamples);
    const auto outputRate = settings.performResample ? settings.targetSampleRate : metadata.sampleRate;
    const auto offset = scheduleFrames(scheduleOffsetSec, outputRate);
    if (!range || outputRate == 0 || !offset) return {};
    const auto loop = loopRange(source, *range);
    QByteArray request;
    QDataStream stream(&request, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kArtifactVersion << static_cast<quint32>(kMediaCacheArtifactVersion)
           << inputFingerprint << static_cast<quint64>(range->start) << static_cast<quint64>(range->end)
           << static_cast<quint64>(loop ? loop->start : 0) << static_cast<quint64>(loop ? loop->end : 0)
           << static_cast<quint64>(*offset)
           << static_cast<quint32>(outputRate == metadata.sampleRate ? 0 : outputRate);
    return QCryptographicHash::hash(request, QCryptographicHash::Sha256).toHex();
}

QByteArray artifactHeader(const MaterializedArtifact& artifact)
{
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << kArtifactMagic << kArtifactVersion << static_cast<quint32>(artifact.sampleRate)
           << static_cast<quint32>(artifact.channels)
           << static_cast<quint64>(artifact.loopStartFrame)
           << static_cast<quint64>(artifact.loopEndFrame) << static_cast<quint64>(artifact.revision);
    return header;
}

QByteArray artifactDigest(const QByteArray& header, const std::vector<float>& samples)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(header);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(samples.data()),
        static_cast<qsizetype>(samples.size() * sizeof(float))));
    return hash.result();
}

std::optional<MaterializedArtifact> loadArtifact(const QString& outputDir, const QByteArray& key)
{
    if (outputDir.isEmpty()) return std::nullopt;
    MaterializedArtifact artifact;
    artifact.cacheKey = QString::fromLatin1(key);
    artifact.path = QDir(outputDir).filePath(artifact.cacheKey + QStringLiteral(".pcm"));
    QFile file(artifact.path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const auto header = file.read(kHeaderBytes);
    const auto digest = file.read(kDigestBytes);
    if (header.size() != kHeaderBytes || digest.size() != kDigestBytes) return std::nullopt;
    QDataStream stream(header);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0, version = 0, sampleRate = 0, channels = 0;
    quint64 loopStart = 0, loopEnd = 0, revision = 0;
    stream >> magic >> version >> sampleRate >> channels >> loopStart >> loopEnd >> revision;
    const auto payloadBytes = file.size() - kHeaderBytes - kDigestBytes;
    if (stream.status() != QDataStream::Ok || magic != kArtifactMagic || version != kArtifactVersion
        || sampleRate == 0 || channels == 0 || channels > 8 || payloadBytes <= 0
        || payloadBytes % (channels * sizeof(float)) != 0)
        return std::nullopt;
    const auto frames = static_cast<quint64>(payloadBytes / (channels * sizeof(float)));
    if (loopEnd == 0 ? loopStart != 0 : loopStart >= loopEnd || loopEnd > frames)
        return std::nullopt;
    auto samples = std::make_shared<std::vector<float>>(payloadBytes / sizeof(float));
    if (file.read(reinterpret_cast<char*>(samples->data()), payloadBytes) != payloadBytes
        || artifactDigest(header, *samples) != digest)
        return std::nullopt;
    artifact.sampleRate = sampleRate;
    artifact.channels = static_cast<uint16_t>(channels);
    artifact.loopStartFrame = loopStart;
    artifact.loopEndFrame = loopEnd;
    artifact.loopable = loopEnd != 0;
    artifact.cacheHit = true;
    artifact.contentHash = QString::fromLatin1(digest.toHex());
    artifact.samples = std::move(samples);
    artifact.revision = revision;
    return artifact;
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

void MaterializationCache::clear()
{
    QMutexLocker lock(&mutex_);
    artifacts_.clear();
}

std::optional<uint64_t> MaterializationCache::contentRevision(const MaterializedArtifact& artifact)
{
    return artifact.samples ? std::optional<uint64_t>(artifact.revision) : std::nullopt;
}

std::optional<FrameRange> mapFrameRange(const SoundSourceDef& source, uint64_t totalFrames)
{
    if (totalFrames == 0) return std::nullopt;
    const uint64_t start = std::min(source.trimStartFrame, totalFrames);
    const uint64_t end = std::clamp(source.trimEndFrame.value_or(totalFrames), start, totalFrames);
    if (end <= start) return std::nullopt;
    return FrameRange{start, end};
}

static std::optional<MaterializedArtifact> findCached(const QByteArray& key,
                                                     const QString& outputDir,
                                                     MaterializationCache* cache)
{
    if (key.isEmpty()) return std::nullopt;
    if (cache) {
        auto artifact = cache->find(key);
        if (artifact) {
            artifact->cacheHit = true;
            return artifact;
        }
    }
    auto artifact = loadArtifact(outputDir, key);
    if (artifact) {
        artifact->cacheHit = true;
        if (cache) cache->insert(key, *artifact);
    }
    return artifact;
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
    if (const auto region = loopRange(source, *range)) {
        loop.enabled = true;
        loop.loopStart = region->start;
        loop.loopEnd = region->end - 1;
    }
    if (settings.performResample && settings.targetSampleRate != static_cast<uint32_t>(sampleRate)) {
        if (!AssetConverter::processSamples(trimmed, sampleRate, channels, settings, loop, error))
            return std::nullopt;
    } else if (loop.enabled) {
        ++loop.loopEnd;
    }
    const uint32_t outputRate = static_cast<uint32_t>(sampleRate);
    const double offset = scheduleOffsetSec * outputRate;
    const auto mappedOffset = scheduleFrames(scheduleOffsetSec, outputRate);
    const size_t availableFrames = (trimmed.max_size() - trimmed.size())
        / static_cast<size_t>(channels);
    if (!mappedOffset || offset >= static_cast<double>(availableFrames)) {
        fail(error, QStringLiteral("schedule offset is too large"));
        return std::nullopt;
    }
    const uint64_t offsetFrames = *mappedOffset;
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
                                                MaterializationCache* cache,
                                                const BuildSourceInput* input,
                                                const std::function<std::shared_ptr<const BuildSourceInput>(QString*)>&
                                                    captureSource)
{
    if (platform.isNull()) {
        fail(error, QStringLiteral("Platform ID is required for derivative media"));
        return std::nullopt;
    }
    const QString inputRevision = input ? input->contentHash : AssetImporter::computeContentHash(inputPath);
    const auto metadata = input ? input->metadata : AssetImporter().readMetadata(inputPath);
    const auto key = requestKey(inputRevision, source, scheduleOffsetSec, settings, metadata);
    if (auto cached = findCached(key, outputDir, cache))
        return cached;
    std::shared_ptr<const BuildSourceInput> captured;
    if (captureSource) {
        captured = captureSource(error);
        if (!captured) return std::nullopt;
        input = captured.get();
    }
    auto media = prepare(input ? input->inputPath : inputPath, source, scheduleOffsetSec, settings, error);
    if (!media) return std::nullopt;
    if (key.isEmpty()) {
        fail(error, QStringLiteral("Cannot identify converted media input"));
        return std::nullopt;
    }
    if (inputRevision.isEmpty() || (!input && AssetImporter::computeContentHash(inputPath) != inputRevision)) {
        fail(error, QStringLiteral("Source file changed during derivative conversion"));
        return std::nullopt;
    }
    MaterializedArtifact artifact;
    artifact.cacheKey = QString::fromLatin1(key);
    if (!outputDir.isEmpty())
        artifact.path = QDir(outputDir).filePath(artifact.cacheKey + QStringLiteral(".pcm"));
    artifact.sampleRate = media->sampleRate;
    artifact.channels = media->channels;
    artifact.loopStartFrame = media->loopStartFrame;
    artifact.loopEndFrame = media->loopEndFrame;
    artifact.loopable = media->loopEndFrame != 0;
    artifact.samples = std::make_shared<const std::vector<float>>(std::move(media->samples));
    HarkenEditor::Bridge::PackMediaDesc description;
    description.interleavedSamples = artifact.samples;
    description.sampleRate = artifact.sampleRate;
    description.channels = artifact.channels;
    description.format = HarkenEditor::Bridge::PackMediaFormat::PcmFloat;
    description.loopable = artifact.loopable;
    description.loopStartFrame = artifact.loopStartFrame;
    description.loopEndFrame = artifact.loopEndFrame;
    if (!HarkenEditor::Bridge::computePackMediaContentRevision(description, &artifact.revision)) {
        fail(error, QStringLiteral("Cannot compute converted media revision"));
        return std::nullopt;
    }
    const auto header = artifactHeader(artifact);
    const auto digest = artifactDigest(header, *artifact.samples);
    artifact.contentHash = QString::fromLatin1(digest.toHex());
    const auto payloadBytes = static_cast<qint64>(artifact.samples->size() * sizeof(float));
    if (!outputDir.isEmpty() && !writeCacheArtifact(artifact.path, [&](QIODevice& file) {
            return file.write(header) == header.size() && file.write(digest) == digest.size()
                && file.write(reinterpret_cast<const char*>(artifact.samples->data()), payloadBytes) == payloadBytes;
        })) {
        fail(error, QStringLiteral("failed to write derivative media"));
        return std::nullopt;
    }
    if (cache) cache->insert(key, artifact);
    return artifact;
}

}
