#ifndef HARKEN_PACK_MEDIA_H
#define HARKEN_PACK_MEDIA_H

#include "PackFile.h"

#define HK_PACK_SAMPLE_FORMAT_F32_PLANAR 0u
#define HK_PACK_MEDIA_FLAG_LOOPABLE 0x1u
#define HK_PACK_MEDIA_KNOWN_FLAGS HK_PACK_MEDIA_FLAG_LOOPABLE

#pragma pack(push, 1)
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
