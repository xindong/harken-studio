#ifndef HARKEN_STUDIO_BUILD_PACK_FORMAT_H
#define HARKEN_STUDIO_BUILD_PACK_FORMAT_H

#include "harken_studio_build/PublicValues.h"
#include <harken_pack/HarkenPack.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace harkenstudiobuild {

inline constexpr std::uint64_t kFnv1a64OffsetBasis = HK_PACK_FNV1A64_OFFSET_BASIS;
inline constexpr std::uint64_t kFnv1a64Prime = HK_PACK_FNV1A64_PRIME;

inline std::uint64_t Fnv1a64(const void* data, std::size_t bytes,
                             std::uint64_t hash = kFnv1a64OffsetBasis) {
    return hkPackFnv1a64(data, bytes, hash);
}

inline void StoreLE16(std::uint8_t* out, std::uint16_t v) {
    out[0] = static_cast<std::uint8_t>(v);
    out[1] = static_cast<std::uint8_t>(v >> 8);
}

inline void StoreLE32(std::uint8_t* out, std::uint32_t v) {
    out[0] = static_cast<std::uint8_t>(v);
    out[1] = static_cast<std::uint8_t>(v >> 8);
    out[2] = static_cast<std::uint8_t>(v >> 16);
    out[3] = static_cast<std::uint8_t>(v >> 24);
}

inline void StoreLE64(std::uint8_t* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline std::uint16_t LoadLE16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(in[0] |
                                      (static_cast<std::uint32_t>(in[1])
                                       << 8));
}

inline std::uint32_t LoadLE32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

inline std::uint64_t LoadLE64(const std::uint8_t* in) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    }
    return v;
}

inline void StoreObjectId(std::uint8_t* out, harkenstudio::ObjectId value) {
    const auto bytes = ObjectIdBytes(value);
    std::memcpy(out, bytes.data(), bytes.size());
}

inline harkenstudio::ObjectId LoadObjectId(const std::uint8_t* in) {
    return ObjectIdFromBytes(std::span<const std::uint8_t, 16>(in, 16));
}

struct PackTocEntry {
    harkenstudio::ObjectId soundId{};
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    std::uint16_t sampleFormat = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t blobOffset = 0;
    std::uint64_t blobBytes = 0;
    std::uint64_t blobChecksum = 0;
    std::uint32_t flags = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0; // 0 = whole file when loopable
};

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_PACK_FORMAT_H
