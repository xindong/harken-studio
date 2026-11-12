#ifndef HARKEN_PACK_BUS_H
#define HARKEN_PACK_BUS_H

#include "PackFile.h"

typedef struct HkPackBus {
    HkPackObjectId busId;
    HkPackObjectId parentBusId;
    float volume;
    float outputBusVolume;
} HkPackBus;
HK_PACK_SIZE(HkPackBus, 40);
HK_PACK_OFFSET(HkPackBus, busId, 0);
HK_PACK_OFFSET(HkPackBus, parentBusId, 16);
HK_PACK_OFFSET(HkPackBus, volume, 32);
HK_PACK_OFFSET(HkPackBus, outputBusVolume, 36);

typedef struct HkPackBusDefinitionBus {
    HkPackObjectId busId;
    HkPackObjectId parentId;
    uint32_t feedsFirstOrdinal;
    float faderDb;
    float outputBusVolumeDb;
    uint32_t faderAfterSlot;
    uint32_t outputLayout;
    uint32_t effectFirst;
    uint32_t effectCount;
    uint32_t downmixFirst;
    uint32_t downmixCount;
    uint32_t reserved0;
} HkPackBusDefinitionBus;
HK_PACK_SIZE(HkPackBusDefinitionBus, 72);
HK_PACK_OFFSET(HkPackBusDefinitionBus, busId, 0);
HK_PACK_OFFSET(HkPackBusDefinitionBus, parentId, 16);
HK_PACK_OFFSET(HkPackBusDefinitionBus, feedsFirstOrdinal, 32);
HK_PACK_OFFSET(HkPackBusDefinitionBus, faderDb, 36);
HK_PACK_OFFSET(HkPackBusDefinitionBus, outputBusVolumeDb, 40);
HK_PACK_OFFSET(HkPackBusDefinitionBus, faderAfterSlot, 44);
HK_PACK_OFFSET(HkPackBusDefinitionBus, outputLayout, 48);
HK_PACK_OFFSET(HkPackBusDefinitionBus, effectFirst, 52);
HK_PACK_OFFSET(HkPackBusDefinitionBus, effectCount, 56);
HK_PACK_OFFSET(HkPackBusDefinitionBus, downmixFirst, 60);
HK_PACK_OFFSET(HkPackBusDefinitionBus, downmixCount, 64);
HK_PACK_OFFSET(HkPackBusDefinitionBus, reserved0, 68);

typedef struct HkPackBusDefinitionCoefficient {
    float value;
} HkPackBusDefinitionCoefficient;
HK_PACK_SIZE(HkPackBusDefinitionCoefficient, 4);
HK_PACK_OFFSET(HkPackBusDefinitionCoefficient, value, 0);

typedef struct HkPackBusDefinitionDownmix {
    HkPackObjectId busId;
    uint32_t sourceLayout;
    uint32_t destinationLayout;
    uint32_t coefficientFirst;
    uint32_t coefficientCount;
    uint64_t reserved0;
} HkPackBusDefinitionDownmix;
HK_PACK_SIZE(HkPackBusDefinitionDownmix, 40);
HK_PACK_OFFSET(HkPackBusDefinitionDownmix, busId, 0);
HK_PACK_OFFSET(HkPackBusDefinitionDownmix, sourceLayout, 16);
HK_PACK_OFFSET(HkPackBusDefinitionDownmix, destinationLayout, 20);
HK_PACK_OFFSET(HkPackBusDefinitionDownmix, coefficientFirst, 24);
HK_PACK_OFFSET(HkPackBusDefinitionDownmix, coefficientCount, 28);
HK_PACK_OFFSET(HkPackBusDefinitionDownmix, reserved0, 32);

typedef struct HkPackBusDefinitionSend {
    HkPackObjectId source;
    HkPackObjectId target;
    float levelDb;
    uint32_t reserved0;
    uint64_t reserved1;
} HkPackBusDefinitionSend;
HK_PACK_SIZE(HkPackBusDefinitionSend, 48);
HK_PACK_OFFSET(HkPackBusDefinitionSend, source, 0);
HK_PACK_OFFSET(HkPackBusDefinitionSend, target, 16);
HK_PACK_OFFSET(HkPackBusDefinitionSend, levelDb, 32);
HK_PACK_OFFSET(HkPackBusDefinitionSend, reserved0, 36);
HK_PACK_OFFSET(HkPackBusDefinitionSend, reserved1, 40);

typedef struct HkPackBusDefinitionSnapshotHeader {
    uint32_t schemaVersion;
    uint32_t storageModelVersion;
    uint32_t headerBytes;
    uint32_t publicationCapabilityVintage;
    uint64_t contentGeneration;
    uint64_t publishedBusFingerprint;
    uint32_t busCount;
    uint32_t sendCount;
    uint32_t effectCount;
    uint32_t effectParameterCount;
    uint32_t downmixCount;
    uint32_t coefficientCount;
    uint64_t busOffset;
    uint64_t sendOffset;
    uint64_t effectOffset;
    uint64_t effectParameterOffset;
    uint64_t downmixOffset;
    uint64_t coefficientOffset;
    uint64_t payloadBytes;
    uint64_t checksum;
    uint64_t compileTargetProfileFingerprint;
    uint32_t profileVintage;
    uint32_t renderSampleRateHz;
    uint32_t mixLayout;
    uint32_t deviceLayout;
    uint64_t runtimeCapabilityMask;
    uint64_t reserved0;
    uint32_t busRowBytes;
    uint32_t sendRowBytes;
    uint32_t downmixRowBytes;
    uint32_t coefficientRowBytes;
    uint64_t reserved1;
} HkPackBusDefinitionSnapshotHeader;
HK_PACK_SIZE(HkPackBusDefinitionSnapshotHeader, 184);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, schemaVersion, 0);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, storageModelVersion, 4);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, headerBytes, 8);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, publicationCapabilityVintage, 12);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, contentGeneration, 16);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, publishedBusFingerprint, 24);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, busCount, 32);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, sendCount, 36);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, effectCount, 40);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, effectParameterCount, 44);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, downmixCount, 48);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, coefficientCount, 52);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, busOffset, 56);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, sendOffset, 64);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, effectOffset, 72);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, effectParameterOffset, 80);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, downmixOffset, 88);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, coefficientOffset, 96);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, payloadBytes, 104);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, checksum, 112);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, compileTargetProfileFingerprint, 120);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, profileVintage, 128);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, renderSampleRateHz, 132);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, mixLayout, 136);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, deviceLayout, 140);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, runtimeCapabilityMask, 144);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, reserved0, 152);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, busRowBytes, 160);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, sendRowBytes, 164);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, downmixRowBytes, 168);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, coefficientRowBytes, 172);
HK_PACK_OFFSET(HkPackBusDefinitionSnapshotHeader, reserved1, 176);

typedef struct HkPackBusEffect {
    HkPackObjectId busId;
    uint32_t slot;
    uint32_t effectType;
    uint32_t descriptorVintage;
    uint32_t bypass;
    uint32_t faderPointGuard;
    uint32_t paramCount;
    uint32_t paramBytes;
    uint32_t reserved44;
    float params[HK_PACK_VOICE_RECIPE_MAX_PARAMS];
    HkPackObjectId effectId;
} HkPackBusEffect;
HK_PACK_SIZE(HkPackBusEffect, 320);
HK_PACK_OFFSET(HkPackBusEffect, busId, 0);
HK_PACK_OFFSET(HkPackBusEffect, slot, 16);
HK_PACK_OFFSET(HkPackBusEffect, effectType, 20);
HK_PACK_OFFSET(HkPackBusEffect, descriptorVintage, 24);
HK_PACK_OFFSET(HkPackBusEffect, bypass, 28);
HK_PACK_OFFSET(HkPackBusEffect, faderPointGuard, 32);
HK_PACK_OFFSET(HkPackBusEffect, paramCount, 36);
HK_PACK_OFFSET(HkPackBusEffect, paramBytes, 40);
HK_PACK_OFFSET(HkPackBusEffect, reserved44, 44);
HK_PACK_OFFSET(HkPackBusEffect, params, 48);
HK_PACK_OFFSET(HkPackBusEffect, effectId, 304);

typedef struct HkPackBusSend {
    HkPackObjectId fromBusId;
    HkPackObjectId toBusId;
    float levelDb;
    uint32_t reserved0;
} HkPackBusSend;
HK_PACK_SIZE(HkPackBusSend, 40);
HK_PACK_OFFSET(HkPackBusSend, fromBusId, 0);
HK_PACK_OFFSET(HkPackBusSend, toBusId, 16);
HK_PACK_OFFSET(HkPackBusSend, levelDb, 32);
HK_PACK_OFFSET(HkPackBusSend, reserved0, 36);

typedef struct HkPackEffectDynamicRoute {
    HkPackObjectId targetBusId;
    uint32_t effectSlot;
    uint32_t routeSlot;
    uint32_t expectedEffectType;
    uint32_t signalForm;
    uint32_t sourceKind;
    uint32_t tap;
    HkPackObjectId sourceId;
    float levelDb;
    uint32_t reserved60;
    uint64_t reserved64;
} HkPackEffectDynamicRoute;
HK_PACK_SIZE(HkPackEffectDynamicRoute, 72);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, targetBusId, 0);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, effectSlot, 16);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, routeSlot, 20);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, expectedEffectType, 24);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, signalForm, 28);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, sourceKind, 32);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, tap, 36);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, sourceId, 40);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, levelDb, 56);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, reserved60, 60);
HK_PACK_OFFSET(HkPackEffectDynamicRoute, reserved64, 64);

#endif
