#ifndef HARKEN_CORE_MEDIA_CONTENT_REVISION_H
#define HARKEN_CORE_MEDIA_CONTENT_REVISION_H

#include <harken/ObjectId.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace harken {

inline constexpr std::uint64_t kMediaRevisionFnv1a64OffsetBasis =
    14695981039346656037ull;
inline constexpr std::uint64_t kMediaRevisionFnv1a64Prime =
    1099511628211ull;

class MediaContentRevisionHasherV1 {
public:
    explicit MediaContentRevisionHasherV1(const char* domain,
                                          std::size_t domainBytes) noexcept {
        AddBytes(domain, domainBytes);
    }

    void AddByte(std::uint8_t value) noexcept {
        m_hash ^= value;
        m_hash *= kMediaRevisionFnv1a64Prime;
    }

    void AddBytes(const void* data, std::size_t bytes) noexcept {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            AddByte(p[i]);
        }
    }

    void AddU32(std::uint32_t value) noexcept {
        std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 24),
        };
        AddBytes(bytes, sizeof(bytes));
    }

    void AddU64(std::uint64_t value) noexcept {
        std::uint8_t bytes[8] = {
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 24),
            static_cast<std::uint8_t>(value >> 32),
            static_cast<std::uint8_t>(value >> 40),
            static_cast<std::uint8_t>(value >> 48),
            static_cast<std::uint8_t>(value >> 56),
        };
        AddBytes(bytes, sizeof(bytes));
    }

    std::uint64_t Finish() const noexcept {
        return m_hash == 0 ? 1 : m_hash;
    }

    void AddObjectId(VnObjectId value) noexcept {
        const auto bytes = ObjectIdBytes(value);
        AddBytes(bytes.data(), bytes.size());
    }

private:
    std::uint64_t m_hash = kMediaRevisionFnv1a64OffsetBasis;
};

struct MediaPcmContentRevisionInputV1 {
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint64_t frameCount = 0;
    const float* const* channels = nullptr;
    std::uint32_t stride = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
};

namespace detail {

inline bool MediaPcmRevisionInputValidV1(
    const MediaPcmContentRevisionInputV1& input) noexcept {
    if (input.sampleRate == 0 || input.channelCount == 0 || input.frameCount == 0 ||
        input.channels == nullptr || input.stride == 0) return false;
    if ((input.loopStartFrame != 0 || input.loopEndFrame != 0) &&
        (input.loopStartFrame >= input.loopEndFrame ||
         input.loopEndFrame > input.frameCount)) return false;
    return input.frameCount - 1 <=
        std::numeric_limits<std::size_t>::max() / input.stride;
}

inline MediaContentRevisionHasherV1 BeginMediaPcmRevisionV1(
    const MediaPcmContentRevisionInputV1& input) noexcept {
    static constexpr char kDomain[] = "HARKEN-MEDIA-PCM-V1";
    MediaContentRevisionHasherV1 hash(kDomain, sizeof(kDomain));
    hash.AddU32(input.sampleRate);
    hash.AddU32(input.channelCount);
    hash.AddU64(input.frameCount);
    return hash;
}

inline bool ReadMediaPcmRevisionBitsV1(
    const float* channel, std::uint64_t index, std::uint32_t* outBits) noexcept {
    if (channel == nullptr) return false;
    std::uint32_t bits = 0;
    const float sample = channel[index];
    std::memcpy(&bits, &sample, sizeof(bits));
    if ((bits & 0x7f800000u) == 0x7f800000u) return false;
    if ((bits & 0x7fffffffu) == 0) bits = 0;
    *outBits = bits;
    return true;
}

inline std::uint64_t FinishMediaPcmRevisionV1(
    MediaContentRevisionHasherV1& hash,
    const MediaPcmContentRevisionInputV1& input) noexcept {
    if (input.loopEndFrame != 0) {
        hash.AddU64(input.loopStartFrame);
        hash.AddU64(input.loopEndFrame);
    }
    return hash.Finish();
}

} // namespace detail

inline bool ComputeMediaPcmContentRevisionV1(
    std::uint32_t sampleRate, std::uint32_t channelCount,
    std::uint64_t frameCount, const float* const* channels,
    std::uint32_t stride, std::uint64_t* outRevision,
    std::uint64_t loopStartFrame = 0,
    std::uint64_t loopEndFrame = 0) noexcept {
    const MediaPcmContentRevisionInputV1 input{sampleRate, channelCount, frameCount,
        channels, stride, loopStartFrame, loopEndFrame};
    if (outRevision == nullptr || !detail::MediaPcmRevisionInputValidV1(input))
        return false;
    auto hash = detail::BeginMediaPcmRevisionV1(input);
    for (std::uint64_t frame = 0; frame < frameCount; ++frame) {
        for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
            std::uint32_t bits = 0;
            if (!detail::ReadMediaPcmRevisionBitsV1(
                    channels[channel], frame * stride, &bits)) return false;
            hash.AddU32(bits);
        }
    }
    *outRevision = detail::FinishMediaPcmRevisionV1(hash, input);
    return true;
}

namespace detail {

// Both inputs have validated, equal frame/channel counts. Readers return the
// existing canonical sample bits without changing either hash's byte order.
template <class FirstReader, class SecondReader>
inline void ComputeMediaPcmRevisionPairV1(
    const MediaPcmContentRevisionInputV1& first,
    const MediaPcmContentRevisionInputV1& second,
    FirstReader readFirst, SecondReader readSecond,
    std::uint64_t (&revisions)[2], bool (&valid)[2]) noexcept {
    auto firstHash = detail::BeginMediaPcmRevisionV1(first);
    auto secondHash = detail::BeginMediaPcmRevisionV1(second);
    bool firstValid = true;
    bool secondValid = true;
    for (std::uint64_t frame = 0; frame < first.frameCount; ++frame) {
        for (std::uint32_t channel = 0; channel < first.channelCount; ++channel) {
            std::uint32_t firstBits = 0;
            std::uint32_t secondBits = 0;
            if (firstValid) firstValid = readFirst(channel, frame, &firstBits);
            if (secondValid) secondValid = readSecond(channel, frame, &secondBits);
            if (!firstValid && !secondValid) {
                valid[0] = valid[1] = false;
                return;
            }
            if (firstValid && secondValid) {
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    firstHash.AddByte(static_cast<std::uint8_t>(firstBits >> shift));
                    secondHash.AddByte(static_cast<std::uint8_t>(secondBits >> shift));
                }
            } else if (firstValid) {
                firstHash.AddU32(firstBits);
            } else {
                secondHash.AddU32(secondBits);
            }
        }
    }
    valid[0] = firstValid;
    valid[1] = secondValid;
    if (firstValid) revisions[0] = detail::FinishMediaPcmRevisionV1(firstHash, first);
    if (secondValid) revisions[1] = detail::FinishMediaPcmRevisionV1(secondHash, second);
}

} // namespace detail

// False leaves both output arrays untouched and requests the single-input path.
// True reports each input independently; an invalid input has no output revision.
inline bool TryComputeMediaPcmContentRevisionPairV1(
    const MediaPcmContentRevisionInputV1& first,
    const MediaPcmContentRevisionInputV1& second,
    std::uint64_t (&revisions)[2], bool (&valid)[2]) noexcept {
    if (!detail::MediaPcmRevisionInputValidV1(first) ||
        !detail::MediaPcmRevisionInputValidV1(second) ||
        first.frameCount != second.frameCount ||
        first.channelCount != second.channelCount) return false;
    detail::ComputeMediaPcmRevisionPairV1(first, second,
        [&](std::uint32_t channel, std::uint64_t frame, std::uint32_t* bits) {
            return detail::ReadMediaPcmRevisionBitsV1(
                first.channels[channel], frame * first.stride, bits);
        },
        [&](std::uint32_t channel, std::uint64_t frame, std::uint32_t* bits) {
            return detail::ReadMediaPcmRevisionBitsV1(
                second.channels[channel], frame * second.stride, bits);
        }, revisions, valid);
    return true;
}

struct MediaStreamSemanticHeaderV1 {
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint64_t frameCount = 0;
    std::uint32_t codec = 0;
    std::uint32_t formatVintage = 0;
    std::uint32_t chunkFrames = 0;
    std::uint32_t chunkCount = 0;
};

inline bool ComputeMediaStreamContentRevisionV1(
    const MediaStreamSemanticHeaderV1& header,
    const std::uint64_t* chunkChecksums,
    std::uint64_t* outRevision) noexcept {
    if (header.sampleRate == 0 || header.channelCount == 0 ||
        header.frameCount == 0 || header.formatVintage == 0 ||
        header.chunkFrames == 0 ||
        header.chunkCount == 0 || chunkChecksums == nullptr ||
        outRevision == nullptr) {
        return false;
    }

    static constexpr char kDomain[] = "HARKEN-MEDIA-STREAM-V1";
    MediaContentRevisionHasherV1 hash(kDomain, sizeof(kDomain));
    hash.AddU32(header.sampleRate);
    hash.AddU32(header.channelCount);
    hash.AddU64(header.frameCount);
    hash.AddU32(header.codec);
    hash.AddU32(header.formatVintage);
    hash.AddU32(header.chunkFrames);
    hash.AddU32(header.chunkCount);
    for (std::uint32_t i = 0; i < header.chunkCount; ++i) {
        hash.AddU64(chunkChecksums[i]);
    }

    *outRevision = hash.Finish();
    return true;
}

} // namespace harken

#endif // HARKEN_CORE_MEDIA_CONTENT_REVISION_H
