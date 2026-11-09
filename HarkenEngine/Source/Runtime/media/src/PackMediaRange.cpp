#include "harken/media/PackMediaRange.h"

#include "harken/media/PackFormat.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

namespace harken {

std::uint32_t PackMediaChunkCount(std::uint64_t frameCount, std::uint32_t chunkFrames) noexcept {
    if (frameCount == 0 || chunkFrames == 0) return 0;
    const std::uint64_t chunks = frameCount / chunkFrames + (frameCount % chunkFrames != 0);
    return chunks > std::numeric_limits<std::uint32_t>::max() ? 0 : static_cast<std::uint32_t>(chunks);
}

StreamReadStatus ReadPlanarMediaChunk(const StreamByteSource& source,
    std::uint64_t blobOffset, std::uint64_t frameCount, std::uint32_t channels,
    std::uint32_t chunkFrames, std::uint32_t chunk, float* staging,
    StreamReadCancellation cancellation) {
    const std::uint64_t first = static_cast<std::uint64_t>(chunk) * chunkFrames;
    if (staging == nullptr || channels == 0 || chunkFrames == 0 || first >= frameCount ||
        frameCount > std::numeric_limits<std::uint64_t>::max() / sizeof(float) / channels)
        return StreamReadStatus::IoError;
    const auto valid = static_cast<std::size_t>(std::min<std::uint64_t>(chunkFrames, frameCount - first));
    const std::uint64_t channelBytes = frameCount * sizeof(float);
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        float* destination = staging + static_cast<std::size_t>(ch) * chunkFrames;
        const std::uint64_t offset = blobOffset + ch * channelBytes + first * sizeof(float);
        if (offset < blobOffset) return StreamReadStatus::IoError;
        const auto status = ReadStreamBytes(source, offset, destination, valid * sizeof(float), cancellation);
        if (status != StreamReadStatus::Ok) return status;
        if (valid < chunkFrames)
            std::memset(destination + valid, 0, (chunkFrames - valid) * sizeof(float));
    }
    return StreamReadStatus::Ok;
}

bool PlanarChunkChecksumBuilder::Begin(std::uint64_t frameCount, std::uint32_t channels,
                                       std::uint32_t chunkFrames) {
    const auto chunks = PackMediaChunkCount(frameCount, chunkFrames);
    if (chunks == 0 || channels == 0 ||
        frameCount > std::numeric_limits<std::uint64_t>::max() / sizeof(float) / channels)
        return false;
    m_states.assign(chunks, kFnv1a64OffsetBasis);
    m_channelBytes = frameCount * sizeof(float);
    m_chunkBytesPerChannel = static_cast<std::uint64_t>(chunkFrames) * sizeof(float);
    m_position = 0;
    m_total = m_channelBytes * channels;
    return true;
}

void PlanarChunkChecksumBuilder::Add(const std::uint8_t* bytes, std::size_t count) {
    static constexpr std::uint8_t kZeros[256] = {};
    while (count != 0 && m_position < m_total) {
        const std::uint64_t inChannel = m_position % m_channelBytes;
        const auto chunk = static_cast<std::size_t>(inChannel / m_chunkBytesPerChannel);
        const std::uint64_t chunkEnd = std::min(m_channelBytes, (chunk + 1) * m_chunkBytesPerChannel);
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(count, chunkEnd - inChannel));
        auto& state = m_states[chunk];
        state = Fnv1a64(bytes, take, state);
        bytes += take;
        count -= take;
        m_position += take;
        if (inChannel + take == m_channelBytes) {
            for (std::uint64_t pad = (chunk + 1) * m_chunkBytesPerChannel - m_channelBytes; pad != 0;) {
                const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(pad, sizeof(kZeros)));
                state = Fnv1a64(kZeros, step, state);
                pad -= step;
            }
        }
    }
}

bool PlanarChunkChecksumBuilder::Complete() const noexcept {
    return !m_states.empty() && m_position == m_total;
}

bool ReadVerifiedPackMediaChunk(const PackMediaRange& range, std::uint32_t chunk,
                       std::vector<float>& staging) {
    staging.resize(static_cast<std::size_t>(range.chunkFrames) * range.channelCount);
    return ReadPlanarMediaChunk(*range.source, range.blobOffset, range.frameCount,
               range.channelCount, range.chunkFrames, chunk, staging.data(), {}) ==
               StreamReadStatus::Ok &&
           Fnv1a64(staging.data(), static_cast<std::size_t>(range.ChunkBytes())) ==
               range.chunkChecksums[chunk];
}


bool EqualResidentSamples(const PCMView& left, const PCMView& right) noexcept {
    if (left.encoded != nullptr || right.encoded != nullptr)
        return left.encoded != nullptr && left.encoded == right.encoded;
    assert(left.stride == 1 && right.stride == 1);
    if (right.sampleFormat == PcmSampleFormat::Float32) {
        for (std::uint32_t ch = 0; ch < left.channelCount; ++ch) {
            if (left.sampleFormat == PcmSampleFormat::Float32) {
                if (std::memcmp(left.channels[ch], right.channels[ch],
                                static_cast<std::size_t>(left.frames) * sizeof(float)) != 0)
                    return false;
            } else {
                for (std::uint64_t frame = 0; frame < left.frames; ++frame) {
                    const float sample = left.ReadSample(ch, frame);
                    if (std::memcmp(&sample, right.SamplePtr(ch, frame), sizeof(float)) != 0)
                        return false;
                }
            }
        }
        return true;
    }
    if (left.sampleFormat == PcmSampleFormat::Float32) return EqualResidentSamples(right, left);
    for (std::uint32_t ch = 0; ch < left.channelCount; ++ch) {
        if (std::memcmp(left.channels[ch], right.channels[ch],
                        static_cast<std::size_t>(left.frames) * sizeof(std::int16_t)) != 0)
            return false;
    }
    return true;
}

// Bit-exact sample equality across resident planes and Pack ranges, signed
// zero included. Facts, revisions and chunk checksums only reject.
MediaContentMatch MatchMediaContent(const PCMView& leftResident, const PackMediaRange* leftRange,
                          const PCMView& rightResident, const PackMediaRange* rightRange) {
    // When both sides carry a range, either may be streamed later, so compare
    // the ranges themselves; resident samples must not vouch for a range whose
    // file changed in place.
    const bool bothRanges = leftRange != nullptr && rightRange != nullptr;
    const PCMView leftView = bothRanges ? PCMView{} : leftResident;
    const PCMView rightView = bothRanges ? PCMView{} : rightResident;
    if (leftView.Valid() && rightView.Valid())
        return EqualResidentSamples(leftView, rightView) ? MediaContentMatch::Equal : MediaContentMatch::Different;
    if (leftView.encoded != nullptr || rightView.encoded != nullptr)
        return MediaContentMatch::Different;
    if (!leftView.Valid() && leftRange == nullptr) return MediaContentMatch::Different;
    if (!rightView.Valid() && rightRange == nullptr) return MediaContentMatch::Different;
    const PCMView* view = leftView.Valid() ? &leftView : rightView.Valid() ? &rightView : nullptr;
    const PackMediaRange& range = leftView.Valid() ? *rightRange : *leftRange;
    const PackMediaRange* other = view == nullptr ? (&range == leftRange ? rightRange : leftRange) : nullptr;
    if (view != nullptr) {
        if (view->sampleRate != range.sampleRate || view->channelCount != range.channelCount ||
            view->frames != range.frameCount || view->loop.startFrame != range.loop.startFrame ||
            view->loop.endFrame != range.loop.endFrame ||
            (view->contentRevision != 0 && view->contentRevision != range.contentRevision))
            return MediaContentMatch::Different;
    } else {
        if (other->sampleRate != range.sampleRate || other->channelCount != range.channelCount ||
            other->frameCount != range.frameCount || other->loop.startFrame != range.loop.startFrame ||
            other->loop.endFrame != range.loop.endFrame ||
            other->contentRevision != range.contentRevision || other->chunkFrames != range.chunkFrames)
            return MediaContentMatch::Different;
        if (other->source == range.source && other->blobOffset == range.blobOffset)
            return MediaContentMatch::Equal;
        if (other->chunkChecksums != range.chunkChecksums) return MediaContentMatch::Different;
    }
    // Only a range-only side is read. When both are, `range` is the left one.
    const auto rangeFailed = leftView.Valid() ? MediaContentMatch::RightFailed : MediaContentMatch::LeftFailed;
    std::vector<float> staging;
    std::vector<float> otherStaging;
    for (std::uint32_t chunk = 0; chunk < range.chunkChecksums.size(); ++chunk) {
        if (!ReadVerifiedPackMediaChunk(range, chunk, staging)) return rangeFailed;
        if (other != nullptr) {
            if (!ReadVerifiedPackMediaChunk(*other, chunk, otherStaging)) return MediaContentMatch::RightFailed;
            if (std::memcmp(staging.data(), otherStaging.data(), staging.size() * sizeof(float)) != 0)
                return MediaContentMatch::Different;
            continue;
        }
        const std::uint64_t first = static_cast<std::uint64_t>(chunk) * range.chunkFrames;
        const auto valid = std::min<std::uint64_t>(range.chunkFrames, range.frameCount - first);
        for (std::uint32_t ch = 0; ch < range.channelCount; ++ch) {
            const float* samples = staging.data() + static_cast<std::size_t>(ch) * range.chunkFrames;
            for (std::uint64_t frame = 0; frame < valid; ++frame) {
                const float sample = view->ReadSample(ch, first + frame);
                if (std::memcmp(&sample, samples + frame, sizeof(float)) != 0)
                    return MediaContentMatch::Different;
            }
        }
    }
    return MediaContentMatch::Equal;
}

} // namespace harken
