#ifndef HARKEN_MEDIA_VORBIS_MEDIA_H
#define HARKEN_MEDIA_VORBIS_MEDIA_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {

// Temporary test wire layout. No public Pack codec value accepts this format.
namespace vorbis_test_wire {
inline constexpr std::uint32_t kMediaMagic = 0x30425648;
inline constexpr std::uint32_t kStreamMagic = 0x30545348;
inline constexpr std::uint16_t kVersion = 0;
inline constexpr std::size_t kHeaderBytes = 112;
inline constexpr std::size_t kSeekBytes = 20;
inline constexpr std::size_t kStreamHeaderBytes = 24;
}

struct VorbisSeekPoint {
    std::uint64_t frame = 0;
    std::uint64_t packetOffset = 0;
    std::uint32_t discardFrames = 0;
};

// Parsed metadata borrows immutable bytes from its resource owner. Parsing
// and index allocation happen during preparation, never during rendering.
struct VorbisMedia {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0, blockShort = 0, blockLong = 0;
    std::uint64_t frames = 0, revision = 0;
    std::uint32_t seekGranularity = 0;
    std::uint64_t loopStart = 0, loopEnd = 0;
    std::uint64_t loopStartPacket = 0, loopEndPacket = 0;
    std::uint32_t loopDiscard = 0, loopTrim = 0;
    const std::uint8_t* ident = nullptr;
    const std::uint8_t* setup = nullptr;
    const std::uint8_t* packets = nullptr;
    std::uint32_t identBytes = 0, setupBytes = 0;
    std::uint64_t packetBytes = 0;
    std::uint64_t packetDataOffset = 0, packetChecksum = 0;
    // Segment checksums bind the complete metadata prefix as well as payload.
    std::uint64_t segmentChecksumSeed = 0;
    std::vector<VorbisSeekPoint> seek;
    std::vector<std::uint64_t> segmentChecksums;

    bool Packet(std::uint64_t offset, const std::uint8_t*& data,
                std::uint16_t& bytes) const noexcept;
    const VorbisSeekPoint* Seek(std::uint64_t frame) const noexcept;
    bool VerifySegment(std::size_t index) const noexcept;
};

// Failure leaves output unchanged. Full packet integrity is checked for a
// resident blob; streaming consumers can validate segments as they arrive.
// Header inspection never reads packet payload. A stream must independently
// verify each segment before exposing its packets to the renderer.
std::uint64_t VorbisTestMetadataBytes(const void* header, std::size_t bytes) noexcept;
bool ParseVorbisTestMediaHeaders(const void* prefix, std::size_t prefixBytes,
                                std::uint64_t mediaBytes, VorbisMedia& output);
bool ParseVorbisTestMedia(const void* data, std::size_t bytes, VorbisMedia& output);
bool ParseVorbisTestStream(const void* data, std::size_t bytes, VorbisMedia& output);

} // namespace harken
#endif
