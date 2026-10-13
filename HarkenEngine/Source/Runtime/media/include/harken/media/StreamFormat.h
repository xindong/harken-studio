// .stream container v1 byte layout: the single definition point shared
// by the engine-side streaming reader and the compiler tool (packc).
//
// Same header family as .pack: fixed little-endian with NO implicit
// padding, fields serialized one at a time through explicit byte stores
// (never struct overlays), FNV-1a 64 checksums, and a Header -> TOC ->
// payload file shape. The in-memory forms below are plain working copies;
// only the Serialize/Parse helpers define bytes.
//
// Where .pack packs many whole-resident entries, .stream carries
// ONE streamed sound (v1 fixes entryCount to 1; the count field exists so
// the layout never changes if that ever loosens). The payload is framed
// into fixed-size chunks of chunkFrames frames each so a seek to any
// source frame is pure arithmetic — chunk index plus intra-chunk frame
// offset — with no scan and no per-chunk headers. Every chunk occupies
// exactly the same byte span; the final chunk is zero-padded to full size,
// which keeps both the seek math and the tool output deterministic.
//
// Within a chunk, samples are planar: channel 0's chunkFrames floats, then
// channel 1's, and so on — the shape the render side consumes, so the
// streaming thread never re-interleaves.
//
// Integrity is split by who reads what: the header + TOC + chunk-checksum
// array are covered by tocChecksum and verified once at registration; each
// payload chunk carries its own FNV-1a 64 in the checksum array and is
// verified by the streaming thread as that chunk is read.
#ifndef HARKEN_MEDIA_STREAM_FORMAT_H
#define HARKEN_MEDIA_STREAM_FORMAT_H

#include <harken/ObjectId.h>
#include "harken/media/PackFormat.h"

#include <cstddef>
#include <cstdint>

namespace harken {

// Header magic: the four bytes 'V' 'S' 'D' 'A' at file offset 0.
inline constexpr std::uint8_t kStreamMagic[4] = {'V', 'S', 'D', 'A'};

inline constexpr std::uint32_t kStreamFormatVersion = 2;

// Serialized sizes. Header: magic(4) + formatVersion(4) + headerBytes(4) +
// fileBytes(8) + entryCount(4) + tocOffset(4) + chunkFrames(4) +
// tocChecksum(8).
// TOC entry: soundId(8) + sampleRate(4) + channelCount(2) + sampleFormat(2)
// + frameCount(8) + payloadOffset(8) + payloadBytes(8) +
// chunkChecksumOffset(8) + chunkCount(4) + flags(4) + loopStartFrame(8) +
// loopEndFrame(8).
inline constexpr std::uint32_t kStreamHeaderBytes = 40;
inline constexpr std::uint32_t kStreamTocEntryBytes = 80;

// v1 admits exactly one streamed sound per file; a reader refuses any
// other entryCount.
inline constexpr std::uint32_t kStreamEntryCount = 1;

// The payload region starts on this alignment within the file, matching
// the .pack blob discipline so tool output stays uniformly shaped.
inline constexpr std::uint64_t kStreamPayloadAlign = 64;

// Sample formats and flags reuse the .pack vocabulary: only canonical
// float32 planar exists in v1, and only the loopable flag is defined.
inline constexpr std::uint16_t kStreamSampleFormatF32Planar =
    kPackSampleFormatF32Planar;
inline constexpr std::uint32_t kStreamFlagLoopable = kPackFlagLoopable;
inline constexpr std::uint32_t kStreamKnownFlagsMask =
    kStreamFlagLoopable;

// Each per-chunk checksum is one FNV-1a 64 stored little-endian.
inline constexpr std::uint32_t kStreamChunkChecksumBytes = 8;

// Reader and compiler ceiling for one chunk. The streaming thread stages a
// whole chunk per active slot, so this bound also defines the worst-case
// staging capacity reserved by engine configuration validation. This is 64
// times the compiler default; the next power of two would make the default
// 16-slot configuration exceed its combined streaming-memory budget.
inline constexpr std::uint32_t kMaxStreamChunkFrames = 1u << 18;

// --- in-memory working forms ---

struct StreamHeader {
    std::uint32_t formatVersion = 0;
    std::uint32_t headerBytes = 0;
    std::uint64_t fileBytes = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t tocOffset = 0;
    std::uint32_t chunkFrames = 0;
    std::uint64_t tocChecksum = 0;
};

struct StreamTocEntry {
    VnObjectId soundId{};
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    std::uint16_t sampleFormat = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t payloadOffset = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t chunkChecksumOffset = 0;
    std::uint32_t chunkCount = 0;
    std::uint32_t flags = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0; // 0 = whole file when loopable
};

// Writes the magic and header fields into out[0..kStreamHeaderBytes).
inline void SerializeStreamHeader(const StreamHeader& h,
                                     std::uint8_t* out) {
    out[0] = kStreamMagic[0];
    out[1] = kStreamMagic[1];
    out[2] = kStreamMagic[2];
    out[3] = kStreamMagic[3];
    StoreLE32(out + 4, h.formatVersion);
    StoreLE32(out + 8, h.headerBytes);
    StoreLE64(out + 12, h.fileBytes);
    StoreLE32(out + 20, h.entryCount);
    StoreLE32(out + 24, h.tocOffset);
    StoreLE32(out + 28, h.chunkFrames);
    StoreLE64(out + 32, h.tocChecksum);
}

// Parses in[0..kStreamHeaderBytes). Returns false when the magic does
// not match; field sanity beyond the magic is the caller's validation.
inline bool ParseStreamHeader(const std::uint8_t* in,
                                 StreamHeader* out) {
    if (in[0] != kStreamMagic[0] || in[1] != kStreamMagic[1] ||
        in[2] != kStreamMagic[2] || in[3] != kStreamMagic[3]) {
        return false;
    }
    out->formatVersion = LoadLE32(in + 4);
    out->headerBytes = LoadLE32(in + 8);
    out->fileBytes = LoadLE64(in + 12);
    out->entryCount = LoadLE32(in + 20);
    out->tocOffset = LoadLE32(in + 24);
    out->chunkFrames = LoadLE32(in + 28);
    out->tocChecksum = LoadLE64(in + 32);
    return true;
}

// Writes one TOC entry into out[0..kStreamTocEntryBytes).
inline void SerializeStreamTocEntry(const StreamTocEntry& e,
                                       std::uint8_t* out) {
    StoreObjectId(out + 0, e.soundId);
    StoreLE32(out + 16, e.sampleRate);
    StoreLE16(out + 20, e.channelCount);
    StoreLE16(out + 22, e.sampleFormat);
    StoreLE64(out + 24, e.frameCount);
    StoreLE64(out + 32, e.payloadOffset);
    StoreLE64(out + 40, e.payloadBytes);
    StoreLE64(out + 48, e.chunkChecksumOffset);
    StoreLE32(out + 56, e.chunkCount);
    StoreLE32(out + 60, e.flags);
    StoreLE64(out + 64, e.loopStartFrame);
    StoreLE64(out + 72, e.loopEndFrame);
}

// Parses in[0..kStreamTocEntryBytes). Every bit pattern parses; field
// validation is the caller's job.
inline StreamTocEntry ParseStreamTocEntry(const std::uint8_t* in) {
    StreamTocEntry e;
    e.soundId = LoadObjectId(in + 0);
    e.sampleRate = LoadLE32(in + 16);
    e.channelCount = LoadLE16(in + 20);
    e.sampleFormat = LoadLE16(in + 22);
    e.frameCount = LoadLE64(in + 24);
    e.payloadOffset = LoadLE64(in + 32);
    e.payloadBytes = LoadLE64(in + 40);
    e.chunkChecksumOffset = LoadLE64(in + 48);
    e.chunkCount = LoadLE32(in + 56);
    e.flags = LoadLE32(in + 60);
    e.loopStartFrame = LoadLE64(in + 64);
    e.loopEndFrame = LoadLE64(in + 72);
    return e;
}

// --- chunk geometry and seek math ---
// All pure arithmetic over the header/TOC facts; both the tool (when
// laying the file out) and the streaming reader (when seeking) call these
// so the two sides can never disagree on where a frame lives.

// Byte span of every chunk: chunkFrames frames across all channels of
// float32, planar within the chunk. The final chunk is zero-padded to
// exactly this size.
inline std::uint64_t StreamChunkBytes(std::uint32_t chunkFrames,
                                         std::uint16_t channelCount) {
    return static_cast<std::uint64_t>(chunkFrames) *
           static_cast<std::uint64_t>(channelCount) * sizeof(float);
}

// Number of chunks covering frameCount frames (last one possibly partial).
inline std::uint32_t StreamChunkCount(std::uint64_t frameCount,
                                         std::uint32_t chunkFrames) {
    return static_cast<std::uint32_t>(
        (frameCount + chunkFrames - 1) / chunkFrames);
}

// A source-frame position decomposed against the fixed chunk grid: the
// chunk that holds the frame and the frame's offset within that chunk.
// This is the whole loop-seek story — a wrap to loopStartFrame reads the
// containing chunk and starts consuming at frameInChunk.
struct StreamSeek {
    std::uint32_t chunkIndex = 0;
    std::uint32_t frameInChunk = 0;
};

inline StreamSeek StreamSeekToFrame(std::uint64_t frame,
                                          std::uint32_t chunkFrames) {
    StreamSeek s;
    s.chunkIndex = static_cast<std::uint32_t>(frame / chunkFrames);
    s.frameInChunk = static_cast<std::uint32_t>(frame % chunkFrames);
    return s;
}

// Absolute file offset of a chunk's first byte.
inline std::uint64_t StreamChunkFileOffset(std::uint64_t payloadOffset,
                                              std::uint32_t chunkIndex,
                                              std::uint64_t chunkBytes) {
    return payloadOffset + static_cast<std::uint64_t>(chunkIndex) * chunkBytes;
}

// Frames of real (non-padding) material in a chunk: chunkFrames for every
// chunk but possibly fewer in the last.
inline std::uint32_t StreamChunkValidFrames(std::uint32_t chunkIndex,
                                               std::uint32_t chunkFrames,
                                               std::uint64_t frameCount) {
    const std::uint64_t chunkStart =
        static_cast<std::uint64_t>(chunkIndex) * chunkFrames;
    if (chunkStart >= frameCount) {
        return 0;
    }
    const std::uint64_t remaining = frameCount - chunkStart;
    return remaining >= chunkFrames ? chunkFrames
                                    : static_cast<std::uint32_t>(remaining);
}

} // namespace harken

#endif // HARKEN_MEDIA_STREAM_FORMAT_H
