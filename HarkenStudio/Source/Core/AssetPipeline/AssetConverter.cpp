
#include "AssetConverter.h"
#include "AssetRegistry.h"
#include "CacheArtifactWriter.h"
#include "PathUtils.h"
#include "PlatformProfile.h"
#include "../ProjectPlatformActivity.h"

#include <QFile>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QDateTime>
#include <QPointer>
#include <QDir>
#include <QFileInfo>
#include <QByteArray>
#include <QtConcurrent>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <limits>

#include <CDSPResampler.h>

namespace VNX {

namespace {

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

}


AssetConverter::AssetConverter(QObject* parent)
    : QObject(parent)
{
    threadPool_.setMaxThreadCount(QThread::idealThreadCount());
}

AssetConverter::~AssetConverter()
{
    cancelAllJobs();
    threadPool_.waitForDone();
}

void AssetConverter::setProjectRoot(const QString& root)
{
    projectRoot_ = root;
}


void AssetConverter::setBuildSnapshot(const MediaBuildSnapshot& snapshot)
{
    snapshot_ = snapshot;
    projectRoot_ = snapshot.projectRoot;
}

std::optional<MediaConversionRequest> AssetConverter::captureRequest(
    uint32_t sourceID, ProjectPlatformId platform, QString* error) const
{
    const auto request = snapshot_ ? snapshot_->request(sourceID, platform, error)
        : AssetRegistry::instance().conversionRequest(sourceID, platform, error);
    if (request && !projectRoot_.isEmpty()
        && QDir::cleanPath(projectRoot_) != QDir::cleanPath(request->projectRoot)) {
        if (error) *error = QStringLiteral("Conversion project changed before capture");
        return std::nullopt;
    }
    return request;
}

ConversionResult AssetConverter::convert(uint32_t sourceID, ProjectPlatformId platform)
{
    QString error;
    const auto request = captureRequest(sourceID, platform, &error);
    if (request) return convert(*request);
    ConversionResult result;
    result.sourceID = sourceID;
    result.platform = platform;
    result.errorMessage = error;
    return result;
}

ConversionResult AssetConverter::convert(const MediaConversionRequest& request)
{
    const auto activity = ProjectPlatformActivity::acquire();
    return doConversion(request);
}

QFuture<ConversionResult> AssetConverter::convertAsync(uint32_t sourceID, ProjectPlatformId platform)
{
    QString error;
    const auto request = captureRequest(sourceID, platform, &error);
    if (!request) {
        QPromise<ConversionResult> promise;
        promise.start();
        ConversionResult result;
        result.sourceID = sourceID;
        result.platform = platform;
        result.errorMessage = error;
        promise.addResult(result);
        promise.finish();
        return promise.future();
    }
    QMutexLocker lock(&jobMutex_);
    const QString key = request->jobKey();
    if (activeJobs_.contains(key) && !activeJobs_[key]->cancelled.load())
        return activeJobs_[key]->future;
    auto job = std::make_shared<JobState>();
    job->sourceID = sourceID;
    job->platform = platform;
    const auto activity = ProjectPlatformActivity::acquire();
    job->future = QtConcurrent::run(&threadPool_, [this, request = *request, job, activity]() {
        return doConversion(request, job);
    });
    auto* watcher = new QFutureWatcher<ConversionResult>(this);
    connect(watcher, &QFutureWatcher<ConversionResult>::finished, this,
            [this, key, job, watcher, activity]() {
        QMutexLocker lock(&jobMutex_);
        if (activeJobs_.value(key) == job) activeJobs_.remove(key);
        watcher->deleteLater();
    });
    activeJobs_[key] = job;
    watcher->setFuture(job->future);
    return job->future;
}

QFuture<QList<ConversionResult>> AssetConverter::convertBatchAsync(
    const QList<uint32_t>& sourceIDs, ProjectPlatformId platform)
{
    QList<QFuture<ConversionResult>> futures;
    for (uint32_t id : sourceIDs) futures.append(convertAsync(id, platform));
    return QtConcurrent::run(&threadPool_, [futures]() {
        QList<ConversionResult> results;
        for (auto future : futures) {
            future.waitForFinished();
            results.append(future.result());
        }
        return results;
    });
}

bool AssetConverter::isJobActive(uint32_t sourceID, ProjectPlatformId platform) const
{
    QMutexLocker lock(&jobMutex_);
    return std::any_of(activeJobs_.cbegin(), activeJobs_.cend(), [&](const auto& job) {
        return job->sourceID == sourceID && job->platform == platform;
    });
}

void AssetConverter::cancelJob(uint32_t sourceID, ProjectPlatformId platform)
{
    QMutexLocker lock(&jobMutex_);
    for (const auto& job : activeJobs_) {
        if (job->sourceID == sourceID && job->platform == platform)
            job->cancelled.store(true, std::memory_order_release);
    }
}

void AssetConverter::cancelAllJobs()
{
    QMutexLocker lock(&jobMutex_);
    for (auto& job : activeJobs_)
        job->cancelled.store(true, std::memory_order_release);
}

int AssetConverter::activeJobCount() const
{
    QMutexLocker lock(&jobMutex_);
    return activeJobs_.size();
}


bool AssetConverter::resample(const std::vector<float>& input,
                               int inputSampleRate, int outputSampleRate,
                               int channels,
                               std::vector<float>& output)
{
    if (inputSampleRate == outputSampleRate) {
        output = input;
        return true;
    }

    if (channels < 1 || input.empty())
        return false;

    size_t inputFrames = input.size() / channels;

    size_t outputFrames = static_cast<size_t>(
        std::ceil(static_cast<double>(inputFrames) * outputSampleRate / inputSampleRate));

    if (inputFrames > static_cast<size_t>(std::numeric_limits<int>::max())
        || outputFrames > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    std::vector<float> resampled(outputFrames * channels);
    if (channels == 1) {
        r8b::CDSPResampler24 resampler(inputSampleRate, outputSampleRate, 4096);
        resampler.oneshot(input.data(), static_cast<int>(inputFrames),
                          resampled.data(), static_cast<int>(outputFrames));
    } else {
        std::vector<float> channelInput(inputFrames);
        std::vector<float> channelOutput(outputFrames);
        for (int ch = 0; ch < channels; ++ch) {
            for (size_t f = 0; f < inputFrames; ++f)
                channelInput[f] = input[f * channels + ch];
            r8b::CDSPResampler24 resampler(inputSampleRate, outputSampleRate, 4096);
            resampler.oneshot(channelInput.data(), static_cast<int>(inputFrames),
                              channelOutput.data(), static_cast<int>(outputFrames));
            for (size_t f = 0; f < outputFrames; ++f)
                resampled[f * channels + ch] = channelOutput[f];
        }
    }
    output = std::move(resampled);

    return true;
}


void AssetConverter::convertToMono(std::vector<float>& data, int channels)
{
    if (channels <= 1)
        return;

    size_t frames = data.size() / channels;
    std::vector<float> mono(frames);

    float scale = 1.0f / channels;
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            sum += data[f * channels + ch];
        mono[f] = sum * scale;
    }

    data = std::move(mono);
}


bool AssetConverter::processSamples(std::vector<float>& samples, int& sampleRate, int& channels,
                                    const ConversionSettings& settings, LoopInfo& loop,
                                    QString* error)
{
    if (channels < 1 || channels > 2 || sampleRate <= 0 || samples.empty()) {
        if (error) *error = QStringLiteral("Only nonempty mono and stereo media are supported");
        return false;
    }
    const auto inputRate = static_cast<uint32_t>(sampleRate);
    const uint64_t inputFrames = samples.size() / static_cast<size_t>(channels);
    if (settings.performResample && settings.targetSampleRate != static_cast<uint32_t>(sampleRate)) {
        std::vector<float> output;
        if (settings.targetSampleRate == 0
            || !resample(samples, sampleRate, settings.targetSampleRate, channels, output)) {
            if (error) *error = QStringLiteral("Resampling failed");
            return false;
        }
        samples = std::move(output);
        sampleRate = settings.targetSampleRate;
    }
    loop = remapLoopInfo(loop, inputFrames, inputRate, static_cast<uint32_t>(sampleRate),
        samples.size() / static_cast<size_t>(channels));
    return true;
}

static bool writeFloatWavData(QIODevice& file, const std::vector<float>& interleaved,
                              uint16_t channels, uint32_t sampleRate,
                              QString* contentHash = nullptr)
{
    if (channels < 1 || channels > 2 || sampleRate == 0 || interleaved.empty()
        || interleaved.size() % channels != 0)
        return false;
    const uint64_t frames = interleaved.size() / channels;
    const uint64_t dataBytes = interleaved.size() * sizeof(float);
    const uint64_t riffSize = 4 + (8 + 18) + (8 + 4) + (8 + dataBytes);
    if (frames > std::numeric_limits<uint32_t>::max()
        || dataBytes > std::numeric_limits<uint32_t>::max()
        || riffSize > std::numeric_limits<uint32_t>::max())
        return false;
    const uint16_t blockAlign = channels * sizeof(float);
    const uint32_t byteRate = sampleRate * blockAlign;
    QByteArray header;
    header.reserve(58);
    header.append("RIFF", 4);
    appendU32(header, static_cast<uint32_t>(riffSize));
    header.append("WAVEfmt ", 8);
    appendU32(header, 18);
    appendU16(header, 3);
    appendU16(header, channels);
    appendU32(header, sampleRate);
    appendU32(header, byteRate);
    appendU16(header, blockAlign);
    appendU16(header, 32);
    appendU16(header, 0);
    header.append("fact", 4);
    appendU32(header, 4);
    appendU32(header, static_cast<uint32_t>(frames));
    header.append("data", 4);
    appendU32(header, static_cast<uint32_t>(dataBytes));
    const auto* samples = reinterpret_cast<const char*>(interleaved.data());
    if (file.write(header) != header.size()
        || file.write(samples, static_cast<qint64>(dataBytes)) != static_cast<qint64>(dataBytes))
        return false;
    if (contentHash) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(header);
        hash.addData(QByteArrayView(samples, static_cast<qsizetype>(dataBytes)));
        *contentHash = QString::fromLatin1(hash.result().toHex());
    }
    return true;
}

bool AssetConverter::writeFloatWav(const QString& path,
                                   const std::vector<float>& interleaved,
                                   uint16_t channels, uint32_t sampleRate)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly)
        && writeFloatWavData(file, interleaved, channels, sampleRate) && file.commit();
}

bool AssetConverter::writeFloatWavCache(const QString& path,
                                        const std::vector<float>& interleaved,
                                        uint16_t channels, uint32_t sampleRate,
                                        QString* contentHash)
{
    if (contentHash) contentHash->clear();
    QString writtenHash;
    const bool written = writeCacheArtifact(path, [&](QIODevice& file) {
        return writeFloatWavData(file, interleaved, channels, sampleRate,
                                 contentHash ? &writtenHash : nullptr);
    });
    if (written && contentHash) *contentHash = writtenHash;
    return written;
}


bool AssetConverter::decodeSource(const QString& path, std::vector<float>& samples,
                                   int& channels, int& sampleRate)
{
    if (!HarkenEditor::Bridge::decodeAudioFileInterleaved(path, samples,
                                                         channels, sampleRate)) {
        qWarning() << "AssetConverter: Failed to decode:" << path;
        return false;
    }
    return true;
}


ConversionResult AssetConverter::doConversion(
    const MediaConversionRequest& request, const std::shared_ptr<JobState>& job)
{
    const auto cancelled = [&]() { return job && job->cancelled.load(std::memory_order_acquire); };
    const uint32_t sourceID = request.asset.sourceID;
    const ProjectPlatformId platform = request.platform.id;
    const AssetInfo* asset = &request.asset;
    const ConversionSettings& settings = request.settings;
    auto emitProgress = [this, request, job](int percent) {
        QMetaObject::invokeMethod(this, [this, request, job, percent]() {
            if ((!job || !job->cancelled.load()) && AssetRegistry::instance().accepts(request))
                emit conversionProgress(request.asset.sourceID, request.platform.id, percent);
        }, Qt::QueuedConnection);
    };
    auto emitFailed = [this, request, job](const QString& msg) {
        QMetaObject::invokeMethod(this, [this, request, job, msg]() {
            if ((!job || !job->cancelled.load()) && AssetRegistry::instance().accepts(request))
                emit conversionFailed(request.asset.sourceID, request.platform.id, msg);
        }, Qt::QueuedConnection);
    };
    auto publish = [this, request, job](const CacheEntry& entry) {
        auto commit = [self = QPointer<AssetConverter>(this), request, job, entry]() {
            if ((!job || !job->cancelled.load()) && AssetRegistry::instance().updateCache(request, entry) && self)
                emit self->conversionComplete(request.asset.sourceID, request.platform.id, entry.cachePath);
        };
        if (QThread::currentThread() == AssetRegistry::instance().thread()) commit();
        else QMetaObject::invokeMethod(&AssetRegistry::instance(), commit, Qt::QueuedConnection);
    };
    ConversionResult result;
    result.sourceID = sourceID;
    result.platform = platform;
    if (cancelled()) {
        result.errorMessage = QStringLiteral("Cancelled");
        return result;
    }
    if (!request.sourceMatches()) {
        result.errorMessage = QStringLiteral("Source file changed or is unavailable; refresh the asset before conversion");
        emitFailed(result.errorMessage);
        return result;
    }
    emitProgress(10);
    if (request.cacheValid()) {
        result.success = true;
        result.cacheHit = true;
        result.cacheEntry = asset->platformCaches.value(platform);
        result.cacheEntry.lastAccessTime = QDateTime::currentSecsSinceEpoch();
        result.cachePath = result.cacheEntry.cachePath;
        result.absolutePath = QDir(PathUtils::getCacheDir(request.projectRoot)).filePath(result.cachePath);
        result.outputSize = static_cast<uint64_t>(QFileInfo(result.absolutePath).size());
        result.outputSampleRate = settings.performResample ? settings.targetSampleRate : asset->metadata.sampleRate;
        result.outputChannels = asset->metadata.channels;
        publish(result.cacheEntry);
        return result;
    }
    emitProgress(20);

    std::vector<float> samples;
    int channels, sampleRate;
    if (!decodeSource(request.inputPath(), samples, channels, sampleRate)) {
        result.errorMessage = "Failed to decode source file";
        emitFailed(result.errorMessage);
        return result;
    }

    uint64_t originalSize = samples.size() * sizeof(float);
    LoopInfo cacheLoop = asset->metadata.loopInfo;
    emitProgress(30);
    if (!processSamples(samples, sampleRate, channels, settings, cacheLoop, &result.errorMessage)) {
        emitFailed(result.errorMessage);
        return result;
    }
    emitProgress(60);

    emitProgress(80);

    if (cancelled() || !request.sourceMatches()) {
        result.errorMessage = cancelled() ? QStringLiteral("Cancelled") : QStringLiteral("Source file changed during conversion");
        return result;
    }
    QString cachePath = request.cachePath();
    QString absoluteCachePath = PathUtils::toAbsolutePath(cachePath,
                                                           PathUtils::getCacheDir(request.projectRoot));
    if (!writeCacheFile(absoluteCachePath, samples, static_cast<uint16_t>(channels),
                        static_cast<uint32_t>(sampleRate))) {
        result.errorMessage = "Failed to write cache file";
        emitFailed(result.errorMessage);
        return result;
    }

    emitProgress(90);

    if (cancelled() || !request.sourceMatches()) {
        result.errorMessage = cancelled() ? QStringLiteral("Cancelled") : QStringLiteral("Source file changed during conversion");
        return result;
    }

    result.cacheEntry.valid = true;
    result.cacheEntry.timestamp = QDateTime::currentSecsSinceEpoch();
    result.cacheEntry.lastAccessTime = result.cacheEntry.timestamp;
    result.cacheEntry.settingsHash = settings.hash();
    result.cacheEntry.fingerprint = request.fingerprint();
    result.cacheEntry.cachePath = cachePath;
    result.cacheEntry.loopInfo = cacheLoop;

    emitProgress(100);

    const uint64_t cacheSize = static_cast<uint64_t>(QFileInfo(absoluteCachePath).size());

    result.success = true;
    result.cachePath = cachePath;
    result.absolutePath = absoluteCachePath;
    result.outputSize = cacheSize;
    result.outputCodec = ConversionSettings::Codec::PCM_Float;
    result.outputSampleRate = static_cast<uint32_t>(sampleRate);
    result.outputChannels = static_cast<uint16_t>(channels);
    result.compressionRatio = (originalSize > 0)
        ? static_cast<float>(cacheSize) / static_cast<float>(originalSize) : 1.0f;

    publish(result.cacheEntry);
    return result;
}


bool AssetConverter::writeCacheFile(const QString& path, const std::vector<float>& interleaved,
                                     uint16_t channels, uint32_t sampleRate)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    if (channels < 1 || channels > 2 || sampleRate == 0 || interleaved.empty() ||
        interleaved.size() % channels != 0) {
        qWarning() << "Invalid cache WAV payload:" << path
                   << "channels" << channels
                   << "sampleRate" << sampleRate
                   << "samples" << interleaved.size();
        return false;
    }

    const uint64_t frames = static_cast<uint64_t>(interleaved.size() / channels);
    const uint64_t dataBytes64 = static_cast<uint64_t>(interleaved.size()) * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint64_t riffSize64 = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes64);
    if (frames > std::numeric_limits<uint32_t>::max() ||
        dataBytes64 > std::numeric_limits<uint32_t>::max() ||
        riffSize64 > std::numeric_limits<uint32_t>::max()) {
        qWarning() << "Cache WAV exceeds RIFF limits:" << path;
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to create cache file:" << path;
        return false;
    }

    const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
    const uint32_t byteRate = sampleRate * blockAlign;

    QByteArray header;
    header.reserve(58);
    header.append("RIFF", 4);
    appendU32(header, static_cast<uint32_t>(riffSize64));
    header.append("WAVE", 4);
    header.append("fmt ", 4);
    appendU32(header, fmtSize);
    appendU16(header, 3);
    appendU16(header, channels);
    appendU32(header, sampleRate);
    appendU32(header, byteRate);
    appendU16(header, blockAlign);
    appendU16(header, 32);
    appendU16(header, 0);
    header.append("fact", 4);
    appendU32(header, factSize);
    appendU32(header, static_cast<uint32_t>(frames));
    header.append("data", 4);
    appendU32(header, static_cast<uint32_t>(dataBytes64));

    if (file.write(header) != header.size())
        return false;

    const qint64 dataBytes = static_cast<qint64>(dataBytes64);
    if (file.write(reinterpret_cast<const char*>(interleaved.data()), dataBytes) != dataBytes)
        return false;

    return file.commit();
}

LoopInfo AssetConverter::remapLoopInfo(const LoopInfo& sourceLoop,
                                       uint64_t sourceFramesAfterTrim,
                                       uint32_t sourceSampleRate,
                                       uint32_t outputSampleRate,
                                       uint64_t outputFrames)
{
    LoopInfo out;
    if (!sourceLoop.enabled || sourceLoop.loopEnd < sourceLoop.loopStart
        || sourceLoop.loopEnd == std::numeric_limits<uint64_t>::max()
        || sourceFramesAfterTrim == 0 || sourceSampleRate == 0
        || outputSampleRate == 0 || outputFrames == 0) {
        return out;
    }

    const uint64_t start = std::min(sourceLoop.loopStart, sourceFramesAfterTrim);
    const uint64_t end = std::min(sourceLoop.loopEnd + 1, sourceFramesAfterTrim);
    if (end <= start)
        return out;

    auto scaleFrame = [sourceSampleRate, outputSampleRate](uint64_t frame) -> uint64_t {
        const long double scaled =
            static_cast<long double>(frame) * static_cast<long double>(outputSampleRate) /
            static_cast<long double>(sourceSampleRate);
        return static_cast<uint64_t>(std::llround(scaled));
    };

    out.enabled = true;
    out.loopStart = std::min(scaleFrame(start), outputFrames);
    out.loopEnd = std::min(scaleFrame(end), outputFrames);
    out.loopCount = sourceLoop.loopCount;

    if (out.loopEnd <= out.loopStart)
        out = LoopInfo{};

    return out;
}

}
