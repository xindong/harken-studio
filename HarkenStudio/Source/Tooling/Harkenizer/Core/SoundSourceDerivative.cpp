#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/CacheArtifactWriter.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Tooling/Harkenizer/BuildIdentity.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX::SoundSourceDerivative {
namespace {

constexpr char kDerivativeCacheNamespace[] = "harken.sound-source-derivative/v4";

void fail(QString* error, const QString& message)
{
    if (error) *error = message;
}

QString inputFingerprint(const QString& path)
{
    return AssetImporter::computeContentHash(path);
}

QString digest(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool isDigest(const QString& value)
{
    return value.size() == 64 && std::all_of(value.cbegin(), value.cend(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    });
}

bool loadArtifact(MaterializedArtifact& artifact)
{
    if (!isDigest(artifact.cacheKey) || !isDigest(artifact.contentHash)
        || artifact.sampleRate == 0 || artifact.channels == 0 || artifact.channels > 8)
        return false;
    QFile file(artifact.path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const qint64 bytes = file.size();
    if (bytes <= 0 || bytes % (artifact.channels * sizeof(float)) != 0) return false;
    const uint64_t frames = bytes / (artifact.channels * sizeof(float));
    if (artifact.loopable != (artifact.loopEndFrame != 0)
        || (artifact.loopable
            ? artifact.loopStartFrame >= artifact.loopEndFrame || artifact.loopEndFrame > frames
            : artifact.loopStartFrame != 0)) return false;
    auto samples = std::make_shared<std::vector<float>>(bytes / sizeof(float));
    if (file.read(reinterpret_cast<char*>(samples->data()), bytes) != bytes
        || digest(QByteArray::fromRawData(reinterpret_cast<const char*>(samples->data()), bytes))
            != artifact.contentHash) return false;
    artifact.samples = std::move(samples);
    return true;
}

QString receiptPath(const QString& outputDir, const QByteArray& request)
{
    return QDir(outputDir).filePath(digest(request) + QStringLiteral(".json"));
}

std::optional<MaterializedArtifact> loadReceipt(const QString& outputDir, const QByteArray& request)
{
    QFile file(receiptPath(outputDir, request));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return std::nullopt;
    const auto receipt = QJsonDocument::fromJson(file.readAll()).object();
    const auto body = receipt.value(QStringLiteral("artifact")).toObject();
    if (body.value(QStringLiteral("request")).toString() != digest(request)
        || receipt.value(QStringLiteral("sha256")).toString()
            != digest(QJsonDocument(body).toJson(QJsonDocument::Compact))) return std::nullopt;
    MaterializedArtifact artifact;
    artifact.cacheKey = body.value(QStringLiteral("key")).toString();
    if (!isDigest(artifact.cacheKey)) return std::nullopt;
    artifact.path = QDir(outputDir).filePath(artifact.cacheKey + QStringLiteral(".pcm"));
    artifact.contentHash = body.value(QStringLiteral("content")).toString();
    const int sampleRate = body.value(QStringLiteral("sampleRate")).toInt();
    const int channels = body.value(QStringLiteral("channels")).toInt();
    if (sampleRate <= 0 || channels < 1 || channels > 8) return std::nullopt;
    artifact.sampleRate = static_cast<uint32_t>(sampleRate);
    artifact.channels = static_cast<uint16_t>(channels);
    bool startValid = false;
    bool endValid = false;
    artifact.loopStartFrame = body.value(QStringLiteral("loopStart")).toString().toULongLong(&startValid);
    artifact.loopEndFrame = body.value(QStringLiteral("loopEnd")).toString().toULongLong(&endValid);
    artifact.loopable = body.value(QStringLiteral("loopable")).toBool();
    if (!startValid || !endValid || !loadArtifact(artifact)) return std::nullopt;
    artifact.cacheHit = true;
    return artifact;
}

void saveReceipt(const QString& outputDir, const QByteArray& request, const MaterializedArtifact& artifact)
{
    const QJsonObject body{
        {QStringLiteral("request"), digest(request)},
        {QStringLiteral("key"), artifact.cacheKey},
        {QStringLiteral("content"), artifact.contentHash},
        {QStringLiteral("sampleRate"), static_cast<int>(artifact.sampleRate)},
        {QStringLiteral("channels"), artifact.channels},
        {QStringLiteral("loopStart"), QString::number(artifact.loopStartFrame)},
        {QStringLiteral("loopEnd"), QString::number(artifact.loopEndFrame)},
        {QStringLiteral("loopable"), artifact.loopable}};
    const auto bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("artifact"), body},
        {QStringLiteral("sha256"), digest(QJsonDocument(body).toJson(QJsonDocument::Compact))}})
        .toJson(QJsonDocument::Compact);
    writeCacheArtifact(receiptPath(outputDir, request), [&](QIODevice& file) {
        return file.write(bytes) == bytes.size();
    });
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
    contentRevisions_.clear();
}

std::optional<uint64_t> MaterializationCache::contentRevision(const MaterializedArtifact& artifact)
{
    if (artifact.contentHash.isEmpty()) return std::nullopt;
    QByteArray identity;
    QDataStream stream(&identity, QIODevice::WriteOnly);
    stream << artifact.contentHash << artifact.sampleRate << artifact.channels
           << artifact.loopable << static_cast<quint64>(artifact.loopStartFrame)
           << static_cast<quint64>(artifact.loopEndFrame);
    QMutexLocker lock(&mutex_);
    const auto found = contentRevisions_.constFind(identity);
    if (found != contentRevisions_.cend()) return *found;
    HarkenEditor::Bridge::PackMediaDesc media;
    media.interleavedSamples = artifact.samples;
    media.sampleRate = artifact.sampleRate;
    media.channels = artifact.channels;
    media.format = HarkenEditor::Bridge::PackMediaFormat::PcmFloat;
    media.loopable = artifact.loopable;
    media.loopStartFrame = artifact.loopStartFrame;
    media.loopEndFrame = artifact.loopEndFrame;
    uint64_t revision = 0;
    if (!HarkenEditor::Bridge::computePackMediaContentRevision(media, &revision)) return std::nullopt;
    contentRevisions_.insert(identity, revision);
    return revision;
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
    value += key.processingFingerprint.toUtf8() + '\0';
    value += key.compilerIdentity.toUtf8() + '\0';
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
                                                MaterializationCache* cache,
                                                const BuildSourceInput* input)
{
    if (platform.isNull()) {
        fail(error, QStringLiteral("Platform ID is required for derivative media"));
        return std::nullopt;
    }
    const QString inputRevision = input ? input->contentHash : inputFingerprint(inputPath);
    QByteArray request;
    if (!inputRevision.isEmpty()) {
        QDataStream stream(&request, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
        stream << QStringLiteral("harken.sound-source-materialization/v2")
               << QString::fromLatin1(Harkenizer::projectBuildIdentity()) << inputRevision
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
        auto cached = cache ? cache->find(request) : std::nullopt;
        if (!cached) cached = loadReceipt(outputDir, request);
        if (cached) {
            if (!input && inputFingerprint(inputPath) != inputRevision) {
                fail(error, QStringLiteral("Source file changed during derivative conversion"));
                return std::nullopt;
            }
            cached->cacheHit = true;
            if (cache) cache->insert(request, *cached);
            return cached;
        }
    }
    auto media = prepare(inputPath, source, scheduleOffsetSec,
                                settings, error);
    if (!media)
        return std::nullopt;

    if (inputRevision.isEmpty() || (!input && inputFingerprint(inputPath) != inputRevision)) {
        fail(error, QStringLiteral("Source file changed during derivative conversion"));
        return std::nullopt;
    }
    DerivativeKey key{source.sourceId, inputRevision, platform,
                      media->trimRange.start, media->trimRange.end,
                      source.loopRegion, scheduleOffsetSec, settings.hash(), media->channels,
                      digest(QJsonDocument(settings.toJson()).toJson(QJsonDocument::Compact)),
                      QString::fromLatin1(Harkenizer::projectBuildIdentity())};
    const QString id = cacheKey(key);
    const QString path = QDir(outputDir).filePath(id + QStringLiteral(".pcm"));
    const auto samples = std::make_shared<const std::vector<float>>(std::move(media->samples));
    const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(samples->data()),
        static_cast<qsizetype>(samples->size() * sizeof(float)));
    const QString contentHash = digest(bytes);
    const bool written = writeCacheArtifact(path, [&](QIODevice& file) {
        return file.write(bytes) == bytes.size();
    });
    if (!written) {
        fail(error, QStringLiteral("failed to write derivative media"));
        return std::nullopt;
    }
    MaterializedArtifact artifact{path, id, media->sampleRate, media->channels,
                                  media->loopStartFrame,
                                  media->loopEndFrame,
                                  media->loopEndFrame != 0, false, contentHash, samples};
    saveReceipt(outputDir, request, artifact);
    if (cache) cache->insert(request, artifact);
    return artifact;
}

}
