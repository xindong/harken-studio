
#pragma once

#include <cstdint>

namespace VNX {


static constexpr uint8_t IPC_BINARY_MAGIC   = 0x56;
static constexpr uint8_t IPC_BINARY_VERSION = 3;


enum class IPCBinaryCommandType : uint16_t
{
    SetPosition     = 0x02,
    GetMixerLevels  = 0x04,
    SetMixerVolume  = 0x05,
    GetVoiceSnapshot= 0x06,
    StopAll         = 0x07,
    SetSwitch       = 0x08,

    MixerLevelsResp     = 0x84,
    VoiceSnapshotResp   = 0x86,

    Batch           = 0xFF,
};

enum class IPCBinaryStatus : uint16_t
{
    Ok = 0,
    Error = 1,
    InvalidState = 2,
};


#pragma pack(push, 1)

struct IPCBinaryHeader
{
    uint8_t  magic;
    uint8_t  version;
    uint16_t commandType;
    uint32_t payloadSize;
    uint32_t sequenceId;
};

static_assert(sizeof(IPCBinaryHeader) == 12, "IPCBinaryHeader must be 12 bytes");

struct IPCBinaryResponse
{
    uint8_t  magic;
    uint8_t  version;
    uint16_t status;
    uint32_t payloadSize;
    uint32_t sequenceId;
};

static_assert(sizeof(IPCBinaryResponse) == 12, "IPCBinaryResponse must be 12 bytes");


struct IPCSetPositionPayload
{
    uint32_t objectID;
    float    x, y, z;
};

static_assert(sizeof(IPCSetPositionPayload) == 16, "IPCSetPositionPayload must be 16 bytes");

struct IPCSetMixerVolumePayload
{
    uint8_t busId[16];
    float    volumeDb;
};

static_assert(sizeof(IPCSetMixerVolumePayload) == 20, "IPCSetMixerVolumePayload must be 20 bytes");

struct IPCSetSwitchPayload
{
    uint32_t groupNameHash;
    uint32_t stateNameHash;
};

static_assert(sizeof(IPCSetSwitchPayload) == 8, "IPCSetSwitchPayload must be 8 bytes");


struct IPCBusLevelEntry
{
    uint8_t busId[16];
    float    peakL, peakR;
    float    rmsL,  rmsR;
    uint8_t  muted;
    uint8_t  soloed;
    uint8_t  pad[2];
};

static_assert(sizeof(IPCBusLevelEntry) == 36, "IPCBusLevelEntry must be 36 bytes");

struct IPCBatchHeader
{
    uint16_t commandCount;
    uint16_t pad;
};

static_assert(sizeof(IPCBatchHeader) == 4, "IPCBatchHeader must be 4 bytes");

#pragma pack(pop)

}
