
#include "WaveformCache.h"
#include "AudioFormatCatalog.h"
#include "AssetRegistry.h"
#include "PathUtils.h"
#include "WaveformAnalysis.h"
#include "Utils/AudioFileReader.h"

#include "../ProjectTypes.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QSet>
#include <QWaitCondition>

#include <atomic>

#include <algorithm>
#include <QtConcurrent/QtConcurrent>

#include <dr_wav/dr_wav.h>

#include "RuntimeBridge/PackagingBridge.h"

namespace VNX {

namespace {

QMutex& waveformWriteMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, quint64>& waveformRevisions()
{
    static QHash<QString, quint64> revisions;
    return revisions;
}

int waveformChangeDepth = 0;
int waveformActiveReads = 0;
QWaitCondition waveformReadersIdle;
std::atomic<quint64> waveformReadEpoch{0};
QHash<QString, uint32_t> deferredWaveforms;
QHash<QString, QPair<quint64, uint32_t>> pendingWaveforms;

struct SharedWaveform
{
    qint64 sourceSize;
    qint64 sourceMtime;
    qint64 cacheSize;
    qint64 cacheMtime;
    std::weak_ptr<const WaveformPeakData> data;
};

QHash<QString, SharedWaveform>& sharedWaveforms()
{
    static QHash<QString, SharedWaveform> waveforms;
    return waveforms;
}

std::atomic<quint64>& waveformRevisionCounter()
{
    static std::atomic<quint64> counter{0};
    return counter;
}

QString normalizedAudioPath(const QString& audioFilePath)
{
    return PathUtils::normalizeToUnix(
        QDir::cleanPath(QFileInfo(audioFilePath).absoluteFilePath()));
}

quint64 beginWaveformGeneration(const QString& audioFilePath, uint32_t targetPeaks = 0)
{
    QMutexLocker lock(&waveformWriteMutex());
    const QString path = normalizedAudioPath(audioFilePath);
    if (waveformChangeDepth != 0) {
        if (targetPeaks != 0)
            deferredWaveforms.insert(path, targetPeaks);
        return 0;
    }
    const quint64 revision = waveformRevisionCounter().fetch_add(1, std::memory_order_relaxed) + 1;
    waveformRevisions()[path] = revision;
    if (targetPeaks != 0)
        pendingWaveforms.insert(path, {revision, targetPeaks});
    return revision;
}

bool isCurrentWaveformGeneration(const QString& audioFilePath, quint64 revision)
{
    return revision != 0 && waveformRevisions().value(normalizedAudioPath(audioFilePath)) == revision;
}

class WaveformReadScope
{
public:
    explicit WaveformReadScope(const QString& path = {}, quint64 revision = 0)
        : path_(path.isEmpty() ? QString() : normalizedAudioPath(path)), revision_(revision)
    {
        QMutexLocker lock(&waveformWriteMutex());
        if (waveformChangeDepth != 0
            || (!path.isEmpty() && !isCurrentWaveformGeneration(path, revision)))
            return;
        epoch_ = waveformReadEpoch.load(std::memory_order_relaxed);
        active_ = true;
        ++waveformActiveReads;
    }

    ~WaveformReadScope()
    {
        QMutexLocker lock(&waveformWriteMutex());
        const auto pending = pendingWaveforms.constFind(path_);
        if (pending != pendingWaveforms.cend() && pending->first == revision_)
            pendingWaveforms.remove(path_);
        if (active_ && --waveformActiveReads == 0)
            waveformReadersIdle.wakeAll();
    }

    bool active() const { return active_; }
    bool cancelled() const
    {
        return epoch_ != waveformReadEpoch.load(std::memory_order_relaxed);
    }

private:
    QString path_;
    quint64 revision_ = 0;
    quint64 epoch_ = 0;
    bool active_ = false;
};

QString waveformCachePathForAudio(const QString& audioFilePath)
{
    const QString absPath = QDir::cleanPath(QFileInfo(audioFilePath).absoluteFilePath());
    const QString unixPath = PathUtils::normalizeToUnix(absPath);
    const QString marker = QStringLiteral("/") + QString::fromLatin1(PathUtils::AUDIO_SOURCES_DIR) + QStringLiteral("/");
    const int markerIndex = unixPath.indexOf(marker, 0, Qt::CaseInsensitive);
    if (markerIndex < 0) {
        return absPath + QStringLiteral(".vkd");
    }

    const QString projectRoot = unixPath.left(markerIndex);
    const QString relativeAudioPath = unixPath.mid(markerIndex + marker.size());
    const QString waveformRoot = PathUtils::getWaveformCacheDir(projectRoot);
    const QString vkdPath = QDir(waveformRoot).filePath(relativeAudioPath + QStringLiteral(".vkd"));
    QDir().mkpath(QFileInfo(vkdPath).absolutePath());
    return vkdPath;
}

} // namespace


const float* WaveformPeakData::getChannelMinPeaks(uint32_t ch) const
{
    if (ch >= numChannels || peaks.empty()) return nullptr;
    return peaks.data() + ch * numPeaks * 2;
}

const float* WaveformPeakData::getChannelMaxPeaks(uint32_t ch) const
{
    if (ch >= numChannels || peaks.empty()) return nullptr;
    return peaks.data() + ch * numPeaks * 2 + 1;
}

const float* WaveformPeakData::getChannelBand(uint32_t ch) const
{
    if (ch >= numChannels || bandData.empty()) return nullptr;
    return bandData.data() + ch * numPeaks * 3;
}

std::vector<float> WaveformPeakData::getMonoMinPeaks() const
{
    std::vector<float> mins(numPeaks, 0.0f);
    if (numChannels == 0 || peaks.empty()) return mins;

    if (numChannels == 1) {
        for (uint32_t i = 0; i < numPeaks; ++i)
            mins[i] = peaks[i * 2];
        return mins;
    }

    const float inv = 1.0f / static_cast<float>(numChannels);
    for (uint32_t i = 0; i < numPeaks; ++i) {
        float sum = 0.0f;
        for (uint32_t ch = 0; ch < numChannels; ++ch)
            sum += peaks[ch * numPeaks * 2 + i * 2];
        mins[i] = sum * inv;
    }
    return mins;
}

std::vector<float> WaveformPeakData::getMonoMaxPeaks() const
{
    std::vector<float> maxs(numPeaks, 0.0f);
    if (numChannels == 0 || peaks.empty()) return maxs;

    if (numChannels == 1) {
        for (uint32_t i = 0; i < numPeaks; ++i)
            maxs[i] = peaks[i * 2 + 1];
        return maxs;
    }

    const float inv = 1.0f / static_cast<float>(numChannels);
    for (uint32_t i = 0; i < numPeaks; ++i) {
        float sum = 0.0f;
        for (uint32_t ch = 0; ch < numChannels; ++ch)
            sum += peaks[ch * numPeaks * 2 + i * 2 + 1];
        maxs[i] = sum * inv;
    }
    return maxs;
}


WaveformCache& WaveformCache::instance()
{
    static WaveformCache cache;
    return cache;
}

void WaveformCache::beginSourceFileChange()
{
    QMutexLocker lock(&waveformWriteMutex());
    if (++waveformChangeDepth != 1)
        return;
    waveformReadEpoch.fetch_add(1, std::memory_order_relaxed);
    for (auto it = pendingWaveforms.cbegin(); it != pendingWaveforms.cend(); ++it)
        deferredWaveforms.insert(it.key(), it->second);
    pendingWaveforms.clear();
    waveformRevisions().clear();
    sharedWaveforms().clear();
    while (waveformActiveReads != 0)
        waveformReadersIdle.wait(&waveformWriteMutex());
}

void WaveformCache::endSourceFileChange(bool regenerate)
{
    QHash<QString, uint32_t> deferred;
    {
        QMutexLocker lock(&waveformWriteMutex());
        if (--waveformChangeDepth != 0)
            return;
        deferred.swap(deferredWaveforms);
    }
    if (regenerate) {
        for (auto it = deferred.cbegin(); it != deferred.cend(); ++it)
            if (QFileInfo(it.key()).isFile())
                generateAsync(it.key(), it.value());
    }
}

QString WaveformCache::cachePath(const QString& audioFilePath)
{
    return waveformCachePathForAudio(audioFilePath);
}

bool WaveformCache::remove(const QString& audioFilePath)
{
    QMutexLocker lock(&waveformWriteMutex());
    waveformRevisions().remove(normalizedAudioPath(audioFilePath));
    pendingWaveforms.remove(normalizedAudioPath(audioFilePath));
    deferredWaveforms.remove(normalizedAudioPath(audioFilePath));
    sharedWaveforms().remove(normalizedAudioPath(audioFilePath));
    QString path = cachePath(audioFilePath);
    if (QFile::exists(path)) {
        return QFile::remove(path);
    }
    return true;
}

QFuture<bool> WaveformCache::generateAsync(const QString& audioFilePath, uint32_t targetPeaks)
{
    const quint64 revision = beginWaveformGeneration(audioFilePath, targetPeaks);
    if (revision == 0) {
        QPromise<bool> result;
        result.start();
        result.addResult(false);
        result.finish();
        return result.future();
    }
    return QtConcurrent::run([audioFilePath, targetPeaks, revision]() {
        return WaveformCache::generateForRevision(audioFilePath, targetPeaks, revision);
    });
}


bool WaveformCache::generate(const QString& audioFilePath, uint32_t targetPeaks)
{
    return generateForRevision(audioFilePath, targetPeaks,
                               beginWaveformGeneration(audioFilePath, targetPeaks));
}

bool WaveformCache::generateForRevision(const QString& audioFilePath,
                                         uint32_t targetPeaks,
                                         quint64 revision)
{
    const WaveformReadScope reading(audioFilePath, revision);
    if (!reading.active())
        return false;
    QFileInfo fileInfo(audioFilePath);
    if (!fileInfo.exists()) {
        qWarning() << "[WaveformCache] Audio file not found:" << audioFilePath;
        return false;
    }

    const uint64_t sourceFileSize = static_cast<uint64_t>(fileInfo.size());
    const int64_t sourceMtime = fileInfo.lastModified().toMSecsSinceEpoch();
    QString ext = fileInfo.suffix().toLower();
    if (!AudioFormatCatalog::containsExtension(ext)) {
        qDebug() << "[WaveformCache] Unsupported format:" << ext;
        return false;
    }

    if (ext != "wav") {
        qint64 fileSizeBytes = fileInfo.size();
        if (fileSizeBytes > 100 * 1024 * 1024) {
            qWarning() << "[WaveformCache] Large file (" << (fileSizeBytes / (1024*1024))
                       << "MB), full decode to memory required:" << audioFilePath;
        }
        return generateFromDecoded(audioFilePath, cachePath(audioFilePath), targetPeaks,
                                   sourceFileSize, sourceMtime, revision);
    }

    drwav wav;
    AudioFileReader file(audioFilePath);
    if (!file.initWav(wav)) {
        qWarning() << "[WaveformCache] Failed to open WAV:" << audioFilePath;
        return false;
    }

    uint64_t totalFrames = wav.totalPCMFrameCount;
    uint32_t channels    = wav.channels;
    uint32_t sampleRate  = wav.sampleRate;

    if (totalFrames == 0 || channels == 0 || sampleRate == 0) {
        drwav_uninit(&wav);
        qWarning() << "[WaveformCache] Invalid WAV metadata:" << audioFilePath;
        return false;
    }

    if (totalFrames < targetPeaks) {
        targetPeaks = static_cast<uint32_t>(totalFrames);
    }
    if (targetPeaks == 0) {
        drwav_uninit(&wav);
        return false;
    }

    uint64_t samplesPerPeak = totalFrames / targetPeaks;
    if (samplesPerPeak == 0) samplesPerPeak = 1;

    WaveformPeakData data;
    data.numPeaks       = targetPeaks;
    data.samplesPerPeak = static_cast<uint32_t>(samplesPerPeak);
    data.numChannels    = channels;
    data.sampleRate     = sampleRate;
    data.durationSec    = static_cast<double>(totalFrames) / sampleRate;
    data.peaks.resize(targetPeaks * 2 * channels);
    data.bandData.resize(targetPeaks * 3 * channels);

    std::vector<BandAnalyzer> analyzers(channels);
    for (auto& a : analyzers)
        a.prepare(sampleRate);

    const size_t maxBufferFrames = std::max(static_cast<size_t>(samplesPerPeak), size_t(8192));
    std::vector<float> readBuffer(maxBufferFrames * channels);
    std::vector<std::vector<float>> channelBuf(channels);
    for (auto& cb : channelBuf)
        cb.reserve(maxBufferFrames);

    for (uint32_t peakIdx = 0; peakIdx < targetPeaks; ++peakIdx) {
        uint64_t framesToRead = samplesPerPeak;
        if (peakIdx == targetPeaks - 1) {
            framesToRead = totalFrames - (uint64_t(peakIdx) * samplesPerPeak);
        }

        std::vector<float> chunkMin(channels,  1.0f);
        std::vector<float> chunkMax(channels, -1.0f);

        for (auto& cb : channelBuf) cb.clear();

        uint64_t framesRemaining = framesToRead;
        while (framesRemaining > 0) {
            if (reading.cancelled()) {
                drwav_uninit(&wav);
                return false;
            }
            uint64_t toRead = std::min(framesRemaining, static_cast<uint64_t>(maxBufferFrames));
            uint64_t framesRead = drwav_read_pcm_frames_f32(&wav, toRead, readBuffer.data());
            if (framesRead == 0) break;

            for (uint64_t f = 0; f < framesRead; ++f) {
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    float sample = readBuffer[f * channels + ch];
                    channelBuf[ch].push_back(sample);
                    if (sample < chunkMin[ch]) chunkMin[ch] = sample;
                    if (sample > chunkMax[ch]) chunkMax[ch] = sample;
                }
            }

            framesRemaining -= framesRead;
        }

        for (uint32_t ch = 0; ch < channels; ++ch) {
            uint32_t peakOff = ch * targetPeaks * 2 + peakIdx * 2;
            data.peaks[peakOff]     = chunkMin[ch];
            data.peaks[peakOff + 1] = chunkMax[ch];

            BandEnergy energy = analyzers[ch].processChunk(
                channelBuf[ch].data(), static_cast<int>(channelBuf[ch].size()));

            uint32_t bandOff = ch * targetPeaks * 3 + peakIdx * 3;
            data.bandData[bandOff]     = energy.low;
            data.bandData[bandOff + 1] = energy.mid;
            data.bandData[bandOff + 2] = energy.high;
        }
    }

    drwav_uninit(&wav);
    file.close();

    return writeVKD(cachePath(audioFilePath), audioFilePath, data,
                    sourceFileSize, sourceMtime, revision);
}


std::shared_ptr<const WaveformPeakData> WaveformCache::loadShared(const QString& audioFilePath)
{
    const QString path = normalizedAudioPath(audioFilePath);
    QMutexLocker lock(&waveformWriteMutex());
    auto& waveforms = sharedWaveforms();
    const QFileInfo sourceInfo(path);
    const QFileInfo cacheInfo(cachePath(path));
    if (!sourceInfo.exists() || !cacheInfo.exists()) {
        waveforms.remove(path);
        return {};
    }

    const qint64 sourceSize = sourceInfo.size();
    const qint64 sourceMtime = sourceInfo.lastModified().toMSecsSinceEpoch();
    const qint64 cacheSize = cacheInfo.size();
    const qint64 cacheMtime = cacheInfo.lastModified().toMSecsSinceEpoch();
    const auto existing = waveforms.constFind(path);
    if (existing != waveforms.cend()
        && existing->sourceSize == sourceSize && existing->sourceMtime == sourceMtime
        && existing->cacheSize == cacheSize && existing->cacheMtime == cacheMtime) {
        if (auto data = existing->data.lock())
            return data;
    }

    waveforms.removeIf([](QHash<QString, SharedWaveform>::iterator it) {
        return it->data.expired();
    });
    auto data = std::make_shared<WaveformPeakData>();
    if (!load(path, *data)) {
        waveforms.remove(path);
        return {};
    }
    waveforms.insert(path, {sourceSize, sourceMtime, cacheSize, cacheMtime, data});
    return data;
}

bool WaveformCache::load(const QString& audioFilePath, WaveformPeakData& outData)
{
    QString vkdPath = cachePath(audioFilePath);
    QFile file(vkdPath);

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QFileInfo sourceInfo(audioFilePath);
    if (!sourceInfo.exists()) {
        return false;
    }

    uint64_t expectedFileSize = static_cast<uint64_t>(sourceInfo.size());
    int64_t  expectedMtime    = sourceInfo.lastModified().toMSecsSinceEpoch();

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);

    uint32_t sectionCount = 0;
    if (!readAndValidateHeader(stream, expectedFileSize, expectedMtime, sectionCount)) {
        return false;
    }

    struct SectionEntry { uint32_t type, offset, size; };
    std::vector<SectionEntry> sections(sectionCount);

    for (uint32_t i = 0; i < sectionCount; ++i) {
        stream >> sections[i].type >> sections[i].offset >> sections[i].size;
        if (stream.status() != QDataStream::Ok) return false;
    }

    const SectionEntry* waveSec = nullptr;
    const SectionEntry* bandSec = nullptr;
    const SectionEntry* midsSec = nullptr;
    for (const auto& s : sections) {
        if (s.type == VKD::SECTION_WAVE) waveSec = &s;
        else if (s.type == VKD::SECTION_BAND) bandSec = &s;
        else if (s.type == VKD::SECTION_MIDSIDE) midsSec = &s;
    }

    if (!waveSec) {
        qWarning() << "[WaveformCache] No WAVE section in VKD:" << vkdPath;
        return false;
    }

    file.seek(waveSec->offset);

    uint32_t numPeaks, samplesPerPeak, numChannels, sampleRate;
    stream >> numPeaks >> samplesPerPeak >> numChannels >> sampleRate;

    double durationSec;
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream >> durationSec;
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);

    if (stream.status() != QDataStream::Ok || numPeaks == 0 || numChannels == 0) {
        return false;
    }

    uint32_t totalPeakFloats = numPeaks * 2 * numChannels;
    uint32_t expectedWaveSize = VKD::WAVE_HEADER_SIZE + totalPeakFloats * static_cast<uint32_t>(sizeof(float));
    if (waveSec->size != expectedWaveSize) {
        qWarning() << "[WaveformCache] WAVE section size mismatch:" << waveSec->size << "vs" << expectedWaveSize;
        return false;
    }

    std::vector<float> peaks(totalPeakFloats);
    for (uint32_t i = 0; i < totalPeakFloats; ++i) {
        stream >> peaks[i];
    }

    if (stream.status() != QDataStream::Ok) return false;

    outData.numPeaks       = numPeaks;
    outData.samplesPerPeak = samplesPerPeak;
    outData.numChannels    = numChannels;
    outData.sampleRate     = sampleRate;
    outData.durationSec    = durationSec;
    outData.peaks          = std::move(peaks);

    if (bandSec) {
        file.seek(bandSec->offset);

        uint32_t bandChannels, bandPeaks;
        stream >> bandChannels >> bandPeaks;

        if (stream.status() == QDataStream::Ok
            && bandChannels == numChannels && bandPeaks == numPeaks)
        {
            uint32_t totalBandFloats = numPeaks * 3 * numChannels;
            uint32_t expectedBandSize = VKD::BAND_HEADER_SIZE + totalBandFloats * static_cast<uint32_t>(sizeof(float));

            if (bandSec->size == expectedBandSize) {
                outData.bandData.resize(totalBandFloats);
                for (uint32_t i = 0; i < totalBandFloats; ++i) {
                    stream >> outData.bandData[i];
                }
                if (stream.status() != QDataStream::Ok) {
                    outData.bandData.clear();
                }
            }
        }
    }

    if (midsSec) {
        file.seek(midsSec->offset);

        uint32_t msChannels, msPeaks;
        stream >> msChannels >> msPeaks;

        if (stream.status() == QDataStream::Ok
            && msChannels == 2 && msPeaks == numPeaks)
        {
            uint32_t totalMsFloats = numPeaks * 4;
            uint32_t expectedMsSize = VKD::BAND_HEADER_SIZE + totalMsFloats * static_cast<uint32_t>(sizeof(float));

            if (midsSec->size == expectedMsSize) {
                outData.midSidePeaks.resize(totalMsFloats);
                for (uint32_t i = 0; i < totalMsFloats; ++i) {
                    stream >> outData.midSidePeaks[i];
                }
                if (stream.status() != QDataStream::Ok) {
                    outData.midSidePeaks.clear();
                }
            }
        }
    }

    return true;
}


WaveformPeakData WaveformCache::readRange(const QString& audioFilePath,
                                         uint64_t startFrame, uint64_t frameCount,
                                         uint32_t targetPeaks)
{
    const WaveformReadScope reading;
    if (!reading.active())
        return {};
    if (frameCount == 0 || targetPeaks == 0)
        return {};

    WaveformPeakData data;
    const auto prepare = [&](uint64_t totalFrames, uint32_t channels, uint32_t sampleRate) {
        if (startFrame >= totalFrames || channels == 0 || sampleRate == 0)
            return false;
        frameCount = std::min(frameCount, totalFrames - startFrame);
        const uint64_t samplesPerPeak = 1 + (frameCount - 1) / targetPeaks;
        data.samplesPerPeak = static_cast<uint32_t>(samplesPerPeak);
        data.numPeaks = static_cast<uint32_t>(1 + (frameCount - 1) / samplesPerPeak);
        data.numChannels = channels;
        data.sampleRate = sampleRate;
        data.durationSec = double(frameCount) / sampleRate;
        data.peaks.resize(size_t(data.numPeaks) * channels * 2);
        for (size_t i = 0; i < data.peaks.size(); i += 2) {
            data.peaks[i] = 1.0f;
            data.peaks[i + 1] = -1.0f;
        }
        return true;
    };
    const auto accumulate = [&](const float* samples, uint64_t first, uint64_t count) {
        for (uint64_t frame = 0; frame < count; ++frame) {
            const uint64_t peak = std::min(uint64_t{data.numPeaks - 1},
                                          (first + frame) / data.samplesPerPeak);
            for (uint32_t channel = 0; channel < data.numChannels; ++channel) {
                const size_t offset = (size_t(channel) * data.numPeaks + peak) * 2;
                const float sample = samples[frame * data.numChannels + channel];
                data.peaks[offset] = std::min(data.peaks[offset], sample);
                data.peaks[offset + 1] = std::max(data.peaks[offset + 1], sample);
            }
        }
    };

    if (QFileInfo(audioFilePath).suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0) {
        drwav wav;
        AudioFileReader file(audioFilePath);
        if (!file.initWav(wav))
            return {};
        if (!prepare(wav.totalPCMFrameCount, wav.channels, wav.sampleRate)
            || !drwav_seek_to_pcm_frame(&wav, startFrame)) {
            drwav_uninit(&wav);
            return {};
        }
        constexpr uint64_t bufferFrames = 8192;
        std::vector<float> samples(bufferFrames * data.numChannels);
        uint64_t read = 0;
        while (read < frameCount) {
            if (reading.cancelled()) {
                drwav_uninit(&wav);
                return {};
            }
            const uint64_t count = drwav_read_pcm_frames_f32(
                &wav, std::min(bufferFrames, frameCount - read), samples.data());
            if (count == 0)
                break;
            accumulate(samples.data(), read, count);
            read += count;
        }
        drwav_uninit(&wav);
        return read == frameCount ? data : WaveformPeakData{};
    }

    std::vector<float> samples;
    int channels = 0;
    int sampleRate = 0;
    if (!HarkenEditor::Bridge::decodeAudioFileInterleaved(
            audioFilePath, samples, channels, sampleRate)
        || reading.cancelled()
        || channels <= 0 || sampleRate <= 0
        || !prepare(samples.size() / channels, channels, sampleRate))
        return {};
    accumulate(samples.data() + startFrame * channels, 0, frameCount);
    return data;
}

bool WaveformCache::isValid(const QString& audioFilePath)
{
    QString vkdPath = cachePath(audioFilePath);
    QFile file(vkdPath);

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QFileInfo sourceInfo(audioFilePath);
    if (!sourceInfo.exists()) {
        return false;
    }

    uint64_t expectedFileSize = static_cast<uint64_t>(sourceInfo.size());
    int64_t  expectedMtime    = sourceInfo.lastModified().toMSecsSinceEpoch();

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    uint32_t sectionCount = 0;
    return readAndValidateHeader(stream, expectedFileSize, expectedMtime, sectionCount);
}


bool WaveformCache::writeVKD(const QString& vkdPath,
                               const QString& audioFilePath,
                               const WaveformPeakData& data,
                               uint64_t sourceFileSize,
                               int64_t sourceMtime,
                               quint64 revision)
{
    const QString tempPath = vkdPath + QStringLiteral(".")
        + QString::number(revision) + QStringLiteral(".tmp");

    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[WaveformCache] Failed to create temp VKD file:" << tempPath;
        return false;
    }

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);

    uint32_t totalPeakFloats = data.numPeaks * 2 * data.numChannels;
    if (data.peaks.size() < totalPeakFloats) {
        qWarning() << "[WaveformCache] writeVKD: peaks array undersized:"
                   << data.peaks.size() << "vs expected" << totalPeakFloats;
        return false;
    }

    bool hasBand = data.hasBandData();
    bool hasMidSide = data.hasMidSideData();
    uint32_t sectionCount = 1u + (hasBand ? 1u : 0u) + (hasMidSide ? 1u : 0u);

    uint32_t waveDataSize = VKD::WAVE_HEADER_SIZE
        + totalPeakFloats * static_cast<uint32_t>(sizeof(float));
    uint32_t bandDataSize = hasBand
        ? (VKD::BAND_HEADER_SIZE + data.numPeaks * 3 * data.numChannels * static_cast<uint32_t>(sizeof(float)))
        : 0;
    uint32_t midsDataSize = hasMidSide
        ? (VKD::BAND_HEADER_SIZE + data.numPeaks * 4 * static_cast<uint32_t>(sizeof(float)))
        : 0;

    uint32_t dirSize = VKD::SECTION_DIR_ENTRY_SIZE * sectionCount;
    uint32_t waveDataOffset = VKD::HEADER_SIZE + dirSize;
    uint32_t bandDataOffset = waveDataOffset + waveDataSize;
    uint32_t midsDataOffset = bandDataOffset + bandDataSize;

    stream << VKD::MAGIC;
    stream << VKD::VERSION;
    stream << static_cast<quint64>(sourceFileSize);
    stream << static_cast<qint64>(sourceMtime);
    stream << sectionCount;
    stream << static_cast<uint32_t>(0);

    stream << VKD::SECTION_WAVE;
    stream << waveDataOffset;
    stream << waveDataSize;

    if (hasBand) {
        stream << VKD::SECTION_BAND;
        stream << bandDataOffset;
        stream << bandDataSize;
    }

    if (hasMidSide) {
        stream << VKD::SECTION_MIDSIDE;
        stream << midsDataOffset;
        stream << midsDataSize;
    }

    stream << data.numPeaks;
    stream << data.samplesPerPeak;
    stream << data.numChannels;
    stream << data.sampleRate;

    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << data.durationSec;
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);

    for (uint32_t i = 0; i < totalPeakFloats; ++i) {
        stream << data.peaks[i];
    }

    if (hasBand) {
        stream << data.numChannels;
        stream << data.numPeaks;

        uint32_t totalBandFloats = data.numPeaks * 3 * data.numChannels;
        for (uint32_t i = 0; i < totalBandFloats; ++i) {
            stream << data.bandData[i];
        }
    }

    if (hasMidSide) {
        stream << static_cast<uint32_t>(2);
        stream << data.numPeaks;

        uint32_t totalMsFloats = data.numPeaks * 4;
        for (uint32_t i = 0; i < totalMsFloats; ++i) {
            stream << data.midSidePeaks[i];
        }
    }

    file.close();

    QMutexLocker lock(&waveformWriteMutex());
    const QFileInfo currentSource(audioFilePath);
    if (!isCurrentWaveformGeneration(audioFilePath, revision)
        || !currentSource.exists()
        || static_cast<uint64_t>(currentSource.size()) != sourceFileSize
        || currentSource.lastModified().toMSecsSinceEpoch() != sourceMtime) {
        QFile::remove(tempPath);
        return false;
    }
    sharedWaveforms().remove(normalizedAudioPath(audioFilePath));
    QFile::remove(vkdPath);
    if (!QFile::rename(tempPath, vkdPath)) {
        qWarning() << "[WaveformCache] Failed to rename temp VKD to final:" << vkdPath;
        QFile::remove(tempPath);
        return false;
    }
    lock.unlock();
    emit instance().cacheReady(audioFilePath);

    qDebug() << "[WaveformCache] Generated VKD v2:" << vkdPath
             << "| peaks:" << data.numPeaks
             << "| channels:" << data.numChannels
             << "| band:" << hasBand
             << "| midside:" << hasMidSide
             << "| duration:" << data.durationSec << "s"
             << "| size:" << QFileInfo(vkdPath).size() << "bytes";

    return true;
}


bool WaveformCache::readAndValidateHeader(QDataStream& stream,
                                            uint64_t expectedFileSize,
                                            int64_t expectedMtime,
                                            uint32_t& outSectionCount)
{
    uint32_t magic, version;
    quint64 storedFileSize;
    qint64  storedMtime;
    uint32_t sectionCount, reserved;

    stream >> magic >> version >> storedFileSize >> storedMtime >> sectionCount >> reserved;

    if (stream.status() != QDataStream::Ok) {
        return false;
    }

    if (magic != VKD::MAGIC) {
        return false;
    }

    if (version > VKD::VERSION) {
        qDebug() << "[WaveformCache] VKD version too new: got" << version << "max supported" << VKD::VERSION;
        return false;
    }

    if (storedFileSize != expectedFileSize) {
        qDebug() << "[WaveformCache] Source file size changed:" << storedFileSize << "vs" << expectedFileSize;
        return false;
    }

    if (storedMtime != expectedMtime) {
        qDebug() << "[WaveformCache] Source file mtime changed:" << storedMtime << "vs" << expectedMtime;
        return false;
    }

    if (sectionCount == 0 || sectionCount > 16) {
        return false;
    }

    outSectionCount = sectionCount;
    return true;
}


bool WaveformCache::generateFromDecoded(const QString& audioFilePath, const QString& vkdPath,
                                         uint32_t targetPeaks,
                                         uint64_t sourceFileSize,
                                         int64_t sourceMtime,
                                         quint64 revision)
{
    std::vector<float> interleaved;
    int decodedChannels = 0;
    int sampleRate = 0;

    if (!HarkenEditor::Bridge::decodeAudioFileInterleaved(audioFilePath, interleaved,
                                                         decodedChannels, sampleRate))
    {
        qWarning() << "[WaveformCache] AudioDecoder failed to decode:" << audioFilePath;
        return false;
    }

    uint32_t channels    = static_cast<uint32_t>(decodedChannels);
    uint64_t totalFrames = channels > 0
        ? static_cast<uint64_t>(interleaved.size() / channels) : 0;
    
    if (totalFrames == 0 || channels == 0 || sampleRate == 0)
    {
        qWarning() << "[WaveformCache] Invalid decoded audio metadata:" << audioFilePath;
        return false;
    }
    
    if (totalFrames < targetPeaks)
        targetPeaks = static_cast<uint32_t>(totalFrames);
    if (targetPeaks == 0)
        return false;
    
    uint64_t samplesPerPeak = totalFrames / targetPeaks;
    if (samplesPerPeak == 0) samplesPerPeak = 1;
    
    WaveformPeakData data;
    data.numPeaks       = targetPeaks;
    data.samplesPerPeak = static_cast<uint32_t>(samplesPerPeak);
    data.numChannels    = channels;
    data.sampleRate     = static_cast<uint32_t>(sampleRate);
    data.durationSec    = static_cast<double>(totalFrames) / sampleRate;
    data.peaks.resize(targetPeaks * 2 * channels);
    data.bandData.resize(targetPeaks * 3 * channels);

    std::vector<BandAnalyzer> analyzers(channels);
    for (auto& a : analyzers)
        a.prepare(static_cast<uint32_t>(sampleRate));

    std::vector<float> chunkSamples;

    for (uint32_t peakIdx = 0; peakIdx < targetPeaks; ++peakIdx)
    {
        uint64_t startFrame = uint64_t(peakIdx) * samplesPerPeak;
        uint64_t endFrame   = (peakIdx == targetPeaks - 1)
                              ? totalFrames
                              : startFrame + samplesPerPeak;
        int chunkLen = static_cast<int>(endFrame - startFrame);

        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            float chMin =  1.0f;
            float chMax = -1.0f;

            chunkSamples.resize(chunkLen);
            for (int i = 0; i < chunkLen; ++i) {
                float s = interleaved[(startFrame + i) * channels + ch];
                chunkSamples[i] = s;
                if (s < chMin) chMin = s;
                if (s > chMax) chMax = s;
            }

            uint32_t peakOff = ch * targetPeaks * 2 + peakIdx * 2;
            data.peaks[peakOff]     = chMin;
            data.peaks[peakOff + 1] = chMax;

            BandEnergy energy = analyzers[ch].processChunk(chunkSamples.data(), chunkLen);
            uint32_t bandOff = ch * targetPeaks * 3 + peakIdx * 3;
            data.bandData[bandOff]     = energy.low;
            data.bandData[bandOff + 1] = energy.mid;
            data.bandData[bandOff + 2] = energy.high;
        }
    }

    return writeVKD(vkdPath, audioFilePath, data, sourceFileSize, sourceMtime,
                    revision);
}


bool WaveformCache::generateMidSide(const QString& audioFilePath, WaveformPeakData& outData)
{
    return generateMidSideForRevision(audioFilePath, outData,
                                      beginWaveformGeneration(audioFilePath));
}

bool WaveformCache::generateMidSideForRevision(const QString& audioFilePath,
                                                WaveformPeakData& outData,
                                                quint64 revision)
{
    const WaveformReadScope reading(audioFilePath, revision);
    if (!reading.active())
        return false;
    if (!load(audioFilePath, outData)) {
        qWarning() << "[WaveformCache] generateMidSide: failed to load existing VKD for" << audioFilePath;
        return false;
    }

    if (outData.numChannels < 2) {
        qWarning() << "[WaveformCache] generateMidSide: not stereo (" << outData.numChannels << " ch)";
        return false;
    }

    if (outData.hasMidSideData()) {
        return true;
    }

    uint32_t numPeaks = outData.numPeaks;
    uint32_t samplesPerPeak = outData.samplesPerPeak;

    QFileInfo fi(audioFilePath);
    QString ext = fi.suffix().toLower();

    outData.midSidePeaks.resize(numPeaks * 4, 0.0f);

    if (ext == QStringLiteral("wav")) {
        drwav wav;
        AudioFileReader file(audioFilePath);
        if (!file.initWav(wav)) {
            qWarning() << "[WaveformCache] generateMidSide: failed to open WAV:" << audioFilePath;
            outData.midSidePeaks.clear();
            return false;
        }

        uint64_t totalFrames = wav.totalPCMFrameCount;
        uint32_t channels = wav.channels;
        if (channels < 2) {
            drwav_uninit(&wav);
            outData.midSidePeaks.clear();
            return false;
        }

        constexpr int kBufFrames = 4096;
        std::vector<float> readBuf(kBufFrames * channels);

        for (uint32_t peakIdx = 0; peakIdx < numPeaks; ++peakIdx) {
            uint64_t startFrame = static_cast<uint64_t>(peakIdx) * samplesPerPeak;
            uint64_t endFrame = (peakIdx == numPeaks - 1)
                ? totalFrames
                : startFrame + samplesPerPeak;
            uint64_t framesToRead = endFrame - startFrame;

            float midMin = 1.0f, midMax = -1.0f;
            float sideMin = 1.0f, sideMax = -1.0f;

            while (framesToRead > 0) {
                if (reading.cancelled()) {
                    drwav_uninit(&wav);
                    outData.midSidePeaks.clear();
                    return false;
                }
                uint64_t chunk = std::min(framesToRead, static_cast<uint64_t>(kBufFrames));
                uint64_t framesRead = drwav_read_pcm_frames_f32(&wav, chunk, readBuf.data());
                if (framesRead == 0) break;

                for (uint64_t f = 0; f < framesRead; ++f) {
                    float L = readBuf[f * channels];
                    float R = readBuf[f * channels + 1];
                    float mid  = (L + R) * 0.5f;
                    float side = (L - R) * 0.5f;
                    if (mid < midMin) midMin = mid;
                    if (mid > midMax) midMax = mid;
                    if (side < sideMin) sideMin = side;
                    if (side > sideMax) sideMax = side;
                }
                framesToRead -= framesRead;
            }

            uint32_t midOff = peakIdx * 2;
            outData.midSidePeaks[midOff]     = midMin;
            outData.midSidePeaks[midOff + 1] = midMax;

            uint32_t sideOff = numPeaks * 2 + peakIdx * 2;
            outData.midSidePeaks[sideOff]     = sideMin;
            outData.midSidePeaks[sideOff + 1] = sideMax;
        }

        drwav_uninit(&wav);
    } else {
        std::vector<float> interleaved;
        int decodedChannels = 0;
        int sampleRate = 0;
        if (!HarkenEditor::Bridge::decodeAudioFileInterleaved(audioFilePath, interleaved,
                                                             decodedChannels, sampleRate)) {
            qWarning() << "[WaveformCache] generateMidSide: failed to decode:" << audioFilePath;
            outData.midSidePeaks.clear();
            return false;
        }

        if (decodedChannels < 2 || reading.cancelled()) {
            outData.midSidePeaks.clear();
            return false;
        }

        const uint32_t channels = static_cast<uint32_t>(decodedChannels);
        uint32_t totalFrames = static_cast<uint32_t>(interleaved.size() / channels);

        for (uint32_t peakIdx = 0; peakIdx < numPeaks; ++peakIdx) {
            uint32_t startFrame = peakIdx * samplesPerPeak;
            uint32_t endFrame = (peakIdx == numPeaks - 1)
                ? totalFrames
                : startFrame + samplesPerPeak;

            float midMin = 1.0f, midMax = -1.0f;
            float sideMin = 1.0f, sideMax = -1.0f;

            for (uint32_t f = startFrame; f < endFrame && f < totalFrames; ++f) {
                float L = interleaved[static_cast<size_t>(f) * channels];
                float R = interleaved[static_cast<size_t>(f) * channels + 1];
                float mid  = (L + R) * 0.5f;
                float side = (L - R) * 0.5f;
                if (mid < midMin) midMin = mid;
                if (mid > midMax) midMax = mid;
                if (side < sideMin) sideMin = side;
                if (side > sideMax) sideMax = side;
            }

            uint32_t midOff = peakIdx * 2;
            outData.midSidePeaks[midOff]     = midMin;
            outData.midSidePeaks[midOff + 1] = midMax;

            uint32_t sideOff = numPeaks * 2 + peakIdx * 2;
            outData.midSidePeaks[sideOff]     = sideMin;
            outData.midSidePeaks[sideOff + 1] = sideMax;
        }
    }

    QString vkdPath = cachePath(audioFilePath);
    const QFileInfo sourceInfo(audioFilePath);
    if (!writeVKD(vkdPath, audioFilePath, outData,
                  static_cast<uint64_t>(sourceInfo.size()),
                  sourceInfo.lastModified().toMSecsSinceEpoch(), revision)) {
        qWarning() << "[WaveformCache] generateMidSide: failed to rewrite VKD:" << vkdPath;
        outData.midSidePeaks.clear();
        return false;
    }

    qDebug() << "[WaveformCache] Generated Mid/Side peaks for:" << audioFilePath
             << "| peaks:" << numPeaks;
    return true;
}

QFuture<bool> WaveformCache::generateMidSideAsync(const QString& audioFilePath, WaveformPeakData& outData)
{
    const quint64 revision = beginWaveformGeneration(audioFilePath);
    return QtConcurrent::run([&outData, audioFilePath, revision]() -> bool {
        return generateMidSideForRevision(audioFilePath, outData, revision);
    });
}


QFuture<int> WaveformCache::rebuildAllAsync(const ProjectData* project,
                                              const QString& audioSourcesDir,
                                              int maxConcurrent)
{
    if (!project || audioSourcesDir.isEmpty()) {
        return QtConcurrent::run([]() { return 0; });
    }

    QStringList audioPaths;
    QSet<uint32_t> sourceIDs;
    for (const auto* binding : project->allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        for (uint32_t sourceID : getOwnedMedia(binding->rootItem.get()))
            sourceIDs.insert(sourceID);
    }
    for (uint32_t sourceID : sourceIDs) {
        const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
        if (!asset) continue;
        QString absPath = asset->absolutePath;
        if (absPath.isEmpty() && !asset->relativePath.isEmpty())
            absPath = QDir(audioSourcesDir).filePath(asset->relativePath);
        if (QFileInfo::exists(absPath)) audioPaths.append(absPath);
    }

    QList<QPair<QString, quint64>> toRebuild;
    for (const QString& path : audioPaths) {
        if (!WaveformCache::isValid(path))
            toRebuild.append({path, beginWaveformGeneration(path, VKD::DEFAULT_NUM_PEAKS)});
    }

    if (toRebuild.isEmpty()) {
        qDebug() << "[WaveformCache] rebuildAllAsync: all" << audioPaths.size()
                 << "VKDs up to date";
        return QtConcurrent::run([]() { return 0; });
    }

    qDebug() << "[WaveformCache] rebuildAllAsync:" << toRebuild.size()
             << "of" << audioPaths.size() << "need rebuild (max"
             << maxConcurrent << "concurrent)";

    return QtConcurrent::run([toRebuild, maxConcurrent]() -> int {
        QSemaphore sem(maxConcurrent);
        std::atomic<int> rebuilt{0};
        QList<QFuture<bool>> futures;

        for (const auto& [path, revision] : toRebuild) {
            sem.acquire();
            futures.append(QtConcurrent::run([path, revision, &sem, &rebuilt]() -> bool {
                bool ok = WaveformCache::generateForRevision(path, VKD::DEFAULT_NUM_PEAKS, revision);
                if (ok)
                    rebuilt.fetch_add(1, std::memory_order_relaxed);
                else
                    qWarning() << "[WaveformCache] rebuildAllAsync: failed to generate VKD for"
                               << path;
                sem.release();
                return ok;
            }));
        }

        for (auto& f : futures)
            f.waitForFinished();

        int count = rebuilt.load(std::memory_order_relaxed);
        qDebug() << "[WaveformCache] rebuildAllAsync: completed," << count
                 << "files rebuilt";
        return count;
    });
}

}
