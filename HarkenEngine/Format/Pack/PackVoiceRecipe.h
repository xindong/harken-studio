#ifndef HARKEN_PACK_VOICE_RECIPE_H
#define HARKEN_PACK_VOICE_RECIPE_H

#include "PackFile.h"

#define HK_PACK_VOICE_RECIPE_MAX_INSTALLS 8u
#define HK_PACK_VOICE_RECIPE_MAX_BINDINGS 8u
#define HK_PACK_MAX_VOICE_SEND_SLOTS 8u
#define HK_PACK_MAX_VOICE_FX_SLOTS 8u
#define HK_PACK_VOICE_TONE_LOWPASS 0x1u
#define HK_PACK_VOICE_TONE_HIGHPASS 0x2u
#define HK_PACK_VOICE_TONE_LOW_SHELF 0x4u
#define HK_PACK_VOICE_TONE_HIGH_SHELF 0x8u
#define HK_PACK_VOICE_TONE_SECTIONS (HK_PACK_VOICE_TONE_LOWPASS | HK_PACK_VOICE_TONE_HIGHPASS | HK_PACK_VOICE_TONE_LOW_SHELF | HK_PACK_VOICE_TONE_HIGH_SHELF)

typedef struct HkPackVoiceRecipeBinding {
    HkPackObjectId bindingId;
} HkPackVoiceRecipeBinding;
HK_PACK_SIZE(HkPackVoiceRecipeBinding, 16);
HK_PACK_OFFSET(HkPackVoiceRecipeBinding, bindingId, 0);

typedef struct HkPackVoiceRecipeHeader {
    HkPackObjectId playableId;
    uint32_t installCount;
    uint32_t bindingCount;
} HkPackVoiceRecipeHeader;
HK_PACK_SIZE(HkPackVoiceRecipeHeader, 24);
HK_PACK_OFFSET(HkPackVoiceRecipeHeader, playableId, 0);
HK_PACK_OFFSET(HkPackVoiceRecipeHeader, installCount, 16);
HK_PACK_OFFSET(HkPackVoiceRecipeHeader, bindingCount, 20);

typedef struct HkPackVoiceRecipeInstall {
    uint32_t slot;
    uint32_t effectType;
    uint32_t descriptorVintage;
    uint32_t bypass;
    uint32_t paramCount;
    uint32_t paramBytes;
    uint32_t reserved24;
    uint32_t reserved28;
    float params[HK_PACK_VOICE_RECIPE_MAX_PARAMS];
    HkPackObjectId effectId;
} HkPackVoiceRecipeInstall;
HK_PACK_SIZE(HkPackVoiceRecipeInstall, 304);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, slot, 0);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, effectType, 4);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, descriptorVintage, 8);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, bypass, 12);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, paramCount, 16);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, paramBytes, 20);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, reserved24, 24);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, reserved28, 28);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, params, 32);
HK_PACK_OFFSET(HkPackVoiceRecipeInstall, effectId, 288);

typedef struct HkPackVoiceRecipePlayback {
    uint32_t present;
    uint32_t loopCount;
} HkPackVoiceRecipePlayback;
HK_PACK_SIZE(HkPackVoiceRecipePlayback, 8);
HK_PACK_OFFSET(HkPackVoiceRecipePlayback, present, 0);
HK_PACK_OFFSET(HkPackVoiceRecipePlayback, loopCount, 4);

typedef struct HkPackVoiceRecipeSpatial {
    HkPackObjectId attenuationId;
    float distanceScale;
    uint32_t reserved20;
} HkPackVoiceRecipeSpatial;
HK_PACK_SIZE(HkPackVoiceRecipeSpatial, 24);
HK_PACK_OFFSET(HkPackVoiceRecipeSpatial, attenuationId, 0);
HK_PACK_OFFSET(HkPackVoiceRecipeSpatial, distanceScale, 16);
HK_PACK_OFFSET(HkPackVoiceRecipeSpatial, reserved20, 20);

typedef struct HkPackVoiceRecipeTone {
    uint32_t present;
    uint32_t sectionEnableMask;
    float lowpassFrequencyHz;
    float lowpassQ;
    float highpassFrequencyHz;
    float highpassQ;
    float lowShelfFrequencyHz;
    float lowShelfQ;
    float lowShelfGainDb;
    float highShelfFrequencyHz;
    float highShelfQ;
    float highShelfGainDb;
} HkPackVoiceRecipeTone;
HK_PACK_SIZE(HkPackVoiceRecipeTone, 48);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, present, 0);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, sectionEnableMask, 4);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, lowpassFrequencyHz, 8);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, lowpassQ, 12);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, highpassFrequencyHz, 16);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, highpassQ, 20);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, lowShelfFrequencyHz, 24);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, lowShelfQ, 28);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, lowShelfGainDb, 32);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, highShelfFrequencyHz, 36);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, highShelfQ, 40);
HK_PACK_OFFSET(HkPackVoiceRecipeTone, highShelfGainDb, 44);

#endif
