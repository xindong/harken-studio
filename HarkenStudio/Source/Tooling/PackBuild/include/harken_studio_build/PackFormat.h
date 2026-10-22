#ifndef HARKEN_STUDIO_BUILD_PACK_FORMAT_H
#define HARKEN_STUDIO_BUILD_PACK_FORMAT_H

#include "harken_studio_build/PublicValues.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace harkenstudiobuild {
inline constexpr std::uint8_t kPackMagic[4] = {'V', 'S', 'P', 'K'};

inline constexpr std::uint32_t kPackFormatVersion = HARKEN_PACK_SCHEMA_VERSION;
inline constexpr std::uint32_t kPackHeaderBytes = 36;
inline constexpr std::uint32_t kPackTocEntryBytes = 76;
inline constexpr std::uint64_t kPackBlobAlign = 64;
inline constexpr std::uint16_t kPackSampleFormatF32Planar = 0;
inline constexpr std::uint32_t kPackFlagLoopable = 1u << 0;
inline constexpr std::uint32_t kPackKnownFlagsMask =
    kPackFlagLoopable;
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

inline std::uint64_t Fnv1a64(const void* data, std::size_t bytes,
                             std::uint64_t hash = kFnv1a64OffsetBasis) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= static_cast<std::uint64_t>(p[i]);
        hash *= kFnv1a64Prime;
    }
    return hash;
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

struct PackHeader {
    std::uint32_t formatVersion = 0;
    std::uint32_t headerBytes = 0;
    std::uint64_t fileBytes = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t tocOffset = 0;
    std::uint64_t tocChecksum = 0;
};

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
inline void SerializePackHeader(const PackHeader& h,
                                     std::uint8_t* out) {
    out[0] = kPackMagic[0];
    out[1] = kPackMagic[1];
    out[2] = kPackMagic[2];
    out[3] = kPackMagic[3];
    StoreLE32(out + 4, h.formatVersion);
    StoreLE32(out + 8, h.headerBytes);
    StoreLE64(out + 12, h.fileBytes);
    StoreLE32(out + 20, h.entryCount);
    StoreLE32(out + 24, h.tocOffset);
    StoreLE64(out + 28, h.tocChecksum);
}
inline bool ParsePackHeader(const std::uint8_t* in,
                                 PackHeader* out) {
    if (in[0] != kPackMagic[0] || in[1] != kPackMagic[1] ||
        in[2] != kPackMagic[2] || in[3] != kPackMagic[3]) {
        return false;
    }
    out->formatVersion = LoadLE32(in + 4);
    out->headerBytes = LoadLE32(in + 8);
    out->fileBytes = LoadLE64(in + 12);
    out->entryCount = LoadLE32(in + 20);
    out->tocOffset = LoadLE32(in + 24);
    out->tocChecksum = LoadLE64(in + 28);
    return true;
}
inline void SerializePackTocEntry(const PackTocEntry& e,
                                       std::uint8_t* out) {
    StoreObjectId(out + 0, e.soundId);
    StoreLE32(out + 16, e.sampleRate);
    StoreLE16(out + 20, e.channelCount);
    StoreLE16(out + 22, e.sampleFormat);
    StoreLE64(out + 24, e.frameCount);
    StoreLE64(out + 32, e.blobOffset);
    StoreLE64(out + 40, e.blobBytes);
    StoreLE64(out + 48, e.blobChecksum);
    StoreLE32(out + 56, e.flags);
    StoreLE64(out + 60, e.loopStartFrame);
    StoreLE64(out + 68, e.loopEndFrame);
}
inline PackTocEntry ParsePackTocEntry(const std::uint8_t* in) {
    PackTocEntry e;
    e.soundId = LoadObjectId(in + 0);
    e.sampleRate = LoadLE32(in + 16);
    e.channelCount = LoadLE16(in + 20);
    e.sampleFormat = LoadLE16(in + 22);
    e.frameCount = LoadLE64(in + 24);
    e.blobOffset = LoadLE64(in + 32);
    e.blobBytes = LoadLE64(in + 40);
    e.blobChecksum = LoadLE64(in + 48);
    e.flags = LoadLE32(in + 56);
    e.loopStartFrame = LoadLE64(in + 60);
    e.loopEndFrame = LoadLE64(in + 68);
    return e;
}

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_PACK_FORMAT_H
