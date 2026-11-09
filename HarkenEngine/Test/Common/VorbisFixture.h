#pragma once
#include "harken/media/VorbisMedia.h"
#include <cstdint>
#include <vector>

namespace harken::test {
struct VorbisFixture {
    std::vector<std::uint8_t> media;
    // Interleaved samples in Vorbis order, decoded independently by vorbisfile.
    std::vector<float> reference;
};
VorbisFixture MakeVorbisFixture(unsigned channels, bool streaming = false,
    std::uint64_t loopStart = 0, std::uint64_t loopEnd = 0,
    std::uint32_t granularity = 4096);
std::vector<std::uint8_t> WrapVorbisStream(const std::vector<std::uint8_t>& media);
// Reference packet decoding tests the converter's seek metadata before it is
// consumed by the private decoder. Returned samples remain in Vorbis order.
std::vector<float> DecodeReferencePackets(const VorbisMedia& media,
    std::uint64_t frame, std::uint64_t count);
}
