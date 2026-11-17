#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/CacheArtifactWriter.h"
#include "Core/Build/PackagingBridge.h"
#include "Core/SoundSourceSemantics.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QScopeGuard>

#include <vorbis/codec.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <limits>

namespace VNX::SoundSourceDerivative {
namespace {

constexpr quint32 kArtifactMagic = 0x4d435048;
constexpr quint32 kArtifactVersion = 3;
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
                      double scheduleOffsetSec, const std::optional<ResolvedConversion>& conversion,
                      const AudioMetadata& metadata)
{
    if (inputFingerprint.isEmpty() || !std::isfinite(scheduleOffsetSec) || scheduleOffsetSec < 0.0)
        return {};
    const auto format = conversion ? conversion->format : ConversionFormat::PCM;
    if (format != ConversionFormat::PCM && format != ConversionFormat::Vorbis) return {};
    if (format == ConversionFormat::Vorbis && (!std::isfinite(conversion->vorbisQuality)
        || conversion->vorbisQuality < 0.0f || conversion->vorbisQuality > 1.0f)) return {};
    const auto range = mapFrameRange(source, metadata.totalSamples);
    const auto targetSampleRateHz = conversion ? conversion->sampleRate : 0;
    const auto outputRate = targetSampleRateHz != 0 ? targetSampleRateHz : metadata.sampleRate;
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
           << static_cast<quint8>(format)
           << static_cast<quint32>(outputRate == metadata.sampleRate ? 0 : outputRate);
    if (format == ConversionFormat::Vorbis)
        stream << conversion->vorbisQuality << QString::fromUtf8(vorbis_version_string())
               << static_cast<quint32>(kVorbisSeekGrainFrames)
               << static_cast<quint32>(VorbisEncoder::kFormatVersion);
    const auto canonicalSource = SoundSourceSemantics::canonicalized(source, metadata.totalSamples);
    for (const auto* fade : {&canonicalSource.fadeIn, &canonicalSource.fadeOut}) {
        stream << static_cast<quint64>(fade->durationFrames);
        if (fade->durationFrames == 0) continue;
        for (const auto& point : fade->curve)
            stream << point.x << point.value;
        for (const auto& handles : fade->handles)
            stream << handles.h1x << handles.h1y << handles.h2x << handles.h2y;
    }
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

std::optional<MaterializedArtifact> loadArtifact(const QString& outputDir, const QByteArray& key,
                                                ConversionFormat format)
{
    if (outputDir.isEmpty()) return std::nullopt;
    MaterializedArtifact artifact;
    artifact.format = format;
    artifact.cacheKey = QString::fromLatin1(key);
    if (format == ConversionFormat::Vorbis) {
        artifact.path = QDir(outputDir).filePath(artifact.cacheKey + QStringLiteral(".vcm"));
        auto encoded = std::make_shared<VorbisEncoder::EncodedMedia>();
        if (!readVorbisCacheArtifact(artifact.path, *encoded, artifact.revision,
                                    &artifact.contentHash))
            return std::nullopt;
        artifact.sampleRate = encoded->sampleRate;
        artifact.channels = encoded->channels;
        artifact.loopStartFrame = encoded->loopStartFrame;
        artifact.loopEndFrame = encoded->loopEndFrame;
        artifact.loopable = encoded->loopEndFrame != 0;
        artifact.cacheHit = true;
        artifact.encodedMedia = std::move(encoded);
        return artifact;
    }
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

bool publishArtifact(MaterializedArtifact& artifact, const QString& outputDir, QString* error)
{
    if (outputDir.isEmpty()) {
        artifact.path.clear();
        return true;
    }
    const auto path = QDir(outputDir).filePath(artifact.cacheKey
        + (artifact.format == ConversionFormat::Vorbis ? QStringLiteral(".vcm") : QStringLiteral(".pcm")));
    if (artifact.path == path && QFileInfo(path).isFile()) return true;
    bool written = false;
    if (artifact.format == ConversionFormat::Vorbis) {
        if (artifact.cacheImage.isEmpty())
            artifact.cacheImage = encodeVorbisCacheArtifact(*artifact.encodedMedia,
                artifact.revision, &artifact.contentHash, error);
        if (artifact.cacheImage.isEmpty()) return false;
        written = writeCacheArtifact(path, [&](QIODevice& file) {
            return file.write(artifact.cacheImage) == artifact.cacheImage.size();
        });
    } else {
        const auto header = artifactHeader(artifact);
        const auto digest = QByteArray::fromHex(artifact.contentHash.toLatin1());
        const auto payloadBytes = static_cast<qint64>(artifact.samples->size() * sizeof(float));
        written = writeCacheArtifact(path, [&](QIODevice& file) {
            return file.write(header) == header.size() && file.write(digest) == digest.size()
                && file.write(reinterpret_cast<const char*>(artifact.samples->data()), payloadBytes) == payloadBytes;
        });
    }
    if (!written) {
        fail(error, QStringLiteral("Failed to write converted media cache: %1").arg(path));
        return false;
    }
    artifact.path = path;
    return true;
}

}

QString cacheKey(const QString& inputFingerprint, const SoundSourceDef& source,
                 double scheduleOffsetSec, const std::optional<ResolvedConversion>& conversion,
                 const AudioMetadata& metadata)
{
    return QString::fromLatin1(requestKey(inputFingerprint, source, scheduleOffsetSec, conversion, metadata));
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

std::optional<MaterializedArtifact> MaterializationCache::getOrCreate(const QByteArray& request,
    const std::function<std::optional<MaterializedArtifact>(QString*)>& create, QString* error,
    const QString& outputDir)
{
    struct Result
    {
        std::optional<MaterializedArtifact> artifact;
        QString error;
    };
    struct Task
    {
        std::shared_future<Result> result;
        size_t clients = 0;
    };
    static QMutex pendingMutex;
    static QHash<QByteArray, std::shared_ptr<Task>> pending;
    std::promise<Result> promise;
    std::shared_ptr<Task> task;
    bool producer = false;
    {
        QMutexLocker lock(&pendingMutex);
        const auto found = pending.constFind(request);
        if (found != pending.cend()) {
            task = *found;
        } else {
            task = std::make_shared<Task>();
            task->result = promise.get_future().share();
            pending.insert(request, task);
            producer = true;
        }
        ++task->clients;
    }
    const auto release = qScopeGuard([&] {
        QMutexLocker lock(&pendingMutex);
        if (--task->clients == 0) pending.remove(request);
    });
    if (producer) {
        try {
            Result result;
            result.artifact = find(request);
            if (result.artifact) result.artifact->cacheHit = true;
            else result.artifact = create(&result.error);
            if (result.artifact) insert(request, *result.artifact);
            promise.set_value(std::move(result));
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    }
    const auto& result = task->result.get();
    if (error && !result.artifact) *error = result.error;
    auto artifact = result.artifact;
    if (artifact && !producer) {
        artifact->cacheHit = true;
        insert(request, *artifact);
    }
    if (artifact) {
        if (!publishArtifact(*artifact, outputDir, error)) return std::nullopt;
        insert(request, *artifact);
    }
    return artifact;
}

void MaterializationCache::clear()
{
    QMutexLocker lock(&mutex_);
    artifacts_.clear();
}

std::optional<uint64_t> MaterializationCache::contentRevision(const MaterializedArtifact& artifact)
{
    return artifact.samples || artifact.encodedMedia
        ? std::optional<uint64_t>(artifact.revision) : std::nullopt;
}

std::optional<FrameRange> mapFrameRange(const SoundSourceDef& source, uint64_t totalFrames)
{
    if (totalFrames == 0) return std::nullopt;
    const uint64_t start = std::min(source.trimStartFrame, totalFrames);
    const uint64_t end = std::clamp(source.trimEndFrame.value_or(totalFrames), start, totalFrames);
    if (end <= start) return std::nullopt;
    return FrameRange{start, end};
}

std::optional<PreparedMedia> prepare(const QString& inputPath,
                                     const SoundSourceDef& source,
                                     double scheduleOffsetSec,
                                     const std::optional<ResolvedConversion>& conversion,
                                     QString* error)
{
    if (conversion && conversion->format != ConversionFormat::PCM
        && conversion->format != ConversionFormat::Vorbis) {
        fail(error, QStringLiteral("Conversion format is invalid"));
        return std::nullopt;
    }
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
    SoundSourceSemantics::applyFadeEnvelope(trimmed, static_cast<uint32_t>(channels),
                                           source, totalFrames, range->start);
    LoopInfo loop;
    if (const auto region = loopRange(source, *range)) {
        loop.enabled = true;
        loop.loopStart = region->start;
        loop.loopEnd = region->end - 1;
    }
    const auto targetSampleRateHz = conversion ? conversion->sampleRate : 0;
    if (targetSampleRateHz != 0 && targetSampleRateHz != static_cast<uint32_t>(sampleRate)) {
        if (!AssetConverter::processSamples(trimmed, sampleRate, channels, targetSampleRateHz, loop, error))
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
                                                const std::optional<ResolvedConversion>& conversion,
                                                QString* error,
                                                MaterializationCache* cache,
                                                const BuildSourceInput* input,
                                                const std::function<std::shared_ptr<const BuildSourceInput>(QString*)>&
                                                    captureSource)
{
    if (conversion && conversion->format != ConversionFormat::PCM
        && conversion->format != ConversionFormat::Vorbis) {
        fail(error, QStringLiteral("Conversion format is invalid"));
        return std::nullopt;
    }
    if (platform.isNull()) {
        fail(error, QStringLiteral("Platform ID is required for derivative media"));
        return std::nullopt;
    }
    std::shared_ptr<const BuildSourceInput> directInput;
    if (!input) {
        BuildSourceInputs directInputs;
        AssetInfo asset;
        asset.sourceID = source.sourceId;
        asset.absolutePath = inputPath;
        directInput = directInputs.capture(asset, error);
        if (!directInput) return std::nullopt;
        input = directInput.get();
    }
    const QString inputRevision = input->contentHash;
    const auto metadata = input->metadata;
    const auto key = requestKey(inputRevision, source, scheduleOffsetSec, conversion, metadata);
    if (key.isEmpty()) {
        fail(error, QStringLiteral("Cannot identify converted media input"));
        return std::nullopt;
    }
    const auto format = conversion ? conversion->format : ConversionFormat::PCM;
    const auto create = [&](QString* creationError) -> std::optional<MaterializedArtifact> {
        if (auto cached = loadArtifact(outputDir, key, format)) return cached;
        std::shared_ptr<const BuildSourceInput> captured;
        if (captureSource) {
            captured = captureSource(creationError);
            if (!captured) return std::nullopt;
            if (requestKey(captured->contentHash, source, scheduleOffsetSec, conversion,
                           captured->metadata) != key) {
                fail(creationError, QStringLiteral("Source changed during media conversion"));
                return std::nullopt;
            }
            input = captured.get();
        }
        auto media = prepare(input->inputPath, source, scheduleOffsetSec, conversion, creationError);
        if (!media) return std::nullopt;
        MaterializedArtifact artifact;
        artifact.format = format;
        artifact.cacheKey = QString::fromLatin1(key);
        artifact.sampleRate = media->sampleRate;
        artifact.channels = media->channels;
        artifact.loopStartFrame = media->loopStartFrame;
        artifact.loopEndFrame = media->loopEndFrame;
        artifact.loopable = media->loopEndFrame != 0;
        if (format == ConversionFormat::Vorbis) {
            auto encoded = VorbisEncoder::encode(media->samples, media->sampleRate, media->channels,
                conversion->vorbisQuality, media->loopStartFrame, media->loopEndFrame, creationError);
            if (!encoded) return std::nullopt;
            artifact.revision = key.left(16).toULongLong(nullptr, 16);
            artifact.cacheImage = encodeVorbisCacheArtifact(*encoded, artifact.revision,
                &artifact.contentHash, creationError);
            if (artifact.cacheImage.isEmpty()) return std::nullopt;
            artifact.encodedMedia = std::make_shared<const VorbisEncoder::EncodedMedia>(std::move(*encoded));
        } else {
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
                fail(creationError, QStringLiteral("Cannot compute converted media revision"));
                return std::nullopt;
            }
            const auto header = artifactHeader(artifact);
            const auto digest = artifactDigest(header, *artifact.samples);
            artifact.contentHash = QString::fromLatin1(digest.toHex());
        }
        return artifact;
    };
    MaterializationCache localCache;
    return (cache ? *cache : localCache).getOrCreate(key, create, error, outputDir);
}

}
