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

    void AddBytes(const void* data, std::size_t bytes) noexcept {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            m_hash ^= p[i];
            m_hash *= kMediaRevisionFnv1a64Prime;
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

inline bool ComputeMediaPcmContentRevisionV1(
    std::uint32_t sampleRate, std::uint32_t channelCount,
    std::uint64_t frameCount, const float* const* channels,
    std::uint32_t stride, std::uint64_t* outRevision,
    std::uint64_t loopStartFrame = 0,
    std::uint64_t loopEndFrame = 0) noexcept {
    if (sampleRate == 0 || channelCount == 0 || frameCount == 0 ||
        channels == nullptr || stride == 0 || outRevision == nullptr) {
        return false;
    }
    if ((loopStartFrame != 0 || loopEndFrame != 0)
        && (loopStartFrame >= loopEndFrame || loopEndFrame > frameCount)) {
        return false;
    }
    if (frameCount - 1 >
        std::numeric_limits<std::size_t>::max() / stride) {
        return false;
    }

    static constexpr char kDomain[] = "HARKEN-MEDIA-PCM-V1";
    MediaContentRevisionHasherV1 hash(kDomain, sizeof(kDomain));
    hash.AddU32(sampleRate);
    hash.AddU32(channelCount);
    hash.AddU64(frameCount);

    for (std::uint64_t frame = 0; frame < frameCount; ++frame) {
        for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
            if (channels[channel] == nullptr) {
                return false;
            }
            std::uint32_t bits = 0;
            const float sample = channels[channel][frame * stride];
            std::memcpy(&bits, &sample, sizeof(bits));
            if ((bits & 0x7f800000u) == 0x7f800000u) {
                return false;
            }
            if ((bits & 0x7fffffffu) == 0) {
                bits = 0;
            }
            hash.AddU32(bits);
        }
    }

    if (loopEndFrame != 0) {
        hash.AddU64(loopStartFrame);
        hash.AddU64(loopEndFrame);
    }
    *outRevision = hash.Finish();
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
