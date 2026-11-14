#ifndef HARKEN_CORE_STREAM_PACKET_H
#define HARKEN_CORE_STREAM_PACKET_H
#include <cstdint>

namespace harken {
// Internal transport metadata, never a serialized media format. Each record
// carries one complete verified packet and the exact playable frame coverage.
struct StreamPacketHeader {
    std::uint32_t packetBytes = 0;
    std::uint32_t frames = 0;
    std::uint32_t discardFrames = 0;
    std::uint32_t restart = 0;
};
static_assert(sizeof(StreamPacketHeader) == 16);
enum class StreamDecodeResult { Ready, Dry, Invalid };
}
#endif
