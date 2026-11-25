#ifndef HARKEN_PACK_MEDIA_H
#define HARKEN_PACK_MEDIA_H

#include "PackFile.h"

#define HK_PACK_SAMPLE_FORMAT_F32_PLANAR 0u
#define HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT 1u
#define HK_PACK_VORBIS_VERSION 1u
#define HK_PACK_VORBIS_SEEK_GRAIN 4096u
#define HK_PACK_MEDIA_FLAG_LOOPABLE 0x1u
#define HK_PACK_MEDIA_KNOWN_FLAGS HK_PACK_MEDIA_FLAG_LOOPABLE

#pragma pack(push, 1)
// Little-endian blob order: descriptor, identification header, setup header,
// ordered seek entries, then audio packets with a little-endian u32 byte count.
// Frame positions count audible frames per channel. With no loop both bounds
// are zero. The content revision is the FNV-1a checksum of this entire blob
// (zero is represented as one for revision purposes).
typedef struct HkPackVorbisHeader {
    uint32_t version;
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t smallBlockFrames;
    uint16_t largeBlockFrames;
    uint16_t reserved;
    uint64_t frameCount;
    uint64_t prerollFrames;
    uint64_t tailFrames;
    uint64_t loopStartFrame;
    uint64_t loopEndFrame;
    uint32_t identificationBytes;
    uint32_t setupBytes;
    uint32_t seekCount;
    uint32_t seekGrainFrames;
} HkPackVorbisHeader;

// One entry per grid point plus the loop start if it falls between points.
// packetOffset is measured from the beginning of HkPackVorbisHeader to the
// length prefix of the overlap warm-up packet. Discard counts decoded frames
// after warm-up, including any leading trim at audible frame zero.
typedef struct HkPackVorbisSeek {
    uint64_t outputFrame;
    uint64_t packetOffset;
    uint32_t discardFrames;
} HkPackVorbisSeek;

typedef struct HkPackMedia {
    HkPackObjectId soundId;
    uint32_t sampleRate;
    uint16_t channelCount;
    uint16_t sampleFormat;
    uint64_t frameCount;
    uint64_t blobOffset;
    uint64_t blobBytes;
    uint64_t blobChecksum;
    uint32_t flags;
    uint64_t loopStartFrame;
    uint64_t loopEndFrame;
} HkPackMedia;
#pragma pack(pop)
HK_PACK_SIZE(HkPackVorbisHeader, 72);
HK_PACK_OFFSET(HkPackVorbisHeader, version, 0);
HK_PACK_OFFSET(HkPackVorbisHeader, sampleRate, 4);
HK_PACK_OFFSET(HkPackVorbisHeader, channels, 8);
HK_PACK_OFFSET(HkPackVorbisHeader, smallBlockFrames, 10);
HK_PACK_OFFSET(HkPackVorbisHeader, largeBlockFrames, 12);
HK_PACK_OFFSET(HkPackVorbisHeader, reserved, 14);
HK_PACK_OFFSET(HkPackVorbisHeader, frameCount, 16);
HK_PACK_OFFSET(HkPackVorbisHeader, prerollFrames, 24);
HK_PACK_OFFSET(HkPackVorbisHeader, tailFrames, 32);
HK_PACK_OFFSET(HkPackVorbisHeader, loopStartFrame, 40);
HK_PACK_OFFSET(HkPackVorbisHeader, loopEndFrame, 48);
HK_PACK_OFFSET(HkPackVorbisHeader, identificationBytes, 56);
HK_PACK_OFFSET(HkPackVorbisHeader, setupBytes, 60);
HK_PACK_OFFSET(HkPackVorbisHeader, seekCount, 64);
HK_PACK_OFFSET(HkPackVorbisHeader, seekGrainFrames, 68);
HK_PACK_SIZE(HkPackVorbisSeek, 20);
HK_PACK_OFFSET(HkPackVorbisSeek, outputFrame, 0);
HK_PACK_OFFSET(HkPackVorbisSeek, packetOffset, 8);
HK_PACK_OFFSET(HkPackVorbisSeek, discardFrames, 16);
HK_PACK_SIZE(HkPackMedia, 76);
HK_PACK_OFFSET(HkPackMedia, soundId, 0);
HK_PACK_OFFSET(HkPackMedia, sampleRate, 16);
HK_PACK_OFFSET(HkPackMedia, channelCount, 20);
HK_PACK_OFFSET(HkPackMedia, sampleFormat, 22);
HK_PACK_OFFSET(HkPackMedia, frameCount, 24);
HK_PACK_OFFSET(HkPackMedia, blobOffset, 32);
HK_PACK_OFFSET(HkPackMedia, blobBytes, 40);
HK_PACK_OFFSET(HkPackMedia, blobChecksum, 48);
HK_PACK_OFFSET(HkPackMedia, flags, 56);
HK_PACK_OFFSET(HkPackMedia, loopStartFrame, 60);
HK_PACK_OFFSET(HkPackMedia, loopEndFrame, 68);

#endif
