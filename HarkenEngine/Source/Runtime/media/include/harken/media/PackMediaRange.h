// Immutable range of one Pack media blob inside an opened Pack artifact.
// The blob is whole-media planar float32 (channel 0's frames, then channel
// 1's, ...). Streaming reads it in fixed chunks whose staged shape matches
// the .stream chunk: chunkFrames floats per channel, final chunk zero padded.
// Per-chunk checksums are runtime integrity evidence computed while the
// artifact is validated; they are not a Pack format field.
#ifndef HARKEN_MEDIA_PACK_MEDIA_RANGE_H
#define HARKEN_MEDIA_PACK_MEDIA_RANGE_H

#include "harken/core/PcmView.h"
#include "harken/media/StreamByteSource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {
class VorbisStreamMedia;

// The streaming compiler's default chunk length. It bounds per-slot staging
// well below kMaxStreamChunkFrames.
inline constexpr std::uint32_t kPackMediaRangeChunkFrames = 4096;

struct PackExternalStream {
    std::shared_ptr<const StreamByteLocator> locator;
    std::uint64_t fileBytes = 0;
};

struct PackMediaRange {
    std::shared_ptr<const PackExternalStream> external;
    std::shared_ptr<const VorbisStreamMedia> vorbis;
    std::shared_ptr<const StreamByteSource> source;
    std::uint64_t blobOffset = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint64_t frameCount = 0;
    std::uint32_t packFlags = 0;
    PcmLoopRegion loop;
    std::uint64_t contentRevision = 0;
    std::uint32_t chunkFrames = 0;
    std::vector<std::uint64_t> chunkChecksums;

    std::uint64_t ChunkBytes() const noexcept {
        return static_cast<std::uint64_t>(chunkFrames) * channelCount * sizeof(float);
    }
};

// Chunk count for a planar blob, or zero when the shape cannot be chunked.
std::uint32_t PackMediaChunkCount(std::uint64_t frameCount, std::uint32_t chunkFrames) noexcept;

// Checks immutable facts without opening external media or reading payload.
bool ValidPackMediaRange(const PackMediaRange& range) noexcept;

// Reads chunk `chunk` of a planar blob into staging (chunkFrames * channels
// floats). Frames past the media end are zero. Validation is the caller's.
StreamReadStatus ReadPlanarMediaChunk(const StreamByteSource& source,
    std::uint64_t blobOffset, std::uint64_t frameCount, std::uint32_t channels,
    std::uint32_t chunkFrames, std::uint32_t chunk, float* staging,
    StreamReadCancellation cancellation);

// Accumulates per-chunk checksums of the staged chunk representation from
// the blob's physical byte order. Every channel visits chunks in order, so
// each chunk's hash sees channel 0's segment (with padding) before channel 1's.
class PlanarChunkChecksumBuilder {
public:
    bool Begin(std::uint64_t frameCount, std::uint32_t channels, std::uint32_t chunkFrames);
    void Add(const std::uint8_t* bytes, std::size_t count);
    bool Complete() const noexcept;
    std::vector<std::uint64_t> Take() noexcept { return std::move(m_states); }

private:
    std::vector<std::uint64_t> m_states;
    std::uint64_t m_channelBytes = 0;
    std::uint64_t m_chunkBytesPerChannel = 0;
    std::uint64_t m_position = 0;
    std::uint64_t m_total = 0;
};

// Reads one chunk of the range into staging (resized to the chunk shape) and
// proves it against the chunk checksum recorded at validation.
bool ReadVerifiedPackMediaChunk(const PackMediaRange& range, std::uint32_t chunk,
                                std::vector<float>& staging);

enum class MediaContentMatch { Equal, Different, LeftFailed, RightFailed };

// Bit-exact equality of two resident planar views, signed zero included.
bool EqualResidentSamples(const PCMView& left, const PCMView& right) noexcept;

// Bit-exact sample equality across resident planes and Pack ranges. Either
// side may be a resident view, a range, or both. LeftFailed / RightFailed
// report an I/O or integrity failure while reading that side's range; an
// already published range can fail because its file was rewritten in place.
MediaContentMatch MatchMediaContent(const PCMView& leftView, const PackMediaRange* leftRange,
                                    const PCMView& rightView, const PackMediaRange* rightRange);

} // namespace harken

#endif // HARKEN_MEDIA_PACK_MEDIA_RANGE_H
