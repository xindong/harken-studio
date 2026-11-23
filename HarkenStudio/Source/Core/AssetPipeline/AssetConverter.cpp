
#include "AssetConverter.h"
#include "CacheArtifactWriter.h"

#include <QFile>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QDir>
#include <QFileInfo>
#include <QByteArray>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <limits>

#include <CDSPResampler.h>

namespace harken::studio {

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
                                    uint32_t targetSampleRateHz, LoopInfo& loop,
                                    QString* error)
{
    if (channels < 1 || sampleRate <= 0 || samples.empty() || samples.size() % channels != 0) {
        if (error) *error = QStringLiteral("Invalid source samples for resampling");
        return false;
    }
    const auto inputRate = static_cast<uint32_t>(sampleRate);
    const uint64_t inputFrames = samples.size() / static_cast<size_t>(channels);
    if (targetSampleRateHz != 0 && targetSampleRateHz != static_cast<uint32_t>(sampleRate)) {
        std::vector<float> output;
        if (!resample(samples, sampleRate, targetSampleRateHz, channels, output)) {
            if (error) *error = QStringLiteral("Resampling failed");
            return false;
        }
        samples = std::move(output);
        sampleRate = targetSampleRateHz;
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
