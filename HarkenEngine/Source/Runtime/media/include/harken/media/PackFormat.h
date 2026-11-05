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
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace harken {


inline constexpr std::uint32_t kPackFormatVersion = HARKEN_PACK_SCHEMA_VERSION;

inline constexpr bool SupportedPackFormatVersion(std::uint32_t version) noexcept {
    return version == kPackFormatVersion;
}

// Serialized sizes. Header: magic(4) + formatVersion(4) + headerBytes(4) +
// fileBytes(8) + entryCount(4) + tocOffset(4) + tocChecksum(8).
// TOC entry: soundId(8) + sampleRate(4) + channelCount(2) + sampleFormat(2)
// + frameCount(8) + blobOffset(8) + blobBytes(8) + blobChecksum(8) +
// flags(4) + loopStartFrame(8) + loopEndFrame(8).

// The end offset at entryCount is also the complete TOC byte length.



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

// Interleave two independent checksums without changing either byte sequence.
inline std::array<std::uint64_t, 2> Fnv1a64Pair(
    const void* firstData, std::size_t firstBytes,
    const void* secondData, std::size_t secondBytes,
    std::uint64_t firstHash = kFnv1a64OffsetBasis,
    std::uint64_t secondHash = kFnv1a64OffsetBasis) {
    const auto* first = static_cast<const unsigned char*>(firstData);
    const auto* second = static_cast<const unsigned char*>(secondData);
    const std::size_t common = std::min(firstBytes, secondBytes);
    for (std::size_t i = 0; i < common; ++i) {
        firstHash = (firstHash ^ first[i]) * kFnv1a64Prime;
        secondHash = (secondHash ^ second[i]) * kFnv1a64Prime;
    }
    if (firstBytes != common)
        firstHash = Fnv1a64(first + common, firstBytes - common, firstHash);
    if (secondBytes != common)
        secondHash = Fnv1a64(second + common, secondBytes - common, secondHash);
    return {firstHash, secondHash};
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

// Writes the magic and header fields into out[0..kPackMediaFileHeaderBytes).


// Parses in[0..kPackMediaFileHeaderBytes). Returns false when the magic does
// not match; field sanity beyond the magic is the caller's validation.


// Writes one TOC entry into out[0..kPackMediaBytes).


// Parses in[0..kPackMediaBytes). Every bit pattern parses; field
// validation is the caller's job.


} // namespace harken

#endif // HARKEN_MEDIA_PACK_FORMAT_H
