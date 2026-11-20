#ifndef HARKEN_PACK_STREAM_H
#define HARKEN_PACK_STREAM_H

#include "PackFile.h"
#include "PackMedia.h"

#define HK_PACK_STREAM_FILE_MAGIC "VSDA"
#define HK_PACK_STREAM_FILE_FORMAT_VERSION 2u
#define HK_PACK_STREAM_FILE_HEADER_BYTES 40u
#define HK_PACK_STREAM_FILE_TOC_ENTRY_BYTES 80u
#define HK_PACK_STREAM_FILE_ENTRY_COUNT 1u
#define HK_PACK_STREAM_FILE_PAYLOAD_ALIGN UINT64_C(64)
#define HK_PACK_STREAM_FILE_KNOWN_FLAGS HK_PACK_MEDIA_FLAG_LOOPABLE
#define HK_PACK_STREAM_FILE_CHUNK_CHECKSUM_BYTES 8u
#define HK_PACK_STREAM_FILE_MAX_CHUNK_FRAMES 262144u

#pragma pack(push, 1)
typedef struct HkPackStreamFileHeader {
    uint8_t magic[4];
    uint32_t formatVersion;
    uint32_t headerBytes;
    uint64_t fileBytes;
    uint32_t entryCount;
    uint32_t tocOffset;
    uint32_t chunkFrames;
    uint64_t tocChecksum;
} HkPackStreamFileHeader;

typedef struct HkPackStreamFileTocEntry {
    HkPackObjectId soundId;
    uint32_t sampleRate;
    uint16_t channelCount;
    uint16_t sampleFormat;
    uint64_t frameCount;
    uint64_t payloadOffset;
    uint64_t payloadBytes;
    uint64_t chunkChecksumOffset;
    uint32_t chunkCount;
    uint32_t flags;
    uint64_t loopStartFrame;
    uint64_t loopEndFrame;
} HkPackStreamFileTocEntry;
#pragma pack(pop)
HK_PACK_SIZE(HkPackStreamFileHeader, HK_PACK_STREAM_FILE_HEADER_BYTES);
HK_PACK_OFFSET(HkPackStreamFileHeader, magic, 0);
HK_PACK_OFFSET(HkPackStreamFileHeader, formatVersion, 4);
HK_PACK_OFFSET(HkPackStreamFileHeader, headerBytes, 8);
HK_PACK_OFFSET(HkPackStreamFileHeader, fileBytes, 12);
HK_PACK_OFFSET(HkPackStreamFileHeader, entryCount, 20);
HK_PACK_OFFSET(HkPackStreamFileHeader, tocOffset, 24);
HK_PACK_OFFSET(HkPackStreamFileHeader, chunkFrames, 28);
HK_PACK_OFFSET(HkPackStreamFileHeader, tocChecksum, 32);
HK_PACK_SIZE(HkPackStreamFileTocEntry, HK_PACK_STREAM_FILE_TOC_ENTRY_BYTES);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, soundId, 0);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, sampleRate, 16);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, channelCount, 20);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, sampleFormat, 22);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, frameCount, 24);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, payloadOffset, 32);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, payloadBytes, 40);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, chunkChecksumOffset, 48);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, chunkCount, 56);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, flags, 60);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, loopStartFrame, 64);
HK_PACK_OFFSET(HkPackStreamFileTocEntry, loopEndFrame, 72);

typedef struct HkPackStreamFileSeek {
    uint32_t chunkIndex;
    uint32_t frameInChunk;
} HkPackStreamFileSeek;

static inline uint64_t hkPackStreamFileChunkBytes(uint32_t chunkFrames, uint16_t channelCount) {
    return (uint64_t)chunkFrames * (uint64_t)channelCount * (uint64_t)sizeof(float);
}

static inline uint32_t hkPackStreamFileChunkCount(uint64_t frameCount, uint32_t chunkFrames) {
    return (uint32_t)((frameCount + chunkFrames - 1) / chunkFrames);
}

static inline HkPackStreamFileSeek hkPackStreamFileSeekToFrame(uint64_t frame, uint32_t chunkFrames) {
    HkPackStreamFileSeek seek;
    seek.chunkIndex = (uint32_t)(frame / chunkFrames);
    seek.frameInChunk = (uint32_t)(frame % chunkFrames);
    return seek;
}

static inline uint64_t hkPackStreamFileChunkOffset(uint64_t payloadOffset, uint32_t chunkIndex,
                                                   uint64_t chunkBytes) {
    return payloadOffset + (uint64_t)chunkIndex * chunkBytes;
}

static inline uint32_t hkPackStreamFileChunkValidFrames(uint32_t chunkIndex, uint32_t chunkFrames,
                                                        uint64_t frameCount) {
    const uint64_t chunkStart = (uint64_t)chunkIndex * chunkFrames;
    if (chunkStart >= frameCount) return 0;
    const uint64_t remaining = frameCount - chunkStart;
    return remaining >= chunkFrames ? chunkFrames : (uint32_t)remaining;
}

#endif
