#ifndef HARKEN_STUDIO_BUILD_STREAM_FORMAT_H
#define HARKEN_STUDIO_BUILD_STREAM_FORMAT_H

#include <harken_studio/ObjectId.h>
#include "harken_studio_build/PackFormat.h"

#include <cstddef>
#include <cstdint>

namespace harkenstudiobuild {
inline constexpr std::uint8_t kStreamMagic[4] = {'V', 'S', 'D', 'A'};

inline constexpr std::uint32_t kStreamFormatVersion = 2;
inline constexpr std::uint32_t kStreamHeaderBytes = 40;
inline constexpr std::uint32_t kStreamTocEntryBytes = 80;
inline constexpr std::uint32_t kStreamEntryCount = 1;
inline constexpr std::uint64_t kStreamPayloadAlign = 64;
inline constexpr std::uint16_t kStreamSampleFormatF32Planar =
    kPackSampleFormatF32Planar;
inline constexpr std::uint32_t kStreamFlagLoopable = kPackFlagLoopable;
inline constexpr std::uint32_t kStreamKnownFlagsMask =
    kStreamFlagLoopable;
inline constexpr std::uint32_t kStreamChunkChecksumBytes = 8;
inline constexpr std::uint32_t kMaxStreamChunkFrames = 1u << 18;

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
    harkenstudio::ObjectId soundId{};
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
inline std::uint64_t StreamChunkBytes(std::uint32_t chunkFrames,
                                         std::uint16_t channelCount) {
    return static_cast<std::uint64_t>(chunkFrames) *
           static_cast<std::uint64_t>(channelCount) * sizeof(float);
}
inline std::uint32_t StreamChunkCount(std::uint64_t frameCount,
                                         std::uint32_t chunkFrames) {
    return static_cast<std::uint32_t>(
        (frameCount + chunkFrames - 1) / chunkFrames);
}
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
inline std::uint64_t StreamChunkFileOffset(std::uint64_t payloadOffset,
                                              std::uint32_t chunkIndex,
                                              std::uint64_t chunkBytes) {
    return payloadOffset + static_cast<std::uint64_t>(chunkIndex) * chunkBytes;
}
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

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_STREAM_FORMAT_H
