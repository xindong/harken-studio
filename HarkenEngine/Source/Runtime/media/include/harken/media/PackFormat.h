// .pack container v1 byte layout: the single definition point shared
// by the engine-side reader (pack_table) and the compiler tool (packc).
//
// The on-disk format is fixed little-endian with NO implicit padding, so
// fields are serialized one at a time through explicit byte stores instead
// of overlaying structs (a natural-alignment struct would insert padding
// after the 4-byte fields and break the layout). The in-memory forms below
// are plain working copies; only the Serialize/Parse helpers define bytes.
//
// File shape: Header, then the TOC (entryCount fixed-size entries), then
// the blob region. Every blob offset is 64-byte aligned so reference-mode
// registration can point straight into the loaded pack with SIMD-friendly
// addresses.
#ifndef HARKEN_MEDIA_PACK_FORMAT_H
#define HARKEN_MEDIA_PACK_FORMAT_H

#include <harken/ObjectId.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace harken {

// Header magic: the four bytes 'V' 'S' 'P' 'K' at file offset 0.
inline constexpr std::uint8_t kPackMagic[4] = {'V', 'S', 'P', 'K'};

inline constexpr std::uint32_t kPackFormatVersion = HARKEN_PACK_SCHEMA_VERSION;
inline constexpr std::uint32_t kPackFormatVersion3 = 3;
inline constexpr std::uint32_t kPackFormatVersion4 = 4;

inline constexpr bool SupportedPackFormatVersion(std::uint32_t version) noexcept {
    return version == kPackFormatVersion3 || version == kPackFormatVersion4;
}

// Serialized sizes. Header: magic(4) + formatVersion(4) + headerBytes(4) +
// fileBytes(8) + entryCount(4) + tocOffset(4) + tocChecksum(8).
// TOC entry: soundId(8) + sampleRate(4) + channelCount(2) + sampleFormat(2)
// + frameCount(8) + blobOffset(8) + blobBytes(8) + blobChecksum(8) +
// flags(4) + loopStartFrame(8) + loopEndFrame(8).
inline constexpr std::uint32_t kPackHeaderBytes = 36;
inline constexpr std::uint32_t kPackTocEntryBytes = 76;

// Every blob offset is a multiple of this within the file.
inline constexpr std::uint64_t kPackBlobAlign = 64;

// TOC sampleFormat values. Only canonical float32 planar exists in v1; the
// field is reserved for future codecs.
inline constexpr std::uint16_t kPackSampleFormatF32Planar = 0;

// TOC flags. Bits outside the known mask are undefined in v1 and a reader
// refuses them.
inline constexpr std::uint32_t kPackFlagLoopable = 1u << 0;
inline constexpr std::uint32_t kPackKnownFlagsMask =
    kPackFlagLoopable;

// --- FNV-1a 64-bit ---
// Both checksum kinds (per-blob and TOC) use FNV-1a 64: for each byte,
// hash = (hash XOR byte) * prime, starting from the offset basis.
// Public-domain algorithm by Fowler, Noll, and Vo; constants and formula
// from the authors' reference page (isthe.com/chongo/tech/comp/fnv).
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

// --- little-endian field access ---
// Byte-wise on purpose: the file format is little-endian by definition,
// and these helpers make that explicit instead of relying on host order.

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

inline void StoreObjectId(std::uint8_t* out, VnObjectId value) {
    const auto bytes = ObjectIdBytes(value);
    std::memcpy(out, bytes.data(), bytes.size());
}

inline VnObjectId LoadObjectId(const std::uint8_t* in) {
    return ObjectIdFromBytes(std::span<const std::uint8_t, 16>(in, 16));
}

// --- in-memory working forms ---

struct PackHeader {
    std::uint32_t formatVersion = 0;
    std::uint32_t headerBytes = 0;
    std::uint64_t fileBytes = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t tocOffset = 0;
    std::uint64_t tocChecksum = 0;
};

struct PackTocEntry {
    VnObjectId soundId{};
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

// Writes the magic and header fields into out[0..kPackHeaderBytes).
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

// Parses in[0..kPackHeaderBytes). Returns false when the magic does
// not match; field sanity beyond the magic is the caller's validation.
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

// Writes one TOC entry into out[0..kPackTocEntryBytes).
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

// Parses in[0..kPackTocEntryBytes). Every bit pattern parses; field
// validation is the caller's job.
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

} // namespace harken

#endif // HARKEN_MEDIA_PACK_FORMAT_H
